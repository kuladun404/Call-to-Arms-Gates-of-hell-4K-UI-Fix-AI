/* Граница «процессор <-> видеокарта» и вызовы с несходящимися настоящими размерами — через собранную d3d11.dll (WARP, x1.5).
 *   test_boundary.exe <папка с d3d11.dll и ui4k.ini> upload|readback|copy|resolve|targets
 * Воспроизводит падение игры с модом (файл падения 16.11: драйвер читает за концом памяти игры, когда игра загружает
 * картинку логического размера в текстуру, которую мод увеличил) и соседние случаи того же рода:
 *   upload   — UpdateSubresource в увеличенную текстуру: память с картинкой кончается ровно на границе страницы,
 *              дальше закрытая страница. Чтение за концом = падение (ловим и печатаем CRASH).
 *   readback — «читаемая» (staging) текстура размера экрана остаётся логической; копия увеличенной в неё уменьшается,
 *              копия из неё в увеличенную растягивается (скриншот, картинка сохранения).
 *   copy     — копии между текстурами разного настоящего размера: содержимое пересчитано, а не «угол».
 *   resolve  — сведение выборок MSAA в увеличенную пару и обратно.
 *   targets  — цель и глубина разного настоящего размера: глубина отвязана, вызов с несходящимися размерами не уходит в D3D.
 *   loglimit — С6: на log=1 лог не растёт на строку с каждой созданной текстурой.
 *   profile  — HDR и MSAA игра меняет без перезапуска: мод перечитывает профиль, когда игра пересоздаёт экранные цели
 *              (ini: presets=0, render=50, test_profiles=<папка>\prof; начальный options.set пишет build.py).
 *              MSAA размер мира не меняет (П7, Г1); profile_safe — то же при msaa_safe=1: с MSAA мир = интерфейс.
 *   hdrmsaa  — HDR включён + MSAA x4 + мир меньше экрана (ini: game_hdr=1, game_aa=4, presets=0, render=50): картинки
 *              мира игры, в том числе многовыборочные, — в размере мира; глубина остаётся привязанной; сведение выборок
 *              уходит в D3D как есть.
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                     UINT, UINT, ID3DBlob **, ID3DBlob **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *sc;
static const char *dir;
static const UINT RED = 0xFF0000FF, BLUE = 0xFFFF0000, GREEN = 0xFF00FF00, WHITE = 0xFFFFFFFF;
static const UINT RT = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
#define FMT DXGI_FORMAT_R8G8B8A8_UNORM

static ID3D11DeviceContext *real_of(ID3D11DeviceContext *w) { return *(ID3D11DeviceContext **)((char *)w + 8); }

/* ---- память, за концом которой закрытая страница ---- */
static const BYTE *g_guard[16];
static int g_nguard;
static void *guard_alloc(size_t size)
{
    size_t pages = (size + 4095) / 4096;
    BYTE *p = (BYTE *)VirtualAlloc(NULL, (pages + 1) * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(p + pages * 4096, 4096, PAGE_NOACCESS, &old);
    if (g_nguard < 16) g_guard[g_nguard++] = p + pages * 4096;
    return p + pages * 4096 - size;   /* конец картинки = начало закрытой страницы */
}

static LONG CALLBACK on_crash(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const BYTE *a = (const BYTE *)e->ExceptionRecord->ExceptionInformation[1];
    int past = 0;
    for (int i = 0; i < g_nguard; i++) past |= a >= g_guard[i] && a < g_guard[i] + 4096;
    printf("CRASH: access violation %s %p%s\nFAIL\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
           (void *)a, past ? " - right past the end of the game's pixel buffer (the game crashes the same way)" : "");
    ExitProcess(3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- текстуры ---- */
static ID3D11Texture2D *tex_ex(UINT w, UINT h, DXGI_FORMAT f, UINT mips, UINT samples, D3D11_USAGE u, UINT bind, UINT cpu,
                               const void *fill)
{
    D3D11_TEXTURE2D_DESC d = {w, h, mips, 1, f, {samples, 0}, u, bind, cpu, 0};
    D3D11_SUBRESOURCE_DATA sd = {fill, w * 4, 0};
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, fill ? &sd : NULL, &t);
    CHECK(SUCCEEDED(hr) && t, "create %ux%u samples=%u hr=0x%08lx", w, h, samples, hr);
    return t;
}
static ID3D11Texture2D *tex(UINT w, UINT h, UINT bind, const void *fill)
{
    return tex_ex(w, h, FMT, 1, 1, D3D11_USAGE_DEFAULT, bind, 0, fill);
}

static void real_size(void *t, UINT *w, UINT *h)
{
    IDXGISurface *s = NULL;
    *w = *h = 0;
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)t, &IID_IDXGISurface, (void **)&s)) && s) {
        DXGI_SURFACE_DESC d;
        IDXGISurface_GetDesc(s, &d);
        *w = d.Width; *h = d.Height;
        IDXGISurface_Release(s);
    }
}

/* настоящие пиксели подресурса (мимо хуков мода). Читаемая текстура на 1 больше: такой размер мод не трогает */
static UINT *read_sub(ID3D11Texture2D *t, UINT sub, UINT pw, UINT ph)
{
    ID3D11Texture2D *st = tex_ex(pw + 1, ph + 1, FMT, 1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL);
    UINT *px = (UINT *)calloc((size_t)pw * ph, 4);
    if (!st) return px;
    UINT rw, rh;
    real_size(st, &rw, &rh);
    CHECK(rw == pw + 1 && rh == ph + 1, "test helper: staging %ux%u became %ux%u", pw + 1, ph + 1, rw, rh);
    D3D11_BOX b = {0, 0, 0, pw, ph, 1};
    ID3D11DeviceContext_CopySubresourceRegion(real_of(ctx), (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)t, sub, &b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(real_of(ctx), (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < ph; y++) memcpy(px + y * pw, (BYTE *)m.pData + y * m.RowPitch, pw * 4);
        ID3D11DeviceContext_Unmap(real_of(ctx), (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return px;
}
static UINT *read(ID3D11Texture2D *t, UINT pw, UINT ph) { return read_sub(t, 0, pw, ph); }

/* четыре четверти: красная | синяя / зелёная | белая */
static UINT quad_color(UINT x, UINT y, UINT w, UINT h) { return y < h / 2 ? (x < w / 2 ? RED : BLUE) : (x < w / 2 ? GREEN : WHITE); }
static void fill_quads(UINT *p, UINT w, UINT h, UINT pitch_px)
{
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) p[y * pitch_px + x] = quad_color(x, y, w, h);
}
/* закрасить настоящие пиксели (мимо хуков) */
static void paint_quads(ID3D11Texture2D *t, UINT pw, UINT ph)
{
    UINT *p = (UINT *)malloc((size_t)pw * ph * 4);
    fill_quads(p, pw, ph, pw);
    ID3D11DeviceContext_UpdateSubresource(real_of(ctx), (ID3D11Resource *)t, 0, NULL, p, pw * 4, 0);
    free(p);
}
/* проверка четвертей в картинке w x h: углы и середины четвертей */
static void check_quads(const UINT *p, UINT w, UINT h, const char *what)
{
    const UINT xs[4] = {0, w / 4, w * 3 / 4, w - 1}, ys[4] = {0, h / 4, h * 3 / 4, h - 1};
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            UINT want = quad_color(xs[i], ys[j], w, h), got = p[ys[j] * w + xs[i]];
            CHECK(got == want, "%s: pixel (%u,%u) of %ux%u = %08X, want %08X", what, xs[i], ys[j], w, h, got, want);
        }
}

static int log_has(const char *needle)
{
    static char buf[1 << 20];
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\ui4k.log", dir);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

static int log_count(const char *needle)
{
    static char buf[1 << 21];
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\ui4k.log", dir);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    int c = 0;
    for (const char *s = buf; (s = strstr(s, needle)); s += strlen(needle)) c++;
    return c;
}

/* ============ loglimit: создание текстур не заливает лог (С6) ============ */
static void t_loglimit(void)
{
    /* 1000 маленьких кэшей интерфейса (9x6 -> 14x9), как подсказки и панели за сессию игры */
    for (int i = 0; i < 1000; i++) {
        ID3D11Texture2D *t = tex(9, 6, RT, NULL);
        if (t) ID3D11Texture2D_Release(t);
    }
    int lines = log_count("tex2D ui_rt:"), marks = log_count("interface cache textures so far");
    printf("1000 textures: %d per-texture lines, %d milestone lines\n", lines, marks);
    CHECK(lines == 200, "log=1: only the first 200 interface-cache textures are logged one by one, got %d", lines);
    CHECK(marks >= 2 && marks <= 3, "log=1: milestone lines after that (256, 512 ...), got %d", marks);
    /* экранные цели — редкие и важные для разбора: пишутся */
    ID3D11Texture2D *s = tex(1280, 720, RT, NULL);
    if (s) ID3D11Texture2D_Release(s);
    CHECK(log_count("tex2D screen: 1280x720 -> 1920x1080") == 1, "screen targets are still logged");
}

/* ============ profile: настройки игры меняются на ходу ============ */
static void put_profile(const char *text)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\prof\\1\\options.set", dir);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
    Sleep(400);   /* мод перечитывает профиль не чаще раза в 300 мс */
}

/* настоящий размер новой экранной цели 1280x720 данного формата */
static UINT made_w(DXGI_FORMAT f)
{
    ID3D11Texture2D *t = tex_ex(1280, 720, f, 1, 1, D3D11_USAGE_DEFAULT, RT, 0, NULL);
    UINT w = 0, h = 0;
    if (t) { real_size(t, &w, &h); ID3D11Texture2D_Release(t); }
    return w;
}

static int g_safe;   /* profile_safe: в ini msaa_safe=1 */

static void t_profile(void)
{
    const UINT aa_w = g_safe ? 1920 : 960;   /* размер картинок мира при включённом MSAA */
    const DXGI_FORMAT F_SCR = FMT, F_HDR = DXGI_FORMAT_R16G16B16A16_FLOAT;   /* формат экрана и формат сцены */
    /* старт: HDR включён, MSAA нет; интерфейс x1.5, мир 50% (x0.75) */
    CHECK(made_w(F_SCR) == 1920 && made_w(F_HDR) == 960, "start (HDR on): screen copy %u (want 1920), scene %u (want 960)",
          made_w(F_SCR), made_w(F_HDR));
    /* игрок выключил HDR: мир рисуется прямо в экран, его копии — в размере мира */
    put_profile("{options {video {hdr 0} {antialiasing none}}}");
    CHECK(made_w(F_SCR) == 960, "HDR switched off in the game: screen copy %u, want 960 (world size)", made_w(F_SCR));
    CHECK(log_has("game settings: HDR is now off"), "HDR change noted in the log");
    /* включил MSAA: размер мира от этого не меняется (Г1). С msaa_safe=1 — безопасный режим, всё в размере интерфейса */
    put_profile("{options {video {hdr 0} {antialiasing \"4x\"}}}");
    CHECK(made_w(F_SCR) == aa_w && made_w(F_HDR) == aa_w, "MSAA switched on: screen copy %u, scene %u, want %u both (%s)",
          made_w(F_SCR), made_w(F_HDR), aa_w, g_safe ? "safe mode" : "world size does not depend on MSAA");
    CHECK(!log_has("safe mode") == !g_safe, "safe mode %s in the log", g_safe ? "announced" : "not mentioned");
    CHECK(log_has("MSAA x4 is now on (game profile)"), "MSAA change noted in the log");
    /* выключил MSAA и включил HDR обратно: как на старте */
    put_profile("{options {video {hdr} {antialiasing none}}}");
    CHECK(made_w(F_SCR) == 1920 && made_w(F_HDR) == 960, "back to HDR on, MSAA off: screen copy %u (want 1920), scene %u (want 960)",
          made_w(F_SCR), made_w(F_HDR));
    CHECK(log_has("MSAA is now off (game profile)") && log_has("game settings: HDR is now on"), "both changes noted in the log");
}

/* ============ upload: UpdateSubresource в увеличенную текстуру ============ */
static void t_upload(void)
{
    /* 1. вся картинка 222x76 (игра считает текстуру такой) -> настоящая 333x114 */
    ID3D11Texture2D *a = tex(222, 76, RT, NULL);
    UINT rw, rh;
    real_size(a, &rw, &rh);
    CHECK(rw == 333 && rh == 114, "enlarged target is %ux%u, want 333x114", rw, rh);
    UINT *img = (UINT *)guard_alloc(222 * 76 * 4);
    fill_quads(img, 222, 76, 222);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)a, 0, NULL, img, 222 * 4, 0);
    ID3D11DeviceContext_Flush(ctx);
    UINT *p = read(a, 333, 114);
    check_quads(p, 333, 114, "upload whole image");
    free(p);
    CHECK(img[0] == RED && img[222 * 76 - 1] == WHITE, "the game's pixels must not be modified");

    /* 2. прямоугольник (10,5)-(60,25) красным -> настоящие (15,8)-(90,38) */
    ID3D11RenderTargetView *va = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)a, NULL, &va);
    const float zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, va, zero);
    UINT *part = (UINT *)guard_alloc(50 * 20 * 4);
    for (int i = 0; i < 50 * 20; i++) part[i] = RED;
    D3D11_BOX b = {10, 5, 0, 60, 25, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)a, 0, &b, part, 50 * 4, 0);
    p = read(a, 333, 114);
    CHECK(p[8 * 333 + 15] == RED && p[37 * 333 + 89] == RED && p[20 * 333 + 50] == RED, "upload box: inside %08X %08X %08X",
          p[8 * 333 + 15], p[37 * 333 + 89], p[20 * 333 + 50]);
    CHECK(p[8 * 333 + 14] == 0 && p[8 * 333 + 90] == 0 && p[7 * 333 + 15] == 0 && p[38 * 333 + 15] == 0,
          "upload box: outside %08X %08X %08X %08X", p[8 * 333 + 14], p[8 * 333 + 90], p[7 * 333 + 15], p[38 * 333 + 15]);
    free(p);

    /* 3. шаг строки больше ширины (выравнивание): последняя строка короче шага — читать ровно ширину */
    UINT *wide = (UINT *)guard_alloc((75 * 256 + 222) * 4);
    fill_quads(wide, 222, 76, 256);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)a, 0, NULL, wide, 256 * 4, 0);
    p = read(a, 333, 114);
    check_quads(p, 333, 114, "upload with row pitch 256 px");
    free(p);

    /* 4. экранная цель 1280x720 -> 1920x1080 */
    ID3D11Texture2D *s = tex(1280, 720, RT, NULL);
    real_size(s, &rw, &rh);
    CHECK(rw == 1920 && rh == 1080, "screen target is %ux%u, want 1920x1080", rw, rh);
    UINT *big = (UINT *)guard_alloc(1280 * 720 * 4);
    fill_quads(big, 1280, 720, 1280);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)s, 0, NULL, big, 1280 * 4, 0);
    p = read(s, 1920, 1080);
    check_quads(p, 1920, 1080, "upload into screen target");
    free(p);

    /* 5. второй уровень детализации: 640x360 (половина экрана) с 2 уровнями, уровень 1 = 320x180 -> 480x270 */
    ID3D11Texture2D *mt = tex_ex(640, 360, FMT, 2, 1, D3D11_USAGE_DEFAULT, RT, 0, NULL);
    UINT *m1 = (UINT *)guard_alloc(320 * 180 * 4);
    fill_quads(m1, 320, 180, 320);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)mt, 1, NULL, m1, 320 * 4, 0);
    p = read_sub(mt, 1, 480, 270);
    check_quads(p, 480, 270, "upload into mip 1");
    free(p);

    /* 6. отложенный контекст (игра рисует 3D в них) */
    ID3D11DeviceContext *dc = NULL;
    ID3D11Device_CreateDeferredContext(dev, 0, &dc);
    if (dc) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, va, zero);
        ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *)a, 0, NULL, img, 222 * 4, 0);
        ID3D11CommandList *cl = NULL;
        ID3D11DeviceContext_FinishCommandList(dc, FALSE, &cl);
        if (cl) { ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE); ID3D11CommandList_Release(cl); }
        p = read(a, 333, 114);
        check_quads(p, 333, 114, "upload on a deferred context");
        free(p);
        ID3D11DeviceContext_Release(dc);
    }

    /* 7. UpdateSubresource1 (ID3D11DeviceContext1) */
    ID3D11DeviceContext1 *c1 = NULL;
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&c1)) && c1) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, va, zero);
        ID3D11DeviceContext1_UpdateSubresource1(c1, (ID3D11Resource *)a, 0, NULL, img, 222 * 4, 0, 0);
        p = read(a, 333, 114);
        check_quads(p, 333, 114, "UpdateSubresource1");
        free(p);
        ID3D11DeviceContext1_Release(c1);
    }

    /* 8. обычная текстура (квадрат 512 — мод её не увеличивает): как было */
    ID3D11Texture2D *pl = tex(512, 512, RT, NULL);
    UINT *sq = (UINT *)guard_alloc(512 * 512 * 4);
    fill_quads(sq, 512, 512, 512);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)pl, 0, NULL, sq, 512 * 4, 0);
    p = read(pl, 512, 512);
    CHECK(!memcmp(p, sq, 512 * 512 * 4), "plain texture upload must stay exact");
    free(p);

    ID3D11RenderTargetView_Release(va);
    ID3D11Texture2D_Release(a); ID3D11Texture2D_Release(s); ID3D11Texture2D_Release(mt); ID3D11Texture2D_Release(pl);
}

/* ============ readback: «читаемые» текстуры остаются логическими ============ */
static void t_readback(void)
{
    ID3D11Texture2D *rt = tex(1280, 720, RT, NULL);   /* экранная цель: настоящая 1920x1080 */
    paint_quads(rt, 1920, 1080);
    ID3D11Texture2D *st = tex_ex(1280, 720, FMT, 1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, NULL);
    UINT rw, rh;
    real_size(st, &rw, &rh);
    CHECK(rw == 1280 && rh == 720, "screen-size staging texture is really %ux%u, want 1280x720 (what the game reads)", rw, rh);

    /* 1. скриншот: вся увеличенная цель -> читаемая; игра читает 720 строк по 1280 точек */
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)rt);
    UINT *got = (UINT *)calloc(1280 * 720, 4);
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m);
    CHECK(SUCCEEDED(hr), "Map staging hr=0x%08lx", hr);
    if (SUCCEEDED(hr)) {
        for (UINT y = 0; y < 720; y++) memcpy(got + y * 1280, (BYTE *)m.pData + y * m.RowPitch, 1280 * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    check_quads(got, 1280, 720, "screenshot (whole frame, not its top-left corner)");

    /* 2. один логический пиксель из увеличенной цели в маленькую читаемую */
    ID3D11Texture2D *one = tex_ex(8, 8, FMT, 1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL);
    const UINT pts[4][2] = {{0, 0}, {1279, 0}, {0, 719}, {1279, 719}};
    for (int i = 0; i < 4; i++) {
        D3D11_BOX b = {pts[i][0], pts[i][1], 0, pts[i][0] + 1, pts[i][1] + 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)one, 0, 0, 0, 0, (ID3D11Resource *)rt, 0, &b);
        UINT v = 0;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)one, 0, D3D11_MAP_READ, 0, &m))) {
            v = *(UINT *)m.pData;
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)one, 0);
        }
        UINT want = quad_color(pts[i][0], pts[i][1], 1280, 720);
        CHECK(v == want, "one pixel (%u,%u): %08X, want %08X", pts[i][0], pts[i][1], v, want);
    }

    /* 3. кадр из back buffer (он увеличен) */
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11RenderTargetView *vb = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &vb);
    const float red[4] = {1, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vb, red);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        UINT a = *(UINT *)m.pData, z = *(UINT *)((BYTE *)m.pData + 719 * m.RowPitch + 1279 * 4);
        CHECK(a == RED && z == RED, "back buffer readback: first %08X last %08X, want red", a, z);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }

    /* 4. загрузка через читаемую текстуру: игра пишет 1280x720, копия в увеличенную цель растягивается */
    ID3D11Texture2D *rt2 = tex(1280, 720, RT, NULL);
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_WRITE, 0, &m);
    CHECK(SUCCEEDED(hr), "Map staging for write hr=0x%08lx", hr);
    if (SUCCEEDED(hr)) {
        for (UINT y = 0; y < 720; y++)
            for (UINT x = 0; x < 1280; x++) *(UINT *)((BYTE *)m.pData + y * m.RowPitch + x * 4) = quad_color(x, y, 1280, 720);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)rt2, (ID3D11Resource *)st);
    UINT *p = read(rt2, 1920, 1080);
    check_quads(p, 1920, 1080, "upload through a staging texture");
    free(p);

    free(got);
    ID3D11RenderTargetView_Release(vb);
    ID3D11Texture2D_Release(bb); ID3D11Texture2D_Release(rt); ID3D11Texture2D_Release(rt2);
    ID3D11Texture2D_Release(st); ID3D11Texture2D_Release(one);
}

/* ============ copy: текстуры разного настоящего размера ============ */
static void t_copy(void)
{
    ID3D11Texture2D *a = tex(858, 348, RT, NULL);   /* увеличена: 1287x522 */
    paint_quads(a, 1287, 522);

    /* 1. область увеличенной -> обычная 512x512 (квадрат): в обычной — логическая область, уменьшенная */
    UINT *zero = (UINT *)calloc(512 * 512, 4);
    ID3D11Texture2D *pl = tex(512, 512, RT, zero);
    D3D11_BOX b = {0, 0, 0, 500, 300, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)pl, 0, 10, 20, 0, (ID3D11Resource *)a, 0, &b);
    UINT *p = read(pl, 512, 512);
    /* логическая точка (450,100) — правая верхняя четверть (синяя); (100,250) — левая нижняя (зелёная) */
    CHECK(p[(20 + 100) * 512 + 10 + 450] == BLUE && p[(20 + 250) * 512 + 10 + 100] == GREEN && p[20 * 512 + 10] == RED &&
          p[(20 + 299) * 512 + 10 + 499] == WHITE, "enlarged -> plain region: %08X %08X %08X %08X",
          p[(20 + 100) * 512 + 10 + 450], p[(20 + 250) * 512 + 10 + 100], p[20 * 512 + 10], p[(20 + 299) * 512 + 10 + 499]);
    CHECK(p[20 * 512 + 9] == 0 && p[19 * 512 + 10] == 0 && p[(20 + 300) * 512 + 10] == 0 && p[20 * 512 + 10 + 500] == 0,
          "enlarged -> plain region: nothing outside the 500x300 rectangle");
    free(p);

    /* 2. целиком увеличенная -> обычная того же логического размера (создана с картинкой — мод её не увеличивает) */
    UINT *z2 = (UINT *)calloc(858 * 348, 4);
    ID3D11Texture2D *same = tex(858, 348, RT, z2);
    UINT rw, rh;
    real_size(same, &rw, &rh);
    CHECK(rw == 858 && rh == 348, "texture created with pixels stays %ux%u", rw, rh);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)same, (ID3D11Resource *)a);
    p = read(same, 858, 348);
    check_quads(p, 858, 348, "enlarged -> plain, whole");
    free(p);

    /* 3. обратно: обычная -> увеличенная, целиком (растяжение) */
    ID3D11Texture2D *a2 = tex(858, 348, RT, NULL);
    UINT *src = (UINT *)malloc(858 * 348 * 4);
    fill_quads(src, 858, 348, 858);
    ID3D11Texture2D *plain = tex(858, 348, D3D11_BIND_SHADER_RESOURCE, src);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)a2, (ID3D11Resource *)plain);
    p = read(a2, 1287, 522);
    check_quads(p, 1287, 522, "plain -> enlarged, whole");
    free(p);

    /* 4. целочисленный формат растянуть нельзя (нет плавной выборки): в D3D уходит только копия, верная в настоящих
     *    размерах, в логе — запись */
    ID3D11Texture2D *iz = tex_ex(858, 348, DXGI_FORMAT_R32_UINT, 1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL);
    ID3D11Texture2D *iq = tex_ex(512, 512, DXGI_FORMAT_R32_UINT, 1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL);
    real_size(iz, &rw, &rh);
    CHECK(rw == 1287 && rh == 522, "integer target 858x348 is really %ux%u, want 1287x522", rw, rh);
    D3D11_BOX part = {400, 100, 0, 858, 348, 1};   /* в настоящих размерах 687x372 не помещается в 512x512 со сдвигом */
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)iq, 0, 100, 200, 0, (ID3D11Resource *)iz, 0, &part);
    ID3D11DeviceContext_Flush(ctx);
    CHECK(log_has("copy: cannot convert"), "a copy that cannot be converted is noted in the log");

    free(zero); free(z2); free(src);
    ID3D11Texture2D_Release(a); ID3D11Texture2D_Release(a2); ID3D11Texture2D_Release(pl); ID3D11Texture2D_Release(same);
    ID3D11Texture2D_Release(plain); ID3D11Texture2D_Release(iz); ID3D11Texture2D_Release(iq);
}

/* ============ resolve: сведение выборок ============ */
static PFN_COMPILE comp;
static ID3DBlob *compile(const char *src, const char *prof)
{
    ID3DBlob *b = NULL;
    if (!comp || FAILED(comp(src, strlen(src), "t", NULL, NULL, "main", prof, 0, 0, &b, NULL))) return NULL;
    return b;
}

static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps[4];   /* красный, синий, зелёный, белый */
static int shaders(void)
{
    static const char *col[4] = {"1, 0, 0, 1", "0, 0, 1, 1", "0, 1, 0, 1", "1, 1, 1, 1"};
    ID3DBlob *bv = compile("float4 main(uint id : SV_VertexID) : SV_Position { float2 t = float2((id << 1) & 2, id & 2);"
                           " return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1); }", "vs_5_0");
    if (!bv) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &g_vs);
    for (int i = 0; i < 4; i++) {
        char src[128];
        snprintf(src, sizeof src, "float4 main() : SV_Target { return float4(%s); }", col[i]);
        ID3DBlob *bp = compile(src, "ps_5_0");
        if (!bp) return 0;
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &g_ps[i]);
    }
    return g_vs && g_ps[3];
}

/* четыре четверти рисованием: viewport игра задаёт в своих (логических) размерах w x h */
static void draw_quads(ID3D11RenderTargetView *v, ID3D11DepthStencilView *z, UINT w, UINT h)
{
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &v, z);
    if (z) ID3D11DeviceContext_ClearDepthStencilView(ctx, z, D3D11_CLEAR_DEPTH, 1, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    for (int i = 0; i < 4; i++) {
        float x0 = (i & 1) ? (float)(w / 2) : 0, y0 = (i & 2) ? (float)(h / 2) : 0;
        D3D11_VIEWPORT vp = {x0, y0, (i & 1) ? (float)(w - w / 2) : (float)(w / 2), (i & 2) ? (float)(h - h / 2) : (float)(h / 2), 0, 1};
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_PSSetShader(ctx, g_ps[i], NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
}

static void t_resolve(void)
{
    UINT q = 0;
    ID3D11Device_CheckMultisampleQualityLevels(dev, FMT, 4, &q);
    if (!q) { printf("no 4x MSAA on this device - skipped\n"); return; }
    if (!shaders()) { CHECK(0, "shaders for the MSAA draws"); return; }
    const float black[4] = {0, 0, 0, 1};

    /* 1. многовыборочная цель размера кэша интерфейса (858x348, 4x) с многовыборочной глубиной и её обычная пара:
     *    игра рисует картинку и сводит выборки — в паре должна оказаться вся картинка, не её угол */
    ID3D11Texture2D *ms = tex_ex(858, 348, FMT, 1, 4, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL);
    ID3D11Texture2D *dz = tex_ex(858, 348, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, 4, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, NULL);
    ID3D11Texture2D *rs = tex(858, 348, RT, NULL);
    ID3D11RenderTargetView *vm = NULL, *vr = NULL;
    ID3D11DepthStencilView *vz = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)ms, NULL, &vm);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rs, NULL, &vr);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dz, NULL, &vz);
    UINT mw, mh, zw, zh;
    real_size(ms, &mw, &mh);
    real_size(dz, &zw, &zh);
    CHECK(mw == zw && mh == zh, "MSAA target (%ux%u) and its MSAA depth (%ux%u) must be the same real size", mw, mh, zw, zh);
    /* WARP сведение между разными размерами терпит (растягивает сам), настоящие драйверы — нет: сверяем размеры */
    UINT pw2, ph2;
    real_size(rs, &pw2, &ph2);
    CHECK(mw == pw2 && mh == ph2, "MSAA target (%ux%u) and the texture it is resolved into (%ux%u) must be the same real size",
          mw, mh, pw2, ph2);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vr, black);
    draw_quads(vm, vz, 858, 348);
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)rs, 0, (ID3D11Resource *)ms, 0, FMT);
    UINT *p = read(rs, 1287, 522);
    check_quads(p, 1287, 522, "draw into MSAA pair, resolve into the enlarged texture");
    free(p);

    /* 2. экранная многовыборочная цель (увеличена) -> обычная текстура того же логического размера (не увеличена) */
    ID3D11Texture2D *sms = tex_ex(1280, 720, FMT, 1, 4, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL);
    UINT rw, rh;
    real_size(sms, &rw, &rh);
    CHECK(rw == 1920 && rh == 1080, "screen MSAA target is %ux%u, want 1920x1080", rw, rh);
    ID3D11RenderTargetView *vs2 = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)sms, NULL, &vs2);
    draw_quads(vs2, NULL, 1280, 720);
    UINT *z = (UINT *)calloc(1280 * 720, 4);
    ID3D11Texture2D *pl = tex(1280, 720, RT, z);   /* с картинкой — не увеличена */
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)pl, 0, (ID3D11Resource *)sms, 0, FMT);
    p = read(pl, 1280, 720);
    check_quads(p, 1280, 720, "resolve enlarged MSAA -> plain texture");
    ID3D11DeviceContext_Flush(ctx);
    CHECK(log_has("resolve: real sizes differ"), "the mod converts such a resolve itself instead of passing it to D3D");
    free(p);
    free(z);

    if (vz) ID3D11DepthStencilView_Release(vz);
    ID3D11RenderTargetView_Release(vm); ID3D11RenderTargetView_Release(vr); ID3D11RenderTargetView_Release(vs2);
    ID3D11Texture2D_Release(ms); ID3D11Texture2D_Release(rs); ID3D11Texture2D_Release(dz);
    ID3D11Texture2D_Release(sms); ID3D11Texture2D_Release(pl);
}

/* ============ targets: цель и глубина разного настоящего размера ============ */
static void t_targets(void)
{
    /* цель создана с картинкой (не увеличена, 858x348), глубина без картинки (увеличена до 1287x522): игра считает их
     * одинаковыми, настоящие размеры разные — такую пару D3D не принимает. Мод обязан отвязать глубину. */
    UINT *z = (UINT *)calloc(858 * 348, 4);
    ID3D11Texture2D *t = tex(858, 348, RT, z);
    ID3D11Texture2D *dz = tex_ex(858, 348, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, NULL);
    ID3D11RenderTargetView *vt = NULL;
    ID3D11DepthStencilView *vz = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &vt);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dz, NULL, &vz);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vt, vz);
    ID3D11RenderTargetView *gr = NULL;
    ID3D11DepthStencilView *gz = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(real_of(ctx), 1, &gr, &gz);
    CHECK(gr == vt, "render target stays bound");
    CHECK(gz == NULL, "depth of another real size must be detached (bound: %p)", (void *)gz);
    if (gr) ID3D11RenderTargetView_Release(gr);
    if (gz) ID3D11DepthStencilView_Release(gz);

    /* одинаковые настоящие размеры — пара остаётся */
    ID3D11Texture2D *t2 = tex(858, 348, RT, NULL);
    ID3D11RenderTargetView *vt2 = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t2, NULL, &vt2);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vt2, vz);
    gr = NULL; gz = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(real_of(ctx), 1, &gr, &gz);
    CHECK(gr == vt2 && gz == vz, "matching pair stays bound");
    if (gr) ID3D11RenderTargetView_Release(gr);
    if (gz) ID3D11DepthStencilView_Release(gz);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);

    free(z);
    ID3D11RenderTargetView_Release(vt); ID3D11RenderTargetView_Release(vt2); ID3D11DepthStencilView_Release(vz);
    ID3D11Texture2D_Release(t); ID3D11Texture2D_Release(t2); ID3D11Texture2D_Release(dz);
}

/* ============ depthcopy: игра копирует буфер глубины целиком (Г2) ============ */
/* Без HDR игра дважды за кадр копирует глубину сцены в такую же текстуру (CopySubresourceRegion, целиком, без области) и
 * читает копию в затенении HBAO+, мягких частицах и глубине резкости. Буфер глубины D3D копирует только целиком: с
 * областью (даже во весь размер) вызов недопустим и молча отбрасывается. Мод обязан передать такую копию без области —
 * иначе на строках с увеличенным интерфейсом копия пустая, затенения нет и мир выглядит плоско («мыльно»). */
static void t_depthcopy(void)
{
    if (!shaders()) { CHECK(0, "shaders for the depth check"); return; }
    const UINT zbind = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;   /* как у игры: fmt=44 bind=0x48 */
    ID3D11Texture2D *rt = tex(1280, 720, RT, NULL);
    ID3D11Texture2D *d1 = tex_ex(1280, 720, DXGI_FORMAT_R24G8_TYPELESS, 1, 1, D3D11_USAGE_DEFAULT, zbind, 0, NULL);
    ID3D11Texture2D *d2 = tex_ex(1280, 720, DXGI_FORMAT_R24G8_TYPELESS, 1, 1, D3D11_USAGE_DEFAULT, zbind, 0, NULL);
    if (!rt || !d1 || !d2) return;
    UINT w1, h1, w2, h2;
    real_size(d1, &w1, &h1);
    real_size(d2, &w2, &h2);
    CHECK(w1 == 1920 && h1 == 1080 && w2 == 1920 && h2 == 1080, "both depth textures enlarged: %ux%u and %ux%u, want 1920x1080", w1, h1, w2, h2);
    D3D11_DEPTH_STENCIL_VIEW_DESC zd;
    ZeroMemory(&zd, sizeof zd);
    zd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    zd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ID3D11DepthStencilView *v1 = NULL, *v2 = NULL;
    ID3D11RenderTargetView *vr = NULL;
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)d1, &zd, &v1);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)d2, &zd, &v2);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &vr);
    if (!v1 || !v2 || !vr) { CHECK(0, "views for the depth check"); return; }
    const float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearDepthStencilView(ctx, v1, D3D11_CLEAR_DEPTH, 0.25f, 0);   /* глубина сцены */
    ID3D11DeviceContext_ClearDepthStencilView(ctx, v2, D3D11_CLEAR_DEPTH, 1.0f, 0);    /* копия: пока пусто */
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vr, black);
    /* как игра: целиком, без области */
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)d2, 0, 0, 0, 0, (ID3D11Resource *)d1, 0, NULL);

    /* что лежит в копии, проверяем рисованием: белый треугольник на глубине 0.5 проходит только там, где в копии 0.25 */
    D3D11_DEPTH_STENCIL_DESC sd;
    ZeroMemory(&sd, sizeof sd);
    sd.DepthEnable = TRUE;
    sd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    sd.DepthFunc = D3D11_COMPARISON_GREATER;
    ID3D11DepthStencilState *st = NULL;
    ID3D11Device_CreateDepthStencilState(dev, &sd, &st);
    D3D11_VIEWPORT vp = {0, 0, 1280, 720, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vr, v2);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, st, 0);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g_ps[3], NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    UINT *p = read(rt, 1920, 1080);
    UINT a = p[0], b = p[540 * 1920 + 960], c = p[1079 * 1920 + 1919];
    CHECK(a == WHITE && b == WHITE && c == WHITE,
          "whole depth copy between two enlarged depth textures did not happen: pixels %08x %08x %08x, want white "
          "(D3D copies a depth buffer only as a whole, without a box)", a, b, c);
    free(p);
    if (st) ID3D11DepthStencilState_Release(st);
    ID3D11RenderTargetView_Release(vr); ID3D11DepthStencilView_Release(v1); ID3D11DepthStencilView_Release(v2);
    ID3D11Texture2D_Release(rt); ID3D11Texture2D_Release(d1); ID3D11Texture2D_Release(d2);
}

/* ============ hdrmsaa: HDR включён + MSAA, мир меньше экрана ============ */
/* настоящие пиксели R16G16B16A16_FLOAT: по 4 половинных числа на пиксель */
static USHORT *read_half(ID3D11Texture2D *t, UINT pw, UINT ph)
{
    ID3D11Texture2D *st = tex_ex(pw + 1, ph + 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL);
    USHORT *px = (USHORT *)calloc((size_t)pw * ph, 8);
    if (!st) return px;
    D3D11_BOX b = {0, 0, 0, pw, ph, 1};
    ID3D11DeviceContext_CopySubresourceRegion(real_of(ctx), (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)t, 0, &b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(real_of(ctx), (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < ph; y++) memcpy(px + (size_t)y * pw * 4, (BYTE *)m.pData + y * m.RowPitch, (size_t)pw * 8);
        ID3D11DeviceContext_Unmap(real_of(ctx), (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return px;
}

static void t_hdrmsaa(void)
{
    const DXGI_FORMAT HDR = DXGI_FORMAT_R16G16B16A16_FLOAT;
    UINT q = 0;
    ID3D11Device_CheckMultisampleQualityLevels(dev, HDR, 4, &q);
    if (!q) { printf("no 4x MSAA for the HDR format on this device - skipped\n"); return; }
    if (!shaders()) { CHECK(0, "shaders for the MSAA draws"); return; }
    /* как игра (лог проверки 17.11): цель мира HDR x4, глубина x4, обычная HDR-текстура под сведение, копия экрана */
    ID3D11Texture2D *ms = tex_ex(1280, 720, HDR, 1, 4, D3D11_USAGE_DEFAULT, RT, 0, NULL);
    ID3D11Texture2D *dz = tex_ex(1280, 720, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, 4, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, NULL);
    ID3D11Texture2D *flat = tex_ex(1280, 720, HDR, 1, 1, D3D11_USAGE_DEFAULT, RT, 0, NULL);
    ID3D11Texture2D *scr = tex(1280, 720, RT, NULL);
    UINT w[4], h[4];
    real_size(ms, &w[0], &h[0]); real_size(dz, &w[1], &h[1]); real_size(flat, &w[2], &h[2]); real_size(scr, &w[3], &h[3]);
    /* мир 50% монитора: 1280x720 игры -> 960x540; интерфейс x1.5 -> 1920x1080 */
    CHECK(w[0] == 960 && h[0] == 540, "multisampled world target is %ux%u, want 960x540 (world size, not interface size)", w[0], h[0]);
    CHECK(w[1] == 960 && h[1] == 540, "multisampled world depth is %ux%u, want 960x540", w[1], h[1]);
    CHECK(w[2] == 960 && h[2] == 540, "plain world texture is %ux%u, want 960x540", w[2], h[2]);
    CHECK(w[3] == 1920 && h[3] == 1080, "screen-format copy is %ux%u, want 1920x1080 (interface size with HDR on)", w[3], h[3]);
    ID3D11RenderTargetView *vm = NULL;
    ID3D11DepthStencilView *vz = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)ms, NULL, &vm);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dz, NULL, &vz);
    if (!vm || !vz) { CHECK(0, "views of the multisampled world target and depth"); return; }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vm, vz);
    ID3D11RenderTargetView *gr = NULL;
    ID3D11DepthStencilView *gz = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(real_of(ctx), 1, &gr, &gz);
    CHECK(gr == vm && gz == vz, "world target and world depth stay bound together (target %p, depth %p)", (void *)gr, (void *)gz);
    if (gr) ID3D11RenderTargetView_Release(gr);
    if (gz) ID3D11DepthStencilView_Release(gz);
    draw_quads(vm, vz, 1280, 720);
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)flat, 0, (ID3D11Resource *)ms, 0, HDR);
    ID3D11DeviceContext_Flush(ctx);
    USHORT *p = read_half(flat, 960, 540);
    /* четверти: красная | синяя / зелёная | белая; 1.0 = 0x3C00 */
    static const USHORT want[4][3] = {{0x3C00, 0, 0}, {0, 0, 0x3C00}, {0, 0x3C00, 0}, {0x3C00, 0x3C00, 0x3C00}};
    for (int i = 0; i < 4; i++) {
        UINT x = (i & 1) ? 720 : 240, y = (i & 2) ? 405 : 135;
        const USHORT *c = p + ((size_t)y * 960 + x) * 4;
        CHECK(c[0] == want[i][0] && c[1] == want[i][1] && c[2] == want[i][2],
              "resolved world picture, quarter %d at (%u,%u): %04x %04x %04x, want %04x %04x %04x", i, x, y, c[0], c[1], c[2],
              want[i][0], want[i][1], want[i][2]);
    }
    free(p);
    CHECK(log_has("MSAA x4 is on in the game settings"), "MSAA taken from the settings");
    CHECK(!log_has("safe mode"), "no safe mode: the world keeps its size with MSAA");
    CHECK(!log_has("resolve: real sizes differ"), "same real size: the resolve goes to D3D as is");
    CHECK(!log_has("depth detached"), "depth is not detached");
    CHECK(log_has("tex2D scene: 1280x720 -> 960x540 fmt=10 ms=4"), "multisampled world target logged at world size");
    ID3D11RenderTargetView_Release(vm); ID3D11DepthStencilView_Release(vz);
    ID3D11Texture2D_Release(ms); ID3D11Texture2D_Release(dz); ID3D11Texture2D_Release(flat); ID3D11Texture2D_Release(scr);
}

/* ============ dof: радиус размытия глубины резкости (Г4) ============
 * Шейдер «размытия» с раскладкой игры (b0: радиус в c24.x, размер картинки в c24.yz) выводит радиус и ширину, которые
 * видит; второй шейдер читает ту же ячейку под другим именем (как dof_CoC — nearScale). Буфер сцены 156 float.
 * Экран 1280x720 x1.5: cb.c правит ширину 1280 -> 1920, радиус должен стать 2 * 1.5^exp только для шейдера размытия,
 * в каком бы порядке игра ни привязывала шейдер и ни писала буфер. */
static int g_dof_exp, g_dof_extra, g_dof_n = 156;   /* g_dof_n — размер буфера сцены в float */
static UINT px_r(ID3D11Texture2D *t)
{
    UINT pw, ph;
    real_size(t, &pw, &ph);
    UINT *p = read(t, pw, ph), v = p[(ph / 2) * pw + pw / 2];
    free(p);
    return v;
}
static void t_dof(void)
{
    CHECK(shaders(), "shaders");
    if (!g_vs) return;
    static const char *src_dof =
        "cbuffer dofb : register(b0) { float maxCoCRadiusPixels : packoffset(c24.x); float2 textureDimensions : packoffset(c24.y); }"
        "float4 main() : SV_Target { return float4(maxCoCRadiusPixels / 16, textureDimensions.x / 4096, 0, 1); }";
    static const char *src_coc = "cbuffer coc : register(b0) { float nearScale : packoffset(c24.x); }"
                                 "float4 main() : SV_Target { return float4(nearScale / 16, 0, 0, 1); }";
    ID3DBlob *bd = compile(src_dof, "ps_5_0"), *bc = compile(src_coc, "ps_5_0");
    CHECK(bd && bc, "compile");
    if (!bd || !bc) return;
    ID3D11PixelShader *pd = NULL, *pc = NULL;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bd), ID3D10Blob_GetBufferSize(bd), NULL, &pd);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bc), ID3D10Blob_GetBufferSize(bc), NULL, &pc);
    /* ревизия кода 4, п. 3: игра держит больше шейдеров размытия, чем помнит быстрый отсев мода (8 адресов) */
    ID3D11PixelShader *extra[8] = {0};
    for (int i = 0; i < g_dof_extra && i < 8; i++)
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bd), ID3D10Blob_GetBufferSize(bd), NULL, &extra[i]);
    D3D11_BUFFER_DESC bdsc = {(UINT)g_dof_n * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bdsc, NULL, &cb);
    ID3D11Texture2D *t = tex(1280, 720, RT, NULL);
    ID3D11RenderTargetView *v = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, &v);
    if (!pd || !pc || !cb || !v) { CHECK(0, "objects"); return; }
    UINT pw, ph;
    real_size(t, &pw, &ph);
    CHECK(pw == 1920 && ph == 1080, "target %ux%u, expected enlarged 1920x1080", pw, ph);

    float k = 1;
    for (int e = 0; e < g_dof_exp; e++) k *= 1.5f;
    #define WRITE(rad) do { D3D11_MAPPED_SUBRESOURCE m; \
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { \
            float *f = (float *)m.pData; memset(f, 0, (size_t)g_dof_n * 4); f[96] = (rad); f[97] = 1280; f[98] = 720; \
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0); } } while (0)
    D3D11_VIEWPORT vp = {0, 0, 1280, 720, 0, 1};
    #define DRAW(ps) do { ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &v, NULL); ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); \
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); \
        ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0); ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb); \
        ID3D11DeviceContext_PSSetShader(ctx, (ps), NULL, 0); ID3D11DeviceContext_Draw(ctx, 3, 0); } while (0)
    #define EXPECT(rad, what) do { UINT p_ = px_r(t), r_ = p_ & 0xFF, w_ = (p_ >> 8) & 0xFF; \
        UINT want_ = (UINT)((float)(rad) / 16 * 255 + 0.5f); \
        printf("%-44s radius %.2f (want %.2f), image width %.0f\n", what, r_ * 16 / 255.0, (double)(rad), w_ * 4096 / 255.0); \
        CHECK(r_ + 1 >= want_ && r_ <= want_ + 1, "%s: radius %.2f, expected %.2f", what, r_ * 16 / 255.0, (double)(rad)); } while (0)

    WRITE(2);                                   /* игра пишет буфер, потом привязывает шейдер размытия */
    DRAW(pd);
    EXPECT(2 * k, "blur, buffer written before the shader");
    UINT p = px_r(t), w = (p >> 8) & 0xFF;
    CHECK(w + 1 >= 119 && w <= 121, "image width %.0f, expected 1920 (pair fixed by cb.c)", w * 4096 / 255.0);
    DRAW(pc);                                   /* другой проход читает ту же ячейку без новой записи */
    EXPECT(2, "other pass, same buffer, no new write");
    ID3D11DeviceContext_PSSetShader(ctx, pd, NULL, 0);   /* шейдер размытия привязан раньше записи */
    WRITE(3);
    DRAW(pd);
    EXPECT(3 * k, "blur, buffer written after the shader");
    DRAW(pd);
    EXPECT(3 * k, "blur again (second direction)");
    WRITE(5);                                   /* запись для другого прохода, пока ещё привязан шейдер размытия */
    DRAW(pc);
    EXPECT(5, "other pass written while blur was bound");
    /* ревизия кода 3: после размытия игра пишет буфер для прохода, где размера картинки размытия нет (0) — такую
     * запись мод не запоминает; вернуть «свою копию» после размытия значило бы затереть свежую запись игры */
    WRITE(4);
    DRAW(pd);
    EXPECT(4 * k, "blur before a write without the image size");
    { D3D11_MAPPED_SUBRESOURCE m;
      if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
          float *f = (float *)m.pData; memset(f, 0, (size_t)g_dof_n * 4); f[96] = 7;
          ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0); } }
    DRAW(pc);
    EXPECT(7, "other pass: fresh write without the image size kept");
    #undef WRITE
    #undef DRAW
    #undef EXPECT
    if (g_dof_exp) {
        CHECK(log_has("dof: depth-of-field blur shader"), "log: blur shader not recognised");
        CHECK(log_has("dof: blur radius 2 -> "), "log: radius line missing");
    } else CHECK(!log_has("dof: blur radius"), "dof_radius=0, but the radius was changed");
    ID3D11RenderTargetView_Release(v); ID3D11Texture2D_Release(t); ID3D11Buffer_Release(cb);
    ID3D11PixelShader_Release(pd); ID3D11PixelShader_Release(pc);
    for (int i = 0; i < 8; i++) if (extra[i]) ID3D11PixelShader_Release(extra[i]);
}

/* ============ binds: растягивающая копия не меняет привязки игры (ревизия кода 17.11, п. 1) ============
 * Приёмник (обычная текстура) привязан на чтение в PS t3, VS t0 и CS t5, источник (увеличенный) — UAV вычислений u2,
 * ещё одна картинка — UAV вывода u1. Копия из источника в приёмник рисуется модом (настоящие размеры разные). D3D при
 * этом сам отвязывает приёмник от чтения, источник — от записи, а смена целей снимает UAV вывода; мод должен вернуть всё. */
static void t_binds(void)
{
    UINT *fill = calloc(300 * 200, 4);
    ID3D11Texture2D *dst = tex(300, 200, RT, fill);                                   /* с начальными данными — не увеличена */
    ID3D11Texture2D *src = tex(1280, 720, RT | D3D11_BIND_UNORDERED_ACCESS, NULL);    /* экранная — увеличена */
    ID3D11Texture2D *ot = tex_ex(64, 48, FMT, 1, 1, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, 0, fill);
    ID3D11ShaderResourceView *sv = NULL;
    ID3D11UnorderedAccessView *ou = NULL, *su = NULL;
    if (dst) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)dst, NULL, &sv);
    if (ot) ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)ot, NULL, &ou);
    if (src) ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)src, NULL, &su);
    if (!sv || !ou || !su) { CHECK(0, "objects"); return; }
    UINT pw, ph;
    real_size(src, &pw, &ph);
    CHECK(pw == 1920 && ph == 1080, "source %ux%u, expected enlarged 1920x1080", pw, ph);
    /* источник — красный: копия должна его прочитать (слой отладки 17.11: при источнике в UAV вычислений D3D обнулял
     * чтение мода, копия выходила чёрной) */
    ID3D11RenderTargetView *srt = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)src, NULL, &srt);
    const float red[4] = {1, 0, 0, 1};
    if (srt) { ID3D11DeviceContext_ClearRenderTargetView(ctx, srt, red); ID3D11RenderTargetView_Release(srt); }
    UINT keep = (UINT)-1;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 3, 1, &sv);
    ID3D11DeviceContext_VSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 5, 1, &sv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 2, 1, &su, &keep);
    ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ctx, 0, NULL, NULL, 1, 1, &ou, &keep);
    D3D11_BOX b = {0, 0, 0, 300, 200, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &b);
    ID3D11ShaderResourceView *p3 = NULL, *v0 = NULL, *c5 = NULL;
    ID3D11UnorderedAccessView *u1 = NULL, *c2 = NULL;
    ID3D11DeviceContext_PSGetShaderResources(ctx, 3, 1, &p3);
    ID3D11DeviceContext_VSGetShaderResources(ctx, 0, 1, &v0);
    ID3D11DeviceContext_CSGetShaderResources(ctx, 5, 1, &c5);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(ctx, 2, 1, &c2);
    ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews(ctx, 0, NULL, NULL, 1, 1, &u1);
    printf("after a stretched copy (1 = kept): PS t3 %d, VS t0 %d, CS t5 %d, CS u2 %d, OM u1 %d\n", p3 == sv, v0 == sv, c5 == sv,
           c2 == su, u1 == ou);
    CHECK(p3 == sv && v0 == sv && c5 == sv, "destination lost from shader input slots");
    CHECK(c2 == su, "source lost from the compute UAV slot");
    CHECK(u1 == ou, "output UAV lost");
    CHECK(log_has("copy: drawn"), "the copy was not drawn by the mod (test does not cover the case)");
    ID3D11UnorderedAccessView *nu = NULL;   /* приёмник читаем после того, как игра сняла привязки */
    ID3D11ShaderResourceView *ns = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 2, 1, &nu, &keep);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 3, 1, &ns);
    UINT *px = read(dst, 300, 200);
    UINT got = px ? px[100 * 300 + 150] : 0;
    free(px);
    printf("copied pixel 0x%08x (want red 0xff0000ff)\n", got);
    CHECK(got == 0xFF0000FFu, "copy content: 0x%08x, want red - the source was not read (still bound as a compute UAV?)", got);
    if (p3) ID3D11ShaderResourceView_Release(p3);
    if (v0) ID3D11ShaderResourceView_Release(v0);
    if (c5) ID3D11ShaderResourceView_Release(c5);
    if (c2) ID3D11UnorderedAccessView_Release(c2);
    if (u1) ID3D11UnorderedAccessView_Release(u1);
    free(fill);
}

/* ============ pool: временные текстуры пересчёта не создаются на каждую копию (ревизия кода 17.11, п. 5) ============
 * Приёмник без привязки цели рисования (только чтение) — копия в него идёт через временную текстуру. Пять копий подряд —
 * одна временная текстура; после 2 с без дела Present её освобождает; следующая копия создаёт её снова. Содержимое
 * приёмника после каждой копии — из источника (запас не подмешивает старое). */
static void t_pool(void)
{
    UINT *fill = calloc(300 * 200, 4);
    ID3D11Texture2D *dst = tex(300, 200, D3D11_BIND_SHADER_RESOURCE, fill);   /* в неё нельзя рисовать */
    ID3D11Texture2D *src = tex(1280, 720, RT, NULL);                          /* увеличена x1.5 */
    if (!dst || !src) return;
    UINT pw, ph;
    real_size(src, &pw, &ph);
    D3D11_BOX b = {0, 0, 0, 300, 200, 1};
    for (int i = 0; i < 5; i++) {
        paint_quads(src, pw, ph);   /* каждый раз заново: копия должна взять свежее */
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &b);
    }
    UINT *p = read(dst, 300, 200);
    CHECK(p[10 * 300 + 10] == RED, "copied pixel 0x%08x, expected red (top-left quarter of the source)", p[10 * 300 + 10]);
    free(p);
    int made = log_count("copy: temporary texture");
    printf("5 copies through a temporary texture: %d created\n", made);
    CHECK(made == 1, "temporary textures created: %d, expected 1", made);
    IDXGISwapChain_Present(sc, 0, 0);
    CHECK(!log_has("temporary texture(s) released"), "released right away, expected after 2 s");
    Sleep(2100);
    IDXGISwapChain_Present(sc, 0, 0);
    CHECK(log_has("temporary texture(s) released"), "not released after 2 s unused");
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &b);
    made = log_count("copy: temporary texture");
    CHECK(made == 2, "after release the next copy should create it again: %d created", made);
    ID3D11Texture2D_Release(dst); ID3D11Texture2D_Release(src);
    free(fill);
}

/* ============ deferred: загрузка с областью в отложенном контексте (ревизия кода 17.11, п. 2) ============
 * Если драйвер не умеет свои командные списки (DriverCommandLists = 0: WARP, часть AMD / Intel), D3D для
 * UpdateSubresource с областью в отложенном контексте прибавляет к указателю данных смещение области. Microsoft велит
 * приложению заранее его вычесть — так и делаем, как сделала бы игра. Зелёный квадрат 64x32 в (100, 50): в обычную
 * текстуру (проверка самого D3D) и в увеличенную (её мод пересчитывает). Данные игры — в середине большого буфера,
 * чтобы сдвинутый указатель не выходил за память. */
static void t_deferred(void)
{
    D3D11_FEATURE_DATA_THREADING th = {0};
    ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_THREADING, &th, sizeof th);
    int quirk = !th.DriverCommandLists;
    printf("driver command lists: %d -> game pre-shifts the pointer: %d\n", th.DriverCommandLists, quirk);
    ID3D11DeviceContext *dc = NULL;
    ID3D11Device_CreateDeferredContext(dev, 0, &dc);
    CHECK(dc != NULL, "no deferred context");
    if (!dc) return;
    BYTE *big = calloc(1, 1 << 20);
    UINT *pix = (UINT *)(big + (1 << 19));
    for (int i = 0; i < 64 * 32; i++) pix[i] = GREEN;
    D3D11_BOX b = {100, 50, 0, 164, 82, 1};
    UINT rp = 64 * 4;
    const BYTE *arg = (const BYTE *)pix - (quirk ? (size_t)b.top * rp + (size_t)b.left * 4 : 0);
    UINT *black = calloc(1280 * 720, 4);
    ID3D11Texture2D *plain = tex(1280, 720, D3D11_BIND_SHADER_RESOURCE, black);   /* начальные данные — не увеличена */
    ID3D11Texture2D *big_t = tex(1280, 720, RT, NULL);                              /* экранная — увеличена x1.5 */
    if (!plain || !big_t) return;
    FLOAT z[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView *rv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)big_t, NULL, &rv);
    if (rv) { ID3D11DeviceContext_ClearRenderTargetView(ctx, rv, z); ID3D11RenderTargetView_Release(rv); }
    ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *)plain, 0, &b, arg, rp, 0);
    ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *)big_t, 0, &b, arg, rp, 0);
    ID3D11CommandList *cl = NULL;
    ID3D11DeviceContext_FinishCommandList(dc, FALSE, &cl);
    if (cl) { ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE); ID3D11CommandList_Release(cl); }
    UINT *p = read(plain, 1280, 720);
    printf("plain texture: inside 0x%08x (want green), outside 0x%08x\n", p[60 * 1280 + 120], p[10 * 1280 + 10]);
    CHECK(p[60 * 1280 + 120] == GREEN && p[10 * 1280 + 10] != GREEN, "D3D itself: the region is not where expected");
    free(p);
    UINT pw, ph;
    real_size(big_t, &pw, &ph);
    p = read(big_t, pw, ph);
    UINT in = p[90 * pw + 180], out = p[10 * pw + 10], edge = p[74 * pw + 149];   /* (100,50)x1.5 = (150,75) */
    printf("enlarged texture %ux%u: inside 0x%08x (want green), outside 0x%08x, just outside 0x%08x\n", pw, ph, in, out, edge);
    CHECK(in == GREEN, "enlarged texture: the game's pixels did not arrive");
    CHECK(out != GREEN && edge != GREEN, "enlarged texture: pixels landed outside the region");
    free(p);
    ID3D11DeviceContext_Release(dc);
    free(big); free(black);
}

/* ============ лог и пакет подписей при второй копии игры и закрытой папке (ревизия кода 17.11, пп. 3–4) ============
 * Три запуска в одной папке (build.py): labels — обычный, пакет пишется рядом с DLL; second — лог и пакет держит
 * «первая копия игры» (этот же процесс открывает их до загрузки DLL, запись другим запрещена): DLL пишет свой лог
 * ui4k.2.log, чужой не трогает, подписи берёт из уже лежащего такого же пакета; labels_ro — на месте пакета папка
 * (записать нельзя): пакет уходит в папку пользователя (test_appdata). */
static int file_has(const char *name, const char *needle)
{
    static char buf[1 << 20];
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\%s", dir, name);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}
static HANDLE g_hold_log = INVALID_HANDLE_VALUE, g_hold_pak = INVALID_HANDLE_VALUE;
static LONGLONG g_log_size = -1;
static HANDLE hold(const char *name)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\%s", dir, name);
    return CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);   /* писать другим нельзя */
}
static void t_labels(const char *mode)
{
    if (!strcmp(mode, "labels")) {
        CHECK(log_has("labels: ") && log_has("ui4k_labels.pak") && log_has("(labels on)"), "labels pak not written next to the DLL");
    } else if (!strcmp(mode, "second")) {
        LARGE_INTEGER s;
        CHECK(g_hold_log != INVALID_HANDLE_VALUE && g_hold_pak != INVALID_HANDLE_VALUE, "files of the first run are missing");
        CHECK(GetFileSizeEx(g_hold_log, &s) && s.QuadPart == g_log_size, "the first copy's log was changed");
        CHECK(file_has("ui4k.2.log", "is written by another copy of the game"), "no own log ui4k.2.log");
        CHECK(file_has("ui4k.2.log", "is already up to date"), "labels pak in use: not reused");
        CHECK(file_has("ui4k.2.log", "(labels on)"), "labels off in the second copy");
        printf("second copy: own log ui4k.2.log, first log untouched (%lld bytes), labels from the pak already there\n", g_log_size);
    } else {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\appdata\\ui4k_labels.pak", dir);
        DWORD a = GetFileAttributesA(p);
        CHECK(log_has("trying the user folder"), "no fallback to the user folder");
        CHECK(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY), "labels pak not in the user folder");
        CHECK(log_has("(labels on)"), "labels off although the user folder is writable");
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: test_boundary <dir> upload|readback|copy|resolve|targets|loglimit|profile|profile_safe|hdrmsaa|depthcopy|depthcopy_warp\nFAIL\n"); return 2; }
    dir = argv[1];
    if (!strcmp(argv[2], "second")) {   /* «первая копия игры» держит свой лог и пакет подписей */
        g_hold_log = hold("ui4k.log");
        g_hold_pak = hold("ui4k_labels.pak");
        LARGE_INTEGER s;
        if (g_hold_log != INVALID_HANDLE_VALUE && GetFileSizeEx(g_hold_log, &s)) g_log_size = s.QuadPart;
    }
    AddVectoredExceptionHandler(1, on_crash);
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    comp = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k boundary", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC scd = {{1280, 720, {60, 1}, FMT}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE,
                                DXGI_SWAP_EFFECT_DISCARD, 0};
    /* depthcopy: на настоящей видеокарте — недопустимую копию глубины отбрасывает она, программный WARP её терпит;
     * «depthcopy_warp» — то же на WARP (там проверяется только, что копия не сломана) */
    HRESULT hr = E_FAIL;
    if (!strcmp(argv[2], "depthcopy")) {
        hr = cdsc(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
        printf(SUCCEEDED(hr) ? "device: hardware\n" : "device: no hardware device (hr=0x%08lx), WARP\n", hr);
    }
    if (FAILED(hr)) hr = cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr) || !sc || !dev || !ctx) { printf("device hr=0x%08lx\nFAIL\n", hr); return 1; }

    if (!strcmp(argv[2], "upload")) t_upload();
    else if (!strcmp(argv[2], "readback")) t_readback();
    else if (!strcmp(argv[2], "copy")) t_copy();
    else if (!strcmp(argv[2], "resolve")) t_resolve();
    else if (!strcmp(argv[2], "targets")) t_targets();
    else if (!strcmp(argv[2], "loglimit")) t_loglimit();
    else if (!strcmp(argv[2], "profile")) t_profile();
    else if (!strcmp(argv[2], "profile_safe")) { g_safe = 1; t_profile(); }
    else if (!strcmp(argv[2], "hdrmsaa")) t_hdrmsaa();
    else if (!strcmp(argv[2], "depthcopy") || !strcmp(argv[2], "depthcopy_warp")) t_depthcopy();
    else if (!strcmp(argv[2], "binds")) t_binds();
    else if (!strcmp(argv[2], "pool")) t_pool();
    else if (!strcmp(argv[2], "deferred")) t_deferred();
    else if (!strcmp(argv[2], "labels") || !strcmp(argv[2], "second") || !strcmp(argv[2], "labels_ro")) t_labels(argv[2]);
    else if (!strcmp(argv[2], "dofx")) { g_dof_exp = 2; g_dof_extra = 8; t_dof(); }
    else if (!strcmp(argv[2], "dofbig")) { g_dof_exp = 2; g_dof_n = 300; t_dof(); }   /* буфер сцены больше 256 float */
    else if (!strncmp(argv[2], "dof", 3)) { g_dof_exp = argv[2][3] ? atoi(argv[2] + 3) : 2; t_dof(); }   /* dof, dof1, dof0 */
    else { printf("unknown mode %s\nFAIL\n", argv[2]); return 2; }

    ID3D11DeviceContext_Flush(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
