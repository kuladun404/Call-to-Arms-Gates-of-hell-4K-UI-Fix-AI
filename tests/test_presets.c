/* L8: строки «мир + масштаб» в списке разрешений (modes.c presets) через собранную DLL на WARP.
 *   test_presets.exe <папка с d3d11.dll и ui4k.ini> [--table]
 * --table (F13, ui4k.ini test_monitor=WxH): только таблица строк для такого монитора — печать и проверки.
 * --two / --two21 (test_monitor + test_monitor2): мониторы 4K и 21:9 — в списке строки монитора игры и одна строка-переход
 *   на другой; переход переносит окно игры туда (--two21: в профиле строка 21:9 — монитор игры 21:9).
 * Таблицу строк берём из ui4k.log (log=2: "  preset WxH: interface a/b world RxS"), монитор — из "presets: monitor".
 * Проверки: EnumDisplaySettingsW отдаёт строки таблицы; создание swapchain строкой A -> back buffer родной, текстура
 * сцены = мир A; ResizeTarget + ResizeBuffers строкой B -> снова родной back buffer, сцена = мир B; подписи в пакете.
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_log[1 << 20];
static const char *dir;
static void read_log(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\ui4k.log", dir);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(g_log, 1, sizeof g_log - 1, f) : 0;
    if (f) fclose(f);
    g_log[n] = 0;
}

typedef struct { unsigned lw, lh, kn, kd, rw, rh; double lab; unsigned mw, mh; } P;   /* mw x mh — монитор строки */

static unsigned upr(unsigned v, unsigned n, unsigned d) { return (2 * v * n + d) / (2 * d); }
static int cursor_w(HCURSOR h)
{
    ICONINFO ii;
    if (!h || !GetIconInfo(h, &ii)) return 0;
    BITMAP bm = {0};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}

/* мониторы 4K + 21:9 (17.11): в списке — строки монитора игры (его пропорции) и одна строка-переход на другой монитор;
 * она переносит окно игры туда, строка своего монитора — обратно. Монитор игры — по строке в профиле игры (--two: строки
 * в профиле нет — основной 4K; --two21: в профиле 1720x720 — монитор 21:9, второй справа от основного) */
static int two_monitors(HMODULE m, const P *ps, int n, unsigned mw, unsigned mh)
{
    int own = 0, ndoor = 0, door = -1, b = -1;
    for (int i = 0; i < n; i++) {
        if (ps[i].mw == mw && ps[i].mh == mh) {
            own++;
            if (b < 0 || ps[i].lab == 2) b = i;
            CHECK((unsigned long long)ps[i].lw * mh == (unsigned long long)ps[i].lh * mw, "row %ux%u has the proportions of %ux%u",
                  ps[i].lw, ps[i].lh, mw, mh);
        } else { ndoor++; door = i; }
    }
    int main4k = mw == 3840 && mh == 2160;
    unsigned ow = main4k ? 3440 : 3840, oh = main4k ? 1440 : 2160;   /* другой монитор */
    LONG gx = main4k ? 0 : 3840, ox = main4k ? 3840 : 0;              /* второй — справа от основного 3840x2160 */
    printf("game monitor %ux%u: %d rows; entries of the other monitor: %d (%ux%u)\n", mw, mh, own, ndoor,
           door >= 0 ? ps[door].lw : 0, door >= 0 ? ps[door].lh : 0);
    CHECK(main4k || (mw == 3440 && mh == 1440), "game monitor %ux%u", mw, mh);
    CHECK(own >= 4, "rows of the game monitor (%d)", own);
    CHECK(ndoor == 1, "exactly one entry of the other monitor (%d)", ndoor);
    if (fails) { printf("FAIL %d\n", fails); return 1; }
    CHECK(ps[door].mw == ow && ps[door].mh == oh, "that entry is for %ux%u (%ux%u)", ow, oh, ps[door].mw, ps[door].mh);
    /* переход — «как на Full HD» другого монитора: 21:9 — 2580x1080 x4/3, 4K — 1920x1080 x2 */
    CHECK(ps[door].lh == 1080 && ps[door].lw == (main4k ? 2580u : 1920u) && ps[door].rw == ow,
          "entry to the other monitor: its 'as on Full HD' row (%ux%u world %ux%u)", ps[door].lw, ps[door].lh, ps[door].rw, ps[door].rh);
    if (!main4k) {
        int fhd = 0;
        for (int i = 0; i < n; i++) fhd |= ps[i].mw == mw && ps[i].lw == 2580 && ps[i].lh == 1080 && ps[i].kn * 3 == ps[i].kd * 4;
        CHECK(fhd, "21:9: x1.333 entry 2580x1080 (interface as on Full HD)");
        for (int i = 0; i < n; i++)
            if (ps[i].mw == mw) printf("  %5ux%-5u world %ux%u  x%-5.4g real x%.3f\n", ps[i].lw, ps[i].lh, ps[i].rw, ps[i].rh,
                                       ps[i].lab, (double)ps[i].kn / ps[i].kd);
    }
    typedef BOOL(WINAPI *PFN_GWR)(HWND, RECT *);
    PFN_GWR real_gwr = (PFN_GWR)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowRect");
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k two monitors", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    /* игра стартует строкой своего монитора (из профиля), окно создано на основном */
    DXGI_SWAP_CHAIN_DESC sd = {{ps[b].lw, ps[b].lh, {0, 0}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    if (FAILED(cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);   /* мод помечает буфер логическим размером */
    if (bb) ID3D11Texture2D_Release(bb);
    RECT r;
    real_gwr(wnd, &r);
    printf("start with %ux%u: window at %ld,%ld\n", ps[b].lw, ps[b].lh, r.left, r.top);
    CHECK(r.left == gx && r.top == 0, "start: window on the game monitor (%ld,%ld), want %ld,0", r.left, r.top, gx);
    read_log();
    char want[200];
    snprintf(want, sizeof want, ": %ux%u marked as logical %ux%u", mw, mh, ps[b].lw, ps[b].lh);
    CHECK(strstr(g_log, want), "back buffer = game monitor (%s)", want);
    /* «Применить» строкой-переходом — окно на другой монитор */
    DXGI_MODE_DESC md = {ps[door].lw, ps[door].lh, {0, 0}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0};
    IDXGISwapChain_ResizeTarget(sc, &md);
    real_gwr(wnd, &r);
    printf("entry %ux%u of the other monitor: window at %ld,%ld\n", ps[door].lw, ps[door].lh, r.left, r.top);
    CHECK(r.left == ox && r.top == 0, "window moved to the other monitor (%ld,%ld), want %ld,0", r.left, r.top, ox);
    /* строкой своего монитора — обратно */
    md.Width = ps[b].lw; md.Height = ps[b].lh;
    IDXGISwapChain_ResizeTarget(sc, &md);
    real_gwr(wnd, &r);
    CHECK(r.left == gx && r.top == 0, "own entry again: window back (%ld,%ld), want %ld,0", r.left, r.top, gx);
    /* другая строка того же монитора — окно не двигается */
    int c = -1;
    for (int i = 0; i < n && c < 0; i++) if (i != b && ps[i].mw == mw && ps[i].mh == mh) c = i;
    /* настоящей SetWindowPos (у теста она перехвачена модом и умножает на масштаб): окно остаётся на мониторе игры, что
     * бы ни было подключено к компьютеру */
    typedef BOOL(WINAPI *PFN_SWP)(HWND, HWND, int, int, int, int, UINT);
    PFN_SWP real_swp = (PFN_SWP)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowPos");
    real_swp(wnd, NULL, gx + 100, 50, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    real_gwr(wnd, &r);
    LONG x0 = r.left, y0 = r.top;
    md.Width = ps[c].lw; md.Height = ps[c].lh;
    IDXGISwapChain_ResizeTarget(sc, &md);
    real_gwr(wnd, &r);
    CHECK(r.left == x0 && r.top == y0, "entry of the same monitor: window stays (%ld,%ld -> %ld,%ld)", x0, y0, r.left, r.top);
    ID3D11DeviceContext_Release(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);
    /* подпись перехода: «<мир> ×<k>  — другой монитор» */
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k_labels.pak", dir);
    FILE *f = fopen(path, "rb");
    static char pak[1 << 16];
    size_t pn = f ? fread(pak, 1, sizeof pak, f) : 0;
    if (f) fclose(f);
    snprintf(want, sizeof want, "%u\xC3\x97%u  \xC3\x97", ps[door].rw, ps[door].rh);
    const char *other = "\xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD0\xBE\xD0\xB9 \xD0\xBC\xD0\xBE\xD0\xBD\xD0\xB8\xD1\x82\xD0\xBE\xD1\x80";
    int lab = 0;
    for (size_t i = 0; i + strlen(want) + 40 < pn && !lab; i++)
        if (!memcmp(pak + i, want, strlen(want))) {
            const char *e = memchr(pak + i, '"', 80);
            if (e) for (const char *q = pak + i; q + strlen(other) <= e && !lab; q++) lab = !memcmp(q, other, strlen(other));
        }
    CHECK(lab, "labels pak: entry %ux%u is labelled 'other monitor'", ps[door].lw, ps[door].lh);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage\nFAIL\n"); return 2; }
    dir = argv[1];
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary\nFAIL\n"); return 1; }
    read_log();
    unsigned mw = 0, mh = 0;
    const char *s = strstr(g_log, "presets: monitor ");
    if (s) sscanf(s, "presets: monitor %ux%u", &mw, &mh);
    CHECK(mw && mh, "presets line in log");
    static P ps[64];
    int n = 0;
    for (s = g_log; (s = strstr(s, "  preset ")) && n < 64; s++)
        if (sscanf(s, "  preset %ux%u: interface %u/%u world %ux%u label %lf monitor %ux%u", &ps[n].lw, &ps[n].lh, &ps[n].kn,
                   &ps[n].kd, &ps[n].rw, &ps[n].rh, &ps[n].lab, &ps[n].mw, &ps[n].mh) >= 7) {
            if (!ps[n].mw) { ps[n].mw = mw; ps[n].mh = mh; }
            n++;
        }
    int table = argc > 2 && !strcmp(argv[2], "--table"), two = argc > 2 && (!strcmp(argv[2], "--two") || !strcmp(argv[2], "--two21"));
    if (argc > 2 && !strcmp(argv[2], "--dsr")) {
        /* С5: в списке Windows есть режим 7680x4320 (как при NVIDIA DSR). Монитор — рабочий стол, а не он. */
        typedef BOOL(WINAPI *PFN_EDS)(LPCWSTR, DWORD, DEVMODEW *);
        PFN_EDS real_eds = (PFN_EDS)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "EnumDisplaySettingsW");
        DEVMODEW dd;
        ZeroMemory(&dd, sizeof dd);
        dd.dmSize = sizeof dd;
        real_eds(NULL, ENUM_REGISTRY_SETTINGS, &dd);
        printf("desktop %lux%lu, mod's monitor %ux%u\n", dd.dmPelsWidth, dd.dmPelsHeight, mw, mh);
        CHECK(mw == dd.dmPelsWidth && mh == dd.dmPelsHeight, "monitor must be the desktop mode %lux%lu, not %ux%u", dd.dmPelsWidth,
              dd.dmPelsHeight, mw, mh);
        CHECK(strstr(g_log, "largest mode in the Windows list 7680x4320"), "the larger mode is seen and reported in the log");
        CHECK(ps[n - 1].lw == mw && ps[n - 1].lh == mh, "last row is the monitor itself (%ux%u)", ps[n - 1].lw, ps[n - 1].lh);
        DEVMODEW dm2;
        int rows = 0;
        for (DWORD i = 0; i < 4096; i++) {
            ZeroMemory(&dm2, sizeof dm2);
            dm2.dmSize = sizeof dm2;
            if (!EnumDisplaySettingsW(NULL, i, &dm2)) break;
            CHECK(dm2.dmPelsWidth <= mw && dm2.dmPelsHeight <= mh, "game list has %lux%lu - larger than the monitor", dm2.dmPelsWidth,
                  dm2.dmPelsHeight);
            rows++;
        }
        CHECK(rows >= n, "game list is not empty (%d rows for %d presets)", rows, n);
        printf(fails ? "FAIL %d\n" : "PASS\n", fails);
        return fails != 0;
    }
    CHECK(n >= (table ? 2 : 4), "at least %d presets (%d)", table ? 2 : 4, n);   /* 2560x1080: шаг 10%% — строк мало */
    if (fails) { printf("FAIL %d\n", fails); return 1; }
    for (int i = 0; i < n; i++) {
        CHECK((unsigned long long)ps[i].lw * ps[i].kn == (unsigned long long)ps[i].mw * ps[i].kd &&
              (unsigned long long)ps[i].lh * ps[i].kn == (unsigned long long)ps[i].mh * ps[i].kd,
              "preset %ux%u x %u/%u is exactly its monitor %ux%u", ps[i].lw, ps[i].lh, ps[i].kn, ps[i].kd, ps[i].mw, ps[i].mh);
        for (int j = 0; j < i; j++) CHECK(ps[i].lw != ps[j].lw || ps[i].lh != ps[j].lh, "unique %ux%u", ps[i].lw, ps[i].lh);
        CHECK(ps[i].lw % 4 == 0 && ps[i].lh % 4 == 0, "preset %ux%u divides by 4 (whole half / quarter screens)", ps[i].lw,
              ps[i].lh);
        /* по возрастанию размера (игра всё равно сортирует сама) */
        if (i) CHECK((unsigned long long)ps[i].lw * ps[i].lh > (unsigned long long)ps[i - 1].lw * ps[i - 1].lh,
                     "row %d (%ux%u) after row %d (%ux%u): ascending", i, ps[i].lw, ps[i].lh, i - 1, ps[i - 1].lw, ps[i - 1].lh);
    }
    CHECK(ps[n - 1].lw == ps[n - 1].mw && ps[n - 1].lh == ps[n - 1].mh, "last row is a monitor itself x1 (%ux%u)", ps[n - 1].lw,
          ps[n - 1].lh);
    int worlds[3] = {0}, nwd = 0;
    for (int i = 0; i < n; i++) {
        /* настоящий масштаб не дальше 6% от подписи */
        double real = (double)ps[i].kn / ps[i].kd, dev = real / ps[i].lab - 1;
        CHECK(dev <= 0.0601 && dev >= -0.0601, "row %ux%u: real x%.3f vs label x%.4g (%.1f%%)", ps[i].lw, ps[i].lh, real,
              ps[i].lab, dev * 100);
        /* у родного мира точный масштаб, если монитор делится на шаг ровно */
        if (ps[i].rw == ps[i].mw && ps[i].lab == 1) CHECK(real == 1, "native x1 row is exact");
        int seenw = 0;
        for (int q = 0; q < nwd; q++) seenw |= worlds[q] == (int)ps[i].rw;
        if (!seenw && nwd < 3) worlds[nwd++] = (int)ps[i].rw;
    }
    if (table) {
        printf("monitor %ux%u: %d rows, %d worlds\n", mw, mh, n, nwd);
        for (int i = 0; i < n; i++)
            printf("  %5ux%-5u world %ux%u  x%-5.4g real x%.3f (%+.1f%%)\n", ps[i].lw, ps[i].lh, ps[i].rw, ps[i].rh, ps[i].lab,
                   (double)ps[i].kn / ps[i].kd, ((double)ps[i].kn / ps[i].kd / ps[i].lab - 1) * 100);
        printf(fails ? "FAIL %d\n" : "PASS\n", fails);
        return fails != 0;
    }

    /* список режимов игры: строки таблицы */
    int seen = 0;
    DEVMODEW dm;
    for (DWORD i = 0; i < 4096; i++) {
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(NULL, i, &dm)) break;
        int ok = 0;
        for (int j = 0; j < n; j++) ok |= dm.dmPelsWidth == ps[j].lw && dm.dmPelsHeight == ps[j].lh;
        CHECK(ok, "mode %lux%lu is a preset", dm.dmPelsWidth, dm.dmPelsHeight);
        seen++;
    }
    CHECK(seen >= n && seen % n == 0, "EnumDisplaySettingsW: %d entries for %d presets", seen, n);
    if (two) return two_monitors(m, ps, n, mw, mh);

    /* A — строка с миром не родного размера, B — другая строка с другим миром и масштабом */
    int a = -1, b = -1;
    for (int i = 0; i < n && a < 0; i++) if (ps[i].rw != mw) a = i;
    for (int i = 0; i < n && b < 0; i++)
        if (i != a && ps[i].rw != ps[a].rw && ps[i].rw != ps[i].lw && (ps[i].kn * ps[a].kd != ps[a].kn * ps[i].kd)) b = i;
    CHECK(a >= 0 && b >= 0, "pick presets A/B");
    if (fails) { printf("FAIL %d\n", fails); return 1; }
    printf("A %ux%u (world %ux%u), B %ux%u (world %ux%u)\n", ps[a].lw, ps[a].lh, ps[a].rw, ps[a].rh, ps[b].lw, ps[b].lh,
           ps[b].rw, ps[b].rh);

    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k presets", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {{ps[a].lw, ps[a].lh, {0, 0}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    if (FAILED(cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    D3D11_TEXTURE2D_DESC td = {ps[a].lw, ps[a].lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *scene = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);
    read_log();
    char want[160];
    snprintf(want, sizeof want, ": %ux%u marked as logical %ux%u", mw, mh, ps[a].lw, ps[a].lh);
    CHECK(strstr(g_log, want), "A: back buffer native (%s)", want);
    snprintf(want, sizeof want, "tex2D scene: %ux%u -> %ux%u fmt=10", ps[a].lw, ps[a].lh, ps[a].rw, ps[a].rh);
    CHECK(strstr(g_log, want), "A: scene texture = world (%s)", want);
    snprintf(want, sizeof want, "preset %ux%u: interface", ps[a].lw, ps[a].lh);
    CHECK(strstr(g_log, want), "A: preset applied (%s)", want);
    if (scene) ID3D11Texture2D_Release(scene);
    if (bb) ID3D11Texture2D_Release(bb);

    /* курсор 32x32, сделанный при строке A (игра в меню делает его один раз) */
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    DWORD *px = NULL;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (px) for (int i = 0; i < 32 * 32; i++) px[i] = 0xFFFF0000;
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    ICONINFO ii = {FALSE, 4, 4, mask, color};
    HCURSOR cur = (HCURSOR)CreateIconIndirect(&ii);
    CHECK(cur != NULL, "cursor at A");
    CHECK(cursor_w(cur) == upr(32, ps[a].kn, ps[a].kd), "cursor at A: %d px, want %u", cursor_w(cur), upr(32, ps[a].kn, ps[a].kd));
    SetCursor(cur);
    CHECK(GetCursor() == cur, "same scale: the game's own cursor is set");

    /* «Применить» в игре: ResizeTarget строкой B, потом ResizeBuffers */
    DXGI_MODE_DESC md = {ps[b].lw, ps[b].lh, {0, 0}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0};
    IDXGISwapChain_ResizeTarget(sc, &md);
    /* как в игре: физический экран тот же, ResizeBuffers ещё не было — буфер уже должен быть нового логического размера */
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    D3D11_TEXTURE2D_DESC bd = {0};
    if (bb) { ID3D11Texture2D_GetDesc(bb, &bd); ID3D11Texture2D_Release(bb); bb = NULL; }
    CHECK(bd.Width == ps[b].lw && bd.Height == ps[b].lh, "after ResizeTarget only: back buffer %ux%u, want %ux%u",
          bd.Width, bd.Height, ps[b].lw, ps[b].lh);
    HRESULT hr = IDXGISwapChain_ResizeBuffers(sc, 1, ps[b].lw, ps[b].lh, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers B 0x%08lx", hr);
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    td.Width = ps[b].lw; td.Height = ps[b].lh;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);
    read_log();
    snprintf(want, sizeof want, ": %ux%u marked as logical %ux%u", mw, mh, ps[b].lw, ps[b].lh);
    CHECK(strstr(g_log, want), "B: back buffer native (%s)", want);
    snprintf(want, sizeof want, "tex2D scene: %ux%u -> %ux%u fmt=10", ps[b].lw, ps[b].lh, ps[b].rw, ps[b].rh);
    CHECK(strstr(g_log, want), "B: scene texture = world (%s)", want);
    snprintf(want, sizeof want, "preset %ux%u: interface", ps[b].lw, ps[b].lh);
    CHECK(strstr(g_log, want), "B: preset applied (%s)", want);
    /* игра сама курсор заново не ставит: мод ставит копию нового масштаба на ближайшем сообщении окна */
    SendMessageW(wnd, WM_SIZE, 0, MAKELPARAM(ps[b].lw, ps[b].lh));
    HCURSOR now = GetCursor();
    CHECK(now && now != cur && cursor_w(now) == upr(32, ps[b].kn, ps[b].kd),
          "cursor right after switching to B, no SetCursor from the game: %d px, want %u", cursor_w(now),
          upr(32, ps[b].kn, ps[b].kd));
    /* и когда игра ставит тот же курсор сама */
    SetCursor(cur);
    now = GetCursor();
    CHECK(now && now != cur && cursor_w(now) == upr(32, ps[b].kn, ps[b].kd),
          "cursor after switching to B: %d px, want %u (re-made from the original picture)", cursor_w(now),
          upr(32, ps[b].kn, ps[b].kd));
    SetCursor(NULL);
    DestroyIcon(cur);
    DeleteObject(color); DeleteObject(mask);
    if (scene) ID3D11Texture2D_Release(scene);
    if (bb) ID3D11Texture2D_Release(bb);
    ID3D11DeviceContext_Release(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);

    /* подписи: пакет рядом с DLL, ключ строки A и знак × */
    snprintf(path, sizeof path, "%s\\ui4k_labels.pak", dir);
    FILE *f = fopen(path, "rb");
    static char pak[1 << 16];
    size_t pn = f ? fread(pak, 1, sizeof pak, f) : 0;
    if (f) fclose(f);
    snprintf(want, sizeof want, "options/video/resolution#/%ux%u", ps[a].lw, ps[a].lh);
    int key = 0;
    for (size_t i = 0; i + strlen(want) < pn; i++) if (!memcmp(pak + i, want, strlen(want))) key = 1;
    snprintf(want, sizeof want, "%u\xC3\x97%u  \xC3\x97", ps[a].rw, ps[a].rh);
    int txt = 0;
    for (size_t i = 0; i + strlen(want) < pn; i++) if (!memcmp(pak + i, want, strlen(want))) txt = 1;
    CHECK(key && txt, "labels pak: key %d text %d", key, txt);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
