/* Э2: кэши окон по происхождению через собранную d3d11.dll (WARP), ui4k.ini со scale=3/2.
 *   test_origin.exe <папка с d3d11.dll и ui4k.ini> [off]
 * Как в игре: эта программа — «exe игры», в ней функция с меткой «t:create» создаёт цели, как буфер окна движка.
 * Мод ищет метку в образе exe при создании устройства и увеличивает цели, созданные из этой функции, любой формы.
 * off — в ui4k.ini origin=0: остаётся только правило формы v2. Настоящий размер — через IDXGISurface::GetDesc.
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *volatile g_tag;   /* ссылка на метку из кода — как у движка */

static ID3D11Texture2D *make(ID3D11Device *d, UINT w, UINT h, UINT bind, int with_init)
{
    static DWORD px[512 * 512];
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {px, w * 4, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(d, &td, with_init ? &sd : NULL, &t);
    return t;
}
/* «t:create» движка: метка в коде функции, создание цели внутри неё (возврат из CreateTexture2D — в ней) */
__declspec(noinline) static ID3D11Texture2D *engine_t_create(ID3D11Device *d, UINT w, UINT h, UINT bind, int with_init)
{
    g_tag = "t:create";
    static DWORD px[512 * 512];
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {px, w * 4, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(d, &td, with_init ? &sd : NULL, &t);
    return t;
}
/* «t:create» с форматом: глубина вида 3D (GUI2::eViewport) */
__declspec(noinline) static ID3D11Texture2D *engine_t_create_fmt(ID3D11Device *d, UINT w, UINT h, UINT bind, DXGI_FORMAT f)
{
    g_tag = "t:create";
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(d, &td, NULL, &t);
    g_tag = NULL;
    return t;
}
/* 19.11: отражение в воде — цель мира от «t:create»: 0x6fea00 (цвет и глубина) из 0x6de9f0 («video/water_quality») */
static const char *volatile g_opt;
__declspec(noinline) static void reflection_create(ID3D11Device *d, ID3D11Texture2D **c, ID3D11Texture2D **z)
{
    *c = engine_t_create(d, 512, 512, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    *z = engine_t_create_fmt(d, 512, 512, D3D11_BIND_DEPTH_STENCIL, DXGI_FORMAT_R24G8_TYPELESS);
    g_tag = NULL;
}
__declspec(noinline) static void water_quality_apply(ID3D11Device *d, ID3D11Texture2D **c, ID3D11Texture2D **z)
{
    g_opt = "video/water_quality";
    reflection_create(d, c, z);
    g_opt = NULL;
}

/* окно движка: отрисовка вызывает «t:create» (как eDialog 0xac0eae -> 0x647060) */
__declspec(noinline) static ID3D11Texture2D *dialog_paint(ID3D11Device *d, UINT w, UINT h)
{
    ID3D11Texture2D *t = engine_t_create(d, w, h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    g_tag = NULL;
    return t;
}

static void real_size(ID3D11Texture2D *t, UINT *w, UINT *h)
{
    IDXGISurface *s = NULL;
    *w = *h = 0;
    if (t && SUCCEEDED(ID3D11Texture2D_QueryInterface(t, &IID_IDXGISurface, (void **)&s)) && s) {
        DXGI_SURFACE_DESC d;
        IDXGISurface_GetDesc(s, &d);
        *w = d.Width;
        *h = d.Height;
        IDXGISurface_Release(s);
    }
}
static void expect(const char *what, ID3D11Texture2D *t, UINT ww, UINT wh)
{
    UINT w, h;
    real_size(t, &w, &h);
    CHECK(t && w == ww && h == wh, "%s: real %ux%u, want %ux%u", what, w, h, ww, wh);
    if (t) ID3D11Texture2D_Release(t);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_origin <dir> [off]\nFAIL\n"); return 2; }
    int off = argc > 2 && !strcmp(argv[2], "off");
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k.ini", argv[1]);
    FILE *f = fopen(path, "wb");
    fprintf(f, "[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\norigin=%d\n", off ? 0 : 1);
    fclose(f);
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    HRESULT hr = cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL);
    CHECK(SUCCEEDED(hr) && dev, "CreateDevice hr=0x%08lx", hr);
    if (!dev) { printf("FAIL\n"); return 1; }
    const UINT RS = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (!off) {
        /* кэши окон любой формы — x1.5 */
        expect("window cache 20x20 (square)", dialog_paint(dev, 20, 20), 30, 30);
        expect("cursor label 512x512 (powers of two)", dialog_paint(dev, 512, 512), 768, 768);
        expect("window cache 256x16 (powers of two)", dialog_paint(dev, 256, 16), 384, 24);
    } else {
        expect("origin=0: window cache 20x20 stays as v2 had it", dialog_paint(dev, 20, 20), 20, 20);
        expect("origin=0: cursor label 512x512 stays", dialog_paint(dev, 512, 512), 512, 512);
    }
    expect("window cache 146x51 (shape rule anyway)", dialog_paint(dev, 146, 51), 219, 77);
    {   /* 19.11: вид 3D (eViewport, 3D-иконки инвентаря) — цель 512x512 и её глубина, обе от «t:create» */
        ID3D11Texture2D *vt = dialog_paint(dev, 512, 512);
        ID3D11Texture2D *vd = engine_t_create_fmt(dev, 512, 512, D3D11_BIND_DEPTH_STENCIL, DXGI_FORMAT_R24G8_TYPELESS);
        UINT tw, th, dw, dh;
        real_size(vt, &tw, &th);
        real_size(vd, &dw, &dh);
        UINT want = off ? 512 : 768;
        CHECK(vt && vd && tw == want && dw == want && th == want && dh == want,
              "3D view target %ux%u and its depth %ux%u, want both %u", tw, th, dw, dh, want);
        ID3D11DeviceContext *ctx = NULL;
        ID3D11Device_GetImmediateContext(dev, &ctx);
        ID3D11RenderTargetView *rtv = NULL;
        ID3D11DepthStencilView *dsv = NULL;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
        if (vt) ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)vt, NULL, &rtv);
        if (vd) ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)vd, &dd, &dsv);
        CHECK(ctx && rtv && dsv, "views of the 3D view made");
        if (ctx && rtv && dsv) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
            ID3D11DepthStencilView *got = NULL;
            ID3D11DeviceContext_OMGetRenderTargets(ctx, 0, NULL, &got);
            CHECK(got == dsv, "the 3D view keeps its depth (not detached)");
            if (got) ID3D11DepthStencilView_Release(got);
            ID3D11DeviceContext_ClearState(ctx);
        }
        if (dsv) ID3D11DepthStencilView_Release(dsv);
        if (rtv) ID3D11RenderTargetView_Release(rtv);
        if (ctx) ID3D11DeviceContext_Release(ctx);
        if (vd) ID3D11Texture2D_Release(vd);
        if (vt) ID3D11Texture2D_Release(vt);
    }
    {   /* 19.11: отражение в воде — от «t:create», но из функции качества воды: мир, размер игры (цвет и глубина) */
        ID3D11Texture2D *wc = NULL, *wz = NULL;
        water_quality_apply(dev, &wc, &wz);
        expect("water reflection 512x512 (world target from t:create)", wc, 512, 512);
        expect("its depth 512x512", wz, 512, 512);
        /* та же «t:create» из окна — по-прежнему кэш интерфейса */
        expect("window cache 512x512 after the reflection", dialog_paint(dev, 512, 512), off ? 512 : 768, off ? 512 : 768);
    }
    /* не из «t:create» — правило формы v2 */
    expect("20x20 target made elsewhere (shadow, atlas)", make(dev, 20, 20, RS, 0), 20, 20);
    expect("2048x2048 target made elsewhere (minimap fog)", make(dev, 2048, 2048, RS, 0), 2048, 2048);
    expect("30x20 target made elsewhere (shape rule)", make(dev, 30, 20, RS, 0), 45, 30);
    /* из «t:create», но не цель с чтением или с начальными данными — не трогаем */
    expect("t:create, picture with pixels", engine_t_create(dev, 20, 20, RS, 1), 20, 20);
    expect("t:create, shader resource only", engine_t_create(dev, 20, 20, D3D11_BIND_SHADER_RESOURCE, 0), 20, 20);

    ID3D11Device_Release(dev);
    /* лог */
    snprintf(path, sizeof path, "%s\\ui4k.log", argv[1]);
    static char log[1 << 20];
    f = fopen(path, "rb");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f) fclose(f);
    log[n] = 0;
    if (!off) {
        CHECK(strstr(log, "origin: \"t:create\" at RVA"), "the label and its function are found in this exe");
        CHECK(strstr(log, "tex2D ui_rt (t:create): 20x20 -> 30x30"), "log names the enlarged cache and why");
        CHECK(strstr(log, "origin: water reflection - \"video/water_quality\" at RVA"), "the water function is found in this exe");
        CHECK(!strstr(log, "0 function(s)"), "the water function has its parts");
        CHECK(strstr(log, "tex2D: water reflection 512x512 fmt=87"), "log names the kept reflection");
        CHECK(strstr(log, "tex2D: water reflection 512x512 fmt=44"), "log names the kept reflection depth");
    } else {
        CHECK(!strstr(log, "origin: \"t:create\" at RVA"), "origin=0: no search at all");
    }
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
