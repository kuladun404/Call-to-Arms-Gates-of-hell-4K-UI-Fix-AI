/* Разведка F14: снимок кадра (diag_at=1) через собранную d3d11.dll на WARP.
 *   test_diag.exe <папка с d3d11.dll и ui4k.ini>
 * Рисует очистку, Draw и копию между Present, потом проверяет ui4k.log. Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                     UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_diag <dir>\nFAIL\n"); return 2; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed\nFAIL\n"); return 1; }
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k diag", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {{640, 360, {0, 0}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0},
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
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    D3D11_TEXTURE2D_DESC td = {640, 360, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *scene = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);
    /* пиксельный шейдер с sample, ld и позицией: разбор байткода должен дать [SLP] */
    ID3D11PixelShader *ps = NULL;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_COMPILE comp = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    static const char src[] = "Texture2D t : register(t0); SamplerState s : register(s0);\n"
                              "float4 main(float4 p : SV_Position) : SV_Target"
                              "{ return t.Sample(s, p.xy / 100) + t.Load(int3(p.xy, 0)); }";
    ID3DBlob *bc = NULL;
    if (comp && SUCCEEDED(comp(src, sizeof src - 1, "t", NULL, NULL, "main", "ps_5_0", 0, 0, &bc, NULL)) && bc) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bc), ID3D10Blob_GetBufferSize(bc), NULL, &ps);
        ID3D10Blob_Release(bc);
    }
    CHECK(ps, "compile test pixel shader (d3dcompiler_47)");
    /* compute-шейдер: его текст должен попасть в ui4k_cs.txt */
    static const char csrc[] = "RWTexture2D<float> u : register(u0);\n"
                               "[numthreads(8,8,1)] void main(uint3 i : SV_DispatchThreadID) { u[i.xy] = 1; }";
    ID3D11ComputeShader *cs = NULL;
    if (comp && SUCCEEDED(comp(csrc, sizeof csrc - 1, "c", NULL, NULL, "main", "cs_5_0", 0, 0, &bc, NULL)) && bc) {
        ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(bc), ID3D10Blob_GetBufferSize(bc), NULL, &cs);
        ID3D10Blob_Release(bc);
    }
    CHECK(cs, "compile test compute shader");
    if (cs) ID3D11ComputeShader_Release(cs);
    /* рисование ниже — правильное: со слоем отладки Direct3D (build.py --debug-layer) без ошибок самого теста */
    static const char vsrc[] = "float4 main(uint i : SV_VertexID) : SV_Position"
                               "{ return float4(i == 1 ? 3 : -1, i == 2 ? -3 : 1, 0, 1); }";
    ID3D11VertexShader *vs = NULL;
    if (comp && SUCCEEDED(comp(vsrc, sizeof vsrc - 1, "v", NULL, NULL, "main", "vs_5_0", 0, 0, &bc, NULL)) && bc) {
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bc), ID3D10Blob_GetBufferSize(bc), NULL, &vs);
        ID3D10Blob_Release(bc);
    }
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    D3D11_SAMPLER_DESC smd;
    ZeroMemory(&smd, sizeof smd);
    smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *smp = NULL;
    ID3D11Device_CreateSamplerState(dev, &smd, &smp);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    D3D11_VIEWPORT dvp = {0, 0, 640, 360, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &dvp);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    const float col[4] = {0, 0, 1, 1};
    for (int f = 0; f < 4; f++) {
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)bb, (ID3D11Resource *)scene);
        IDXGISwapChain_Present(sc, 0, 0);
    }
    if (ps) ID3D11PixelShader_Release(ps);
    if (vs) ID3D11VertexShader_Release(vs);
    if (smp) ID3D11SamplerState_Release(smp);
    ID3D11Texture2D_Release(scene);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(bb);
    ID3D11DeviceContext_Release(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);

    snprintf(path, sizeof path, "%s\\ui4k.log", argv[1]);
    FILE *lf = fopen(path, "rb");
    static char log[1 << 20];
    size_t n = lf ? fread(log, 1, sizeof log - 1, lf) : 0;
    if (lf) fclose(lf);
    log[n] = 0;
    CHECK(strstr(log, "diag: capture start"), "capture start");
    CHECK(strstr(log, "diag: capture end"), "capture end");
    char *d = strstr(log, " Draw 3/0 rt=[ 0=");
    CHECK(d && strstr(d, ":640x360* f28"), "draw line with scaled back buffer as rt 0");
    CHECK(strstr(log, " ClearRTV "), "clear");
    CHECK(d && strstr(d, "[SLP]") && strstr(d, " z=1(def)"), "shader flags SLP and default depth state in draw line");
    CHECK(strstr(log, " CopyResource dst="), "copy");
    char *e = strstr(log, "capture end");
    CHECK(!e || !strstr(e, " Draw 3/0"), "nothing logged after capture end");
    snprintf(path, sizeof path, "%s\\ui4k_cs.txt", argv[1]);
    lf = fopen(path, "rb");
    n = lf ? fread(log, 1, sizeof log - 1, lf) : 0;
    if (lf) fclose(lf);
    log[n] = 0;
    CHECK(strstr(log, "=== cs ") && strstr(log, "cs_5_0") && strstr(log, "dcl_thread_group 8, 8, 1"),
          "compute shader text in ui4k_cs.txt");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
