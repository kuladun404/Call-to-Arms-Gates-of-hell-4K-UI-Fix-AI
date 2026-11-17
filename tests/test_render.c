/* F14: сцена и выход с разными коэффициентами (scale=1.5, render=200 -> сцена ×3, выход ×1.5), WARP.
 *   test_render.exe <папка с d3d11.dll и ui4k.ini>
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

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11DeviceContext *real_of(ID3D11DeviceContext *w) { return *(ID3D11DeviceContext **)((char *)w + 8); }

/* настоящий размер: копия в staging нужного размера не удастся, если размер не тот — берём через real GetDesc
 * на настоящем контексте нельзя; поэтому читаем размер из лога создания и проверяем рисованием */
static ID3D11Texture2D *tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ID3D11Texture2D *t = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t)), "create %ux%u fmt %d", w, h, f);
    return t;
}

static void read_cb(ID3D11Buffer *cb, float *out, UINT n)
{
    D3D11_BUFFER_DESC d = {n * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *st = NULL;
    ID3D11Device_CreateBuffer(dev, &d, NULL, &st);
    ID3D11DeviceContext_CopyResource(real_of(ctx), (ID3D11Resource *)st, (ID3D11Resource *)cb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(real_of(ctx), (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, n * 4);
        ID3D11DeviceContext_Unmap(real_of(ctx), (ID3D11Resource *)st, 0);
    }
    ID3D11Buffer_Release(st);
}

static UINT pixel(ID3D11Texture2D *t, UINT pw, UINT ph, UINT x, UINT y)
{
    D3D11_TEXTURE2D_DESC d = {pw, ph, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &st);   /* staging не того размера, что экран — не масштабируется */
    ID3D11DeviceContext_CopyResource(real_of(ctx), (ID3D11Resource *)st, (ID3D11Resource *)t);
    UINT v = 0xdeadbeef;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(real_of(ctx), (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        v = ((UINT *)((BYTE *)m.pData + y * m.RowPitch))[x];
        ID3D11DeviceContext_Unmap(real_of(ctx), (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return v;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_render <dir>\nFAIL\n"); return 2; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed\nFAIL\n"); return 1; }
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k render", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {{640, 360, {0, 0}, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0}, {1, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    if (FAILED(cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    const UINT RT = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Texture2D *scene = tex(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, RT);          /* не формат back buffer: сцена */
    ID3D11Texture2D *post = tex(640, 360, DXGI_FORMAT_B8G8R8A8_UNORM, RT);           /* формат back buffer: выход */
    ID3D11Texture2D *depth = tex(640, 360, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView *vbb = NULL, *vsc = NULL;
    ID3D11DepthStencilView *vds = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &vbb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scene, NULL, &vsc);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, &dvd, &vds)), "dsv");

    /* 1. размеры в лог: сцена ×3, выход ×1.5 */
    snprintf(path, sizeof path, "%s\\ui4k.log", argv[1]);
    FILE *lf = fopen(path, "rb");
    static char log[1 << 20];
    size_t n = lf ? fread(log, 1, sizeof log - 1, lf) : 0;
    if (lf) fclose(lf);
    log[n] = 0;
    CHECK(strstr(log, "tex2D scene: 640x360 -> 1920x1080 fmt=28"), "scene texture x3");
    CHECK(strstr(log, "tex2D screen: 640x360 -> 960x540 fmt=87"), "output texture x1.5");
    CHECK(strstr(log, "tex2D scene: 640x360 -> 1920x1080 fmt=44"), "depth is scene");

    /* 2. глубина сцены с back buffer — отвязывается; со сценой — остаётся */
    ID3D11RenderTargetView *got[1] = {0};
    ID3D11DepthStencilView *gd = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vbb, vds);
    ID3D11DeviceContext_OMGetRenderTargets(real_of(ctx), 1, got, &gd);
    CHECK(got[0] && !gd, "scene depth detached from back buffer (rt %p ds %p)", (void *)got[0], (void *)gd);
    if (got[0]) ID3D11RenderTargetView_Release(got[0]);
    if (gd) ID3D11DepthStencilView_Release(gd);
    gd = NULL; got[0] = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vsc, vds);
    ID3D11DeviceContext_OMGetRenderTargets(real_of(ctx), 1, got, &gd);
    CHECK(got[0] && gd, "scene depth kept with scene target");
    if (got[0]) ID3D11RenderTargetView_Release(got[0]);
    if (gd) ID3D11DepthStencilView_Release(gd);

    /* 3. viewport по цели: сцена ×3, back buffer ×1.5 */
    D3D11_VIEWPORT vp = {0, 0, 640, 360, 0, 1}, rv;
    UINT nv = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSGetViewports(real_of(ctx), &nv, &rv);
    CHECK(rv.Width == 1920 && rv.Height == 1080, "scene viewport %gx%g", rv.Width, rv.Height);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vbb, NULL);
    nv = 1;
    ID3D11DeviceContext_RSGetViewports(real_of(ctx), &nv, &rv);
    CHECK(rv.Width == 960 && rv.Height == 540, "output viewport %gx%g", rv.Width, rv.Height);

    /* 4а. ревизия кода 4, п. 1: буферы нужного размера без экранных чисел (как служебный буфер HBAO+) не занимают
     *     места в таблице копий контекста (4 места) — иначе буфер сцены ниже не переписывался бы под цель */
    ID3D11Buffer *junk[4] = {0};
    for (int i = 0; i < 4; i++) {
        float z[84];
        for (int j = 0; j < 84; j++) z[j] = 0.25f * (float)(j + i);
        D3D11_BUFFER_DESC jd = {sizeof z, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3D11Device_CreateBuffer(dev, &jd, NULL, &junk[i]);
        if (junk[i]) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)junk[i], 0, NULL, z, 0, 0);
    }

    /* 4. CB156 с размером экрана: записан при цели-сцене -> 1920; рисование в back buffer -> переписан в 960 */
    float c[156] = {0}, back[156];
    c[4] = 640; c[5] = 360; c[8] = 1.0f / 640; c[9] = 1.0f / 360;
    D3D11_BUFFER_DESC bd = {sizeof c, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vsc, vds);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, c, 0, 0);
    read_cb(cb, back, 156);
    CHECK(back[4] == 1920 && back[5] == 1080, "CB written for scene: %g %g", back[4], back[5]);
    {   /* рисование ниже — пустое, но правильное: со слоем отладки Direct3D (build.py --debug-layer) без ошибок теста */
        typedef HRESULT(WINAPI *PFN_C)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                       UINT, UINT, ID3DBlob **, ID3DBlob **);
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        PFN_C comp = dc ? (PFN_C)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
        static const char vsrc[] = "float4 main(uint i : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }";
        ID3DBlob *bc = NULL;
        ID3D11VertexShader *vs = NULL;
        if (comp && SUCCEEDED(comp(vsrc, sizeof vsrc - 1, "v", NULL, NULL, "main", "vs_5_0", 0, 0, &bc, NULL)) && bc) {
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bc), ID3D10Blob_GetBufferSize(bc), NULL, &vs);
            ID3D10Blob_Release(bc);
        }
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (vs) ID3D11VertexShader_Release(vs);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vbb, NULL);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_Draw(ctx, 0, 0);
    read_cb(cb, back, 156);
    CHECK(back[4] == 960 && back[5] == 540 && back[8] > 1.0f / 961 && back[8] < 1.0f / 959,
          "CB rewritten for output: %g %g 1/%g", back[4], back[5], 1 / back[8]);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vsc, NULL);
    ID3D11DeviceContext_Draw(ctx, 0, 0);
    read_cb(cb, back, 156);
    CHECK(back[4] == 1920, "CB back to scene: %g", back[4]);
    for (int i = 0; i < 4; i++) if (junk[i]) ID3D11Buffer_Release(junk[i]);

    /* 5. копия участка внутри сцены: box и точка — по коэффициенту сцены (×3). Между сценой и выходом копий не бывает:
     *    копия требует совместимых форматов, а класс определяется семейством формата. */
    ID3D11Texture2D *scene2 = tex(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, RT);
    ID3D11RenderTargetView *vs2 = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scene2, NULL, &vs2);
    const float red[4] = {1, 0, 0, 1}, black[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vsc, red);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vs2, black);
    D3D11_BOX box = {0, 0, 0, 10, 10, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)scene2, 0, 5, 5, 0, (ID3D11Resource *)scene, 0, &box);
    /* staging R8G8B8A8: читаем через pixel() с форматом BGRA — для красного/чёрного важен только 0 или нет */
    D3D11_TEXTURE2D_DESC sd2 = {1920, 1080, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &sd2, NULL, &st);
    ID3D11DeviceContext_CopyResource(real_of(ctx), (ID3D11Resource *)st, (ID3D11Resource *)scene2);
    D3D11_MAPPED_SUBRESOURCE mp;
    UINT in = 0, out1 = 1, out2 = 1;
    if (SUCCEEDED(ID3D11DeviceContext_Map(real_of(ctx), (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
        UINT *row = (UINT *)((BYTE *)mp.pData + 15 * mp.RowPitch);
        in = row[15]; out1 = row[14];
        out2 = ((UINT *)((BYTE *)mp.pData + 45 * mp.RowPitch))[45];
        ID3D11DeviceContext_Unmap(real_of(ctx), (ID3D11Resource *)st, 0);
    }
    CHECK(in && !out1 && !out2, "scene region copy at x3: in %08X, left %08X, after %08X", in, out1, out2);
    ID3D11Texture2D_Release(st);
    ID3D11RenderTargetView_Release(vs2);
    ID3D11Texture2D_Release(scene2);
    (void)pixel;

    ID3D11Buffer_Release(cb);
    ID3D11RenderTargetView_Release(vbb); ID3D11RenderTargetView_Release(vsc); ID3D11DepthStencilView_Release(vds);
    ID3D11Texture2D_Release(scene); ID3D11Texture2D_Release(post); ID3D11Texture2D_Release(depth);
    ID3D11Texture2D_Release(bb);
    ID3D11DeviceContext_Release(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
