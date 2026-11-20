/* F04/F05 через собранную d3d11.dll (WARP, скрытое окно), ui4k.ini с scale=3/2.
 *   test_res.exe <папка с d3d11.dll и ui4k.ini>
 * Настоящий размер ресурса видим через IDXGISurface::GetDesc (его прокси не трогает).
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void real_size(void *tex, UINT *w, UINT *h)
{
    IDXGISurface *s = NULL;
    *w = *h = 0;
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)tex, &IID_IDXGISurface, (void **)&s)) && s) {
        DXGI_SURFACE_DESC d;
        IDXGISurface_GetDesc(s, &d);
        *w = d.Width; *h = d.Height;
        IDXGISurface_Release(s);
    }
}

static void check_buffer(IDXGISwapChain *sc, UINT lw, UINT lh, UINT pw, UINT ph, const char *what)
{
    DXGI_SWAP_CHAIN_DESC sd;
    IDXGISwapChain_GetDesc(sc, &sd);
    CHECK(sd.BufferDesc.Width == lw && sd.BufferDesc.Height == lh, "%s: swapchain GetDesc %ux%u, want %ux%u", what,
          sd.BufferDesc.Width, sd.BufferDesc.Height, lw, lh);
    ID3D11Texture2D *bb = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    CHECK(SUCCEEDED(hr) && bb, "%s: GetBuffer hr=0x%08lx", what, hr);
    if (!bb) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    UINT rw, rh;
    real_size(bb, &rw, &rh);
    CHECK(td.Width == lw && td.Height == lh, "%s: back buffer GetDesc %ux%u, want %ux%u", what, td.Width, td.Height, lw, lh);
    CHECK(rw == pw && rh == ph, "%s: back buffer real %ux%u, want %ux%u", what, rw, rh, pw, ph);
    ID3D11Device *dev = NULL;
    ID3D11Texture2D_GetDevice(bb, &dev);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {td.Format, D3D11_RTV_DIMENSION_TEXTURE2D};
    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, &rd, &rtv);
    CHECK(SUCCEEDED(hr), "%s: RTV with explicit desc on back buffer hr=0x%08lx", what, hr);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    ID3D11Device_Release(dev);
    ID3D11Texture2D_Release(bb);
}

/* создать текстуру, сверить логический (GetDesc) и настоящий размер, создать view с явным описанием */
static void tex_case(ID3D11Device *dev, const char *name, UINT w, UINT h, DXGI_FORMAT f, UINT bind, D3D11_USAGE u,
                     int init, UINT pw, UINT ph)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, u, bind, u == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0, 0};
    BYTE *px = init ? (BYTE *)calloc((size_t)w * h, 4) : NULL;
    D3D11_SUBRESOURCE_DATA sd = {px, w * 4, 0};
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, init ? &sd : NULL, &t);
    free(px);
    CHECK(SUCCEEDED(hr) && t, "%s: create hr=0x%08lx", name, hr);
    if (!t) return;
    D3D11_TEXTURE2D_DESC g;
    ID3D11Texture2D_GetDesc(t, &g);
    UINT rw, rh;
    real_size(t, &rw, &rh);
    CHECK(g.Width == w && g.Height == h, "%s: GetDesc %ux%u, want logical %ux%u", name, g.Width, g.Height, w, h);
    CHECK(rw == pw && rh == ph, "%s: real %ux%u, want %ux%u", name, rw, rh, pw, ph);
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {f, D3D11_SRV_DIMENSION_TEXTURE2D};
        vd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView *v = NULL;
        hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, &vd, &v);
        CHECK(SUCCEEDED(hr), "%s: SRV with explicit desc hr=0x%08lx", name, hr);
        if (v) ID3D11ShaderResourceView_Release(v);
    }
    if (bind & D3D11_BIND_DEPTH_STENCIL) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd = {f, D3D11_DSV_DIMENSION_TEXTURE2D};
        ID3D11DepthStencilView *v = NULL;
        hr = ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)t, &vd, &v);
        CHECK(SUCCEEDED(hr), "%s: DSV with explicit desc hr=0x%08lx", name, hr);
        if (v) ID3D11DepthStencilView_Release(v);
    }
    ID3D11Texture2D_Release(t);
}

/* импорт exe перехвачен прокси (F10) — измеряем настоящими функциями user32 */
typedef BOOL(WINAPI *PFN_GCR)(HWND, RECT *);
typedef BOOL(WINAPI *PFN_SWP)(HWND, HWND, int, int, int, int, UINT);
static RECT real_client(HWND w)
{
    RECT r = {0};
    ((PFN_GCR)GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect"))(w, &r);
    return r;
}
static UINT client_w(HWND w) { RECT r = real_client(w); return (UINT)(r.right - r.left); }
static UINT client_h(HWND w) { RECT r = real_client(w); return (UINT)(r.bottom - r.top); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_res <dir>\nFAIL\n"); return 2; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");

    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC scd = {{1281, 721, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0},
                                DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    CHECK(SUCCEEDED(hr) && sc && dev, "CreateDeviceAndSwapChain hr=0x%08lx", hr);
    if (!sc || !dev) { printf("FAIL\n"); return 1; }

    /* F04: нечётный логический 1281x721 -> 1922x1082 (round half up), GetDesc возвращает ровно 1281x721 */
    check_buffer(sc, 1281, 721, 1922, 1082, "create 1281x721");

    hr = IDXGISwapChain_ResizeBuffers(sc, 0, 1280, 720, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers 1280x720 hr=0x%08lx", hr);
    check_buffer(sc, 1280, 720, 1920, 1080, "ResizeBuffers 1280x720");

    /* F05 при логическом экране 1280x720 */
    const UINT RT = D3D11_BIND_RENDER_TARGET, SRV = D3D11_BIND_SHADER_RESOURCE;
    tex_case(dev, "screen RT", 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, RT | SRV, D3D11_USAGE_DEFAULT, 0, 1920, 1080);
    tex_case(dev, "half RT", 640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, RT | SRV, D3D11_USAGE_DEFAULT, 0, 960, 540);
    tex_case(dev, "unit preview", 858, 348, DXGI_FORMAT_R8G8B8A8_UNORM, RT | SRV, D3D11_USAGE_DEFAULT, 0, 1287, 522);
    tex_case(dev, "unit preview depth", 858, 348, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL,
             D3D11_USAGE_DEFAULT, 0, 1287, 522);
    tex_case(dev, "screen depth", 1280, 720, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL,
             D3D11_USAGE_DEFAULT, 0, 1920, 1080);
    /* В3: «читаемая» текстура остаётся размера игры — игра сама ходит по её строкам */
    tex_case(dev, "screen staging", 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, 0, 1280, 720);
    tex_case(dev, "screen SRV only", 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, SRV, D3D11_USAGE_DEFAULT, 0, 1280, 720);
    tex_case(dev, "RT with data", 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, RT | SRV, D3D11_USAGE_DEFAULT, 1, 1280, 720);
    tex_case(dev, "square RT", 512, 512, DXGI_FORMAT_R8G8B8A8_UNORM, RT | SRV, D3D11_USAGE_DEFAULT, 0, 512, 512);
    tex_case(dev, "UI image", 144, 72, DXGI_FORMAT_R8G8B8A8_UNORM, SRV, D3D11_USAGE_DEFAULT, 1, 144, 72);

    /* ResizeTarget в окне меняет клиентскую область на физический размер режима */
    DXGI_MODE_DESC md = {1000, 600, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    hr = IDXGISwapChain_ResizeTarget(sc, &md);
    CHECK(SUCCEEDED(hr), "ResizeTarget hr=0x%08lx", hr);
    CHECK(client_w(wnd) == 1500 && client_h(wnd) == 900, "ResizeTarget 1000x600: client %ux%u, want 1500x900",
          client_w(wnd), client_h(wnd));
    /* ResizeBuffers(0,0): размер окна; логический — тот, что задала игра */
    hr = IDXGISwapChain_ResizeBuffers(sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers 0x0 hr=0x%08lx", hr);
    check_buffer(sc, 1000, 600, 1500, 900, "ResizeTarget+ResizeBuffers(0,0)");

    /* окно, размер которого игра не задавала: логический = floor(физический / 1.5) */
    ((PFN_SWP)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowPos"))(wnd, NULL, 0, 0, 800, 600,
                                                                              SWP_NOMOVE | SWP_NOZORDER);
    UINT cw = client_w(wnd), ch = client_h(wnd);
    hr = IDXGISwapChain_ResizeBuffers(sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers after window resize hr=0x%08lx", hr);
    check_buffer(sc, cw * 2 / 3, ch * 2 / 3, cw, ch, "window resized by user");

    /* F08: экранные константы в CB 156 float (логический экран сейчас 1000x600 -> физический 1500x900) */
    hr = IDXGISwapChain_ResizeBuffers(sc, 0, 1000, 600, DXGI_FORMAT_UNKNOWN, 0);
    {
        float in[156] = {0}, out[156];
        in[4] = 1000; in[5] = 600; in[8] = 1.0f / 1000; in[9] = 1.0f / 600; in[20] = 7; in[21] = 600;
        D3D11_BUFFER_DESC bd = {sizeof in, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3D11Buffer *cb = NULL, *st = NULL;
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
        D3D11_BUFFER_DESC sd = {sizeof in, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Device_CreateBuffer(dev, &sd, NULL, &st);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, in, 0, 0);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)cb);
        D3D11_MAPPED_SUBRESOURCE mp;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp);
        memcpy(out, mp.pData, sizeof out);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        CHECK(out[4] == 1500 && out[5] == 900 && out[8] == 1.0f / 1500 && out[9] == 1.0f / 900 && out[20] == 7 &&
              out[21] == 600, "CB156 UpdateSubresource: (%g %g) (%g %g) (%g %g)", out[4], out[5], out[8], out[9],
              out[20], out[21]);
        CHECK(in[4] == 1000, "game's CB data must not be modified");
        /* Map/Unmap dynamic CB 84 float (HBAO+) */
        float h[84] = {0};
        h[0] = 0.5f / 1000; h[1] = 0.5f / 600;
        /* Г2: как у игры — [6,7] 1/размер кадра, [12] RadiusToScreen (одиночное число, пропорционально высоте) */
        h[6] = 1.0f / 1000; h[7] = 1.0f / 600; h[12] = 30000; h[13] = 324; h[14] = -1.0f / 324;   /* R² и −1/R² за радиусом */
        /* ревизия 20.11, Ф1: за радиусом — мировые числа (у игры [40..42] — позиция камеры). Камера рядом с размером экрана
         * не правится: экранные пары только до радиуса */
        h[40] = 1000.5f; h[41] = 600.25f;
        D3D11_BUFFER_DESC dd = {sizeof h, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Buffer *dcb = NULL, *dst = NULL;
        ID3D11Device_CreateBuffer(dev, &dd, NULL, &dcb);
        D3D11_BUFFER_DESC sd2 = {sizeof h, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Device_CreateBuffer(dev, &sd2, NULL, &dst);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            memcpy(mp.pData, h, sizeof h);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dcb, 0);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)dst, (ID3D11Resource *)dcb);
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dst, 0, D3D11_MAP_READ, 0, &mp);
        float *o = (float *)mp.pData;
        CHECK(o[0] == 0.5f / 1500 && o[1] == 0.5f / 900, "CB84 Map/Unmap: (%g %g)", o[0], o[1]);
        CHECK(o[12] > 44999 && o[12] < 45001 && o[13] == 324,
              "CB84 HBAO+ radius follows the real frame height: [12]=%g (want 45000), [13]=%g (want 324)", o[12], o[13]);
        CHECK(o[40] == 1000.5f && o[41] == 600.25f, "CB84: world numbers after the radius left alone: [40]=%g [41]=%g", o[40],
              o[41]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dst, 0);
        /* 20.11: угол обзора, при котором [7..9] устроены как «радиус, R², −1/R²» (A.x·A.y = −1). Номер найден раньше —
         * правится радиус [12], а 1/высота [7] остаётся 1/900 (раньше удваивалась, радиус не правился) */
        h[8] = 1.25f; h[9] = -0.8f;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            memcpy(mp.pData, h, sizeof h);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dcb, 0);
        }
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)dst, (ID3D11Resource *)dcb);
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)dst, 0, D3D11_MAP_READ, 0, &mp);
        o = (float *)mp.pData;
        CHECK(o[12] > 44999 && o[12] < 45001 && o[7] == 1.0f / 900 && o[8] == 1.25f,
              "CB84 at the FOV where [7..9] look like the radius: [12]=%g (want 45000), [7]=%g (want %g), [8]=%g", o[12],
              o[7], 1.0 / 900, o[8]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dst, 0);
        /* CB другого размера (UI-проекция 232) не трогается */
        float u[232] = {0};
        u[8] = 1000; u[9] = 600;
        D3D11_BUFFER_DESC ud = {sizeof u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_BUFFER_DESC us = {sizeof u, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Buffer *ucb = NULL, *ust = NULL;
        ID3D11Device_CreateBuffer(dev, &ud, NULL, &ucb);
        ID3D11Device_CreateBuffer(dev, &us, NULL, &ust);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)ucb, 0, NULL, u, 0, 0);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)ust, (ID3D11Resource *)ucb);
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ust, 0, D3D11_MAP_READ, 0, &mp);
        o = (float *)mp.pData;
        CHECK(o[8] == 1000 && o[9] == 600, "CB232 must stay logical: (%g %g)", o[8], o[9]);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ust, 0);
        ID3D11Buffer_Release(cb); ID3D11Buffer_Release(st); ID3D11Buffer_Release(dcb); ID3D11Buffer_Release(dst);
        ID3D11Buffer_Release(ucb); ID3D11Buffer_Release(ust);
    }

    IDXGISwapChain_Release(sc);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);

    /* фабрика DXGI (импорт exe перехвачен): CreateSwapChain тоже физический */
    ID3D11Device *dev2 = NULL;
    ID3D11DeviceContext *ctx2 = NULL;
    hr = cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev2, NULL, &ctx2);
    IDXGIFactory *f = NULL;
    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&f);
    CHECK(SUCCEEDED(hr) && f, "CreateDXGIFactory hr=0x%08lx", hr);
    if (f && dev2) {
        DXGI_SWAP_CHAIN_DESC d2 = scd;
        d2.BufferDesc.Width = 1280; d2.BufferDesc.Height = 720;
        IDXGISwapChain *sc2 = NULL;
        hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)dev2, &d2, &sc2);
        CHECK(SUCCEEDED(hr) && sc2, "factory CreateSwapChain hr=0x%08lx", hr);
        CHECK(d2.BufferDesc.Width == 1280, "game's desc must not be modified");
        if (sc2) {
            check_buffer(sc2, 1280, 720, 1920, 1080, "factory swapchain");
            IDXGISwapChain_Release(sc2);
        }
        IDXGIFactory_Release(f);
    }
    if (ctx2) ID3D11DeviceContext_Release(ctx2);
    if (dev2) ID3D11Device_Release(dev2);
    DestroyWindow(wnd);

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
