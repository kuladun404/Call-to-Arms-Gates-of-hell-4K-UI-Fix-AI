/* F07: viewport/scissor через собранную d3d11.dll при ×1.5 (WARP).
 *   test_vp.exe <папка с d3d11.dll и ui4k.ini>
 * Масштабированная цель — «превью юнита» 858x348 (класс UI_RT, физически 1287x522), обычная — 512x512.
 * Настоящее состояние читаем у настоящего контекста: он лежит в обёртке по смещению 8 (layout CtxWrap).
 * Рисование: треугольник на весь viewport; viewport = левая половина (429 логических) -> закрашено ровно
 * to_phys(429) = 644 физических столбца. Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                     LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3D11DeviceContext *real_of(ID3D11DeviceContext *w) { return *(ID3D11DeviceContext **)((char *)w + 8); }

static D3D11_VIEWPORT real_vp(ID3D11DeviceContext *c, UINT *n)
{
    D3D11_VIEWPORT v[16] = {0};
    UINT k = 16;
    ID3D11DeviceContext_RSGetViewports(real_of(c), &k, v);
    *n = k;
    return v[0];
}

static const char *SRC =
    "float4 vs(uint i : SV_VertexID) : SV_Position {"
    "  float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
    "float4 ps() : SV_Target { return float4(1, 0, 0, 1); }";

static ID3DBlob *compile(PFN_COMPILE f, const char *entry, const char *target)
{
    ID3DBlob *b = NULL, *e = NULL;
    HRESULT hr = f(SRC, strlen(SRC), "t", NULL, NULL, entry, target, 0, 0, &b, &e);
    if (FAILED(hr)) printf("compile %s: %s\n", entry, e ? (char *)ID3D10Blob_GetBufferPointer(e) : "?");
    return b;
}

static ID3D11Texture2D *rt(ID3D11Device *d, UINT w, UINT h, ID3D11RenderTargetView **v)
{
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(d, &td, NULL, &t);
    ID3D11Device_CreateRenderTargetView(d, (ID3D11Resource *)t, NULL, v);
    return t;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_vp <dir>\nFAIL\n"); return 2; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed\nFAIL\n"); return 1; }
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("device hr=0x%08lx\nFAIL\n", hr); return 1; }

    ID3D11RenderTargetView *va = NULL, *vb = NULL;
    ID3D11Texture2D *ta = rt(dev, 858, 348, &va), *tb = rt(dev, 512, 512, &vb);
    UINT n;
    D3D11_VIEWPORT v;

    /* 1. масштабированная цель: viewport в физике по краям */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &va, NULL);
    D3D11_VIEWPORT half = {0, 0, 429, 348, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &half);
    v = real_vp(ctx, &n);
    CHECK(n == 1 && v.TopLeftX == 0 && v.Width == 644 && v.Height == 522, "scaled vp real [%g %g %gx%g]", v.TopLeftX,
          v.TopLeftY, v.Width, v.Height);
    D3D11_VIEWPORT g[2];
    UINT gn = 2;
    ID3D11DeviceContext_RSGetViewports(ctx, &gn, g);
    CHECK(g[0].Width == 429 && g[0].Height == 348 && g[1].Width == 0, "RSGetViewports logical %gx%g", g[0].Width, g[0].Height);
    UINT cnt = 0;
    ID3D11DeviceContext_RSGetViewports(ctx, &cnt, NULL);
    CHECK(cnt == 1, "RSGetViewports count %u", cnt);
    /* правая половина: стык с левой без щели */
    D3D11_VIEWPORT right = {429, 0, 429, 348, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &right);
    v = real_vp(ctx, &n);
    CHECK(v.TopLeftX == 644 && v.TopLeftX + v.Width == 1287, "right half real [%g %gx%g]", v.TopLeftX, v.Width, v.Height);

    /* scissor: два соседних прямоугольника */
    D3D11_RECT sc[2] = {{0, 0, 429, 348}, {429, 0, 858, 348}}, rs[2];
    ID3D11DeviceContext_RSSetScissorRects(ctx, 2, sc);
    UINT rn = 2;
    ID3D11DeviceContext_RSGetScissorRects(real_of(ctx), &rn, rs);
    CHECK(rn == 2 && rs[0].right == 644 && rs[1].left == 644 && rs[1].right == 1287 && rs[1].bottom == 522,
          "scissor real [%ld..%ld] [%ld..%ld]x%ld", rs[0].left, rs[0].right, rs[1].left, rs[1].right, rs[1].bottom);
    rn = 2;
    ID3D11DeviceContext_RSGetScissorRects(ctx, &rn, rs);
    CHECK(rs[0].right == 429 && rs[1].right == 858, "RSGetScissorRects logical");

    /* 2. обычная цель: те же значения уходят как есть */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &vb, NULL);
    v = real_vp(ctx, &n);
    CHECK(v.TopLeftX == 429 && v.Width == 429, "unscaled target real vp [%g %g]", v.TopLeftX, v.Width);
    rn = 2;
    ID3D11DeviceContext_RSGetScissorRects(real_of(ctx), &rn, rs);
    CHECK(rs[1].left == 429 && rs[1].right == 858, "unscaled scissor [%ld..%ld]", rs[1].left, rs[1].right);
    /* снятие целей не меняет режим */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    v = real_vp(ctx, &n);
    CHECK(v.TopLeftX == 429, "no targets keeps state [%g]", v.TopLeftX);
    /* обратно на масштабированную */
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &va, NULL);
    v = real_vp(ctx, &n);
    CHECK(v.TopLeftX == 644, "back to scaled [%g]", v.TopLeftX);

    /* 3. ClearState */
    ID3D11DeviceContext_ClearState(ctx);
    cnt = 5;
    ID3D11DeviceContext_RSGetViewports(ctx, &cnt, NULL);
    real_vp(ctx, &n);
    CHECK(cnt == 0 && n == 0, "ClearState: logical %u real %u viewports", cnt, n);

    /* 4. рисование: viewport — левая половина, закрашено ровно 644 физических столбца */
    PFN_COMPILE comp = (PFN_COMPILE)GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile");
    ID3DBlob *bv = comp ? compile(comp, "vs", "vs_4_0") : NULL, *bp = comp ? compile(comp, "ps", "ps_4_0") : NULL;
    CHECK(bv && bp, "shader compile");
    if (bv && bp) {
        ID3D11VertexShader *vs = NULL;
        ID3D11PixelShader *ps = NULL;
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &ps);
        const float black[4] = {0, 0, 0, 0};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, va, black);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &va, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &half);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        D3D11_TEXTURE2D_DESC sd = {1287, 522, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                   D3D11_CPU_ACCESS_READ, 0};
        ID3D11Texture2D *st = NULL;
        ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)ta);
        D3D11_MAPPED_SUBRESOURCE mp;
        if (st && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
            const UINT *row = (const UINT *)((BYTE *)mp.pData + mp.RowPitch * 261);
            const UINT *last = (const UINT *)((BYTE *)mp.pData + mp.RowPitch * 521);
            CHECK(row[0] == 0xFF0000FF && row[643] == 0xFF0000FF && row[644] == 0 && row[1286] == 0,
                  "draw: col0=%08X col643=%08X col644=%08X col1286=%08X", row[0], row[643], row[644], row[1286]);
            CHECK(last[100] == 0xFF0000FF, "draw: bottom row covered %08X", last[100]);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        } else
            CHECK(0, "readback map");
        if (st) ID3D11Texture2D_Release(st);
        ID3D11VertexShader_Release(vs);
        ID3D11PixelShader_Release(ps);
    }

    /* 5. deferred: состояние своё; FinishCommandList(FALSE) его сбрасывает; immediate не тронут */
    ID3D11DeviceContext *def = NULL;
    ID3D11Device_CreateDeferredContext(dev, 0, &def);
    ID3D11DeviceContext_OMSetRenderTargets(def, 1, &va, NULL);
    ID3D11DeviceContext_RSSetViewports(def, 1, &right);
    v = real_vp(def, &n);
    CHECK(v.TopLeftX == 644, "deferred scaled vp [%g]", v.TopLeftX);
    ID3D11CommandList *cl = NULL;
    ID3D11DeviceContext_FinishCommandList(def, FALSE, &cl);
    cnt = 5;
    ID3D11DeviceContext_RSGetViewports(def, &cnt, NULL);
    CHECK(cnt == 0, "deferred after FinishCommandList(FALSE): %u viewports", cnt);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &half);
    ID3D11DeviceContext_ExecuteCommandList(ctx, cl, TRUE);
    gn = 1;
    ID3D11DeviceContext_RSGetViewports(ctx, &gn, g);
    CHECK(g[0].Width == 429 && g[0].TopLeftX == 0, "immediate after Execute(restore=TRUE) keeps its vp");
    ID3D11DeviceContext_ExecuteCommandList(ctx, cl, FALSE);
    cnt = 5;
    ID3D11DeviceContext_RSGetViewports(ctx, &cnt, NULL);
    CHECK(cnt == 0, "immediate after Execute(restore=FALSE): %u viewports", cnt);
    ID3D11CommandList_Release(cl);
    ID3D11DeviceContext_Release(def);

    /* дробный viewport — умножение */
    D3D11_VIEWPORT fr = {10.5f, 0, 100, 50, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &va, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &fr);
    v = real_vp(ctx, &n);
    /* края 10.5 и 110.5 дробные -> x1.5 = 15.75 и 165.75; низ 50 целый -> 75 */
    CHECK(v.TopLeftX == 15.75f && v.Width == 150.0f && v.Height == 75.0f, "fractional vp [%g %gx%g]", v.TopLeftX,
          v.Width, v.Height);

    ID3D11RenderTargetView_Release(va); ID3D11RenderTargetView_Release(vb);
    ID3D11Texture2D_Release(ta); ID3D11Texture2D_Release(tb);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
