/* F11: курсор и заставка через собранную d3d11.dll (импорт этого exe перехвачен).
 *   test_cursor.exe <папка с d3d11.dll и ui4k.ini> <num> <den>
 * Курсор не устанавливается (SetCursor не вызываем) — тест не трогает рабочий стол. Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int K_NUM, K_DEN;
static int up(int v) { return (2 * v * K_NUM + K_DEN) / (2 * K_DEN); }

static void icon_size(HICON h, int *w, int *hh, DWORD *hx, DWORD *hy, HBITMAP *color)
{
    ICONINFO ii;
    *w = *hh = 0;
    if (!GetIconInfo(h, &ii)) return;
    BITMAP bm;
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    *w = bm.bmWidth;
    *hh = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    *hx = ii.xHotspot; *hy = ii.yHotspot;
    if (color) *color = ii.hbmColor; else if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) { printf("usage: test_cursor <dir> <num> <den>\nFAIL\n"); return 2; }
    K_NUM = atoi(argv[2]); K_DEN = atoi(argv[3]);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    if (!LoadLibraryA(path)) { printf("LoadLibrary failed\nFAIL\n"); return 1; }

    /* 1. цветной курсор 32x32 с альфой: левая половина непрозрачная красная, правая прозрачная */
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    DWORD *px = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) px[y * 32 + x] = x < 16 ? 0xFFFF0000 : 0x00000000;
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    ICONINFO ii = {FALSE, 5, 7, mask, color};
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    CHECK(c != NULL, "CreateIconIndirect");
    int w, h;
    DWORD hx, hy;
    HBITMAP got = NULL;
    icon_size(c, &w, &h, &hx, &hy, &got);
    CHECK(w == up(32) && h == up(32), "color cursor %dx%d, want %d", w, h, up(32));
    CHECK(hx == (DWORD)up(5) && hy == (DWORD)up(7), "hotspot %lu,%lu want %d,%d", hx, hy, up(5), up(7));
    if (got) {
        int W = up(32);
        DWORD *o = (DWORD *)malloc((size_t)W * W * 4);
        bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -W;
        GetDIBits(dc, got, 0, W, o, &bi, DIB_RGB_COLORS);
        DWORD l = o[(W / 2) * W + 2], r = o[(W / 2) * W + W - 3];
        CHECK((l >> 24) == 0xFF && ((l >> 16) & 255) == 0xFF && (r >> 24) == 0, "pixels: left %08lX right %08lX", l, r);
        /* на границе альфа плавно (дробный) или ступенькой (целый); цвет не темнеет от прозрачного соседа */
        int edge = up(16);
        DWORD e = o[(W / 2) * W + edge - 1];
        CHECK(((e >> 16) & 255) == 0xFF || (e >> 24) == 0, "edge pixel colour kept %08lX", e);
        free(o);
        DeleteObject(got);
    }
    DestroyCursor(c);

    /* 2. монохромный 32x32 (маска 32x64): высота маски ровно 2 x увеличенной половины */
    HBITMAP mono = CreateBitmap(32, 64, 1, 1, NULL);
    ICONINFO mi = {FALSE, 0, 0, mono, NULL};
    c = (HCURSOR)CreateIconIndirect(&mi);
    icon_size(c, &w, &h, &hx, &hy, NULL);
    CHECK(w == up(32) && h == up(32), "mono cursor %dx%d", w, h);
    DestroyCursor(c);

    /* 3. LoadImageW «размер из файла»: системная стрелка ×k */
    typedef HANDLE(WINAPI *PFN_LI)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
    PFN_LI real_li = (PFN_LI)GetProcAddress(GetModuleHandleA("user32.dll"), "LoadImageW");
    HANDLE a0 = real_li(NULL, (LPCWSTR)IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED);
    int w0, h0;
    icon_size((HICON)a0, &w0, &h0, &hx, &hy, NULL);
    HANDLE a1 = LoadImageW(NULL, (LPCWSTR)IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED);
    icon_size((HICON)a1, &w, &h, &hx, &hy, NULL);
    CHECK(w == up(w0) && h == up(h0), "LoadImageW arrow %dx%d, real %dx%d", w, h, w0, h0);
    if (a1 && a1 != a0) DestroyCursor((HCURSOR)a1);

    /* 4. заставка: DC окна класса *SPLASH* получает мировое преобразование ×k, у обычного окна — нет */
    WNDCLASSEXW wc = {sizeof wc, 0, proc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"TEST_SPLASH_WND", NULL};
    RegisterClassExW(&wc);
    HWND sw = CreateWindowExW(0, L"TEST_SPLASH_WND", L"s", WS_POPUP, 0, 0, 200, 100, NULL, NULL, NULL, NULL);
    HDC sdc = GetDC(sw);
    XFORM x = {0};
    GetWorldTransform(sdc, &x);
    CHECK(x.eM11 == (float)K_NUM / K_DEN && GetGraphicsMode(sdc) == GM_ADVANCED, "splash transform %g", x.eM11);
    ReleaseDC(sw, sdc);
    HDC sdc2 = GetDC(sw);
    GetWorldTransform(sdc2, &x);
    ReleaseDC(sw, sdc2);
    WNDCLASSEXW wc2 = {sizeof wc2, 0, proc, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"TEST_PLAIN", NULL};
    RegisterClassExW(&wc2);
    HWND pw = CreateWindowExW(0, L"TEST_PLAIN", L"p", WS_POPUP, 0, 0, 200, 100, NULL, NULL, NULL, NULL);
    HDC pdc = GetDC(pw);
    CHECK(GetGraphicsMode(pdc) != GM_ADVANCED, "plain window untouched");
    ReleaseDC(pw, pdc);
    DestroyWindow(sw); DestroyWindow(pw);
    ReleaseDC(NULL, dc);

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
