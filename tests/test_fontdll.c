/* Э6 через собранную d3d11.dll (WARP), ревизия v3: чёткие буквы — весь путь игры, ui4k.ini без остальных ключей.
 *   test_fontdll.exe <папка с d3d11.dll>
 * Как в игре (реверс «Интерфейс — как рисуется»): страница BMFont заливается UpdateSubresource в промежуточную текстуру,
 * CopyResource в рабочую (виды созданы заранее); полоса букв — UpdateSubresource с областью в общий буфер вершин (шаг 36),
 * Draw полосой при виде страницы в слоте 0 PS, цель — кэш окна (увеличен x2). Образцы — свой ui4k_fonts2.dat в папке.
 * Проверки (по логу мода):
 *  - п. 1: presets=0, lum_fix=0, dof_radius=0, thick_lines=0 — свои буквы всё равно рисуются;
 *  - п. 5: страница с тем же отпечатком по каждому 97-му байту, но другим байтом между ними — не своя (CRC32);
 *  - п. 4: буфер вершин, залитый отложенным контекстом, мод не копирует — рисование остаётся игре;
 *  - п. 6: промежуточную текстуру игра отпускает до копии — страница всё равно узнаётся (мод держит ссылку).
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "fontpages.h"

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_C)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
                               ID3DBlob **, ID3DBlob **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PW 64
static BYTE g_page[PW * PW * 4];   /* BGRA, строки сверху вниз */

static void put32(BYTE *p, UINT v) { memcpy(p, &v, 4); }
static void put16(BYTE *p, UINT v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }

/* страница: шум (чтобы отпечаток был своим) и буква — ячейка (8, 8) 8x8 в канале G (байт 1) */
static void make_page(void)
{
    UINT s = 12345;
    for (int i = 0; i < PW * PW * 4; i++) { s = s * 1103515245u + 12345u; g_page[i] = (BYTE)((s >> 16) & 0x0F); }
    for (int y = 8; y < 16; y++)
        for (int x = 8; x < 16; x++) g_page[(y * PW + x) * 4 + 1] = 200;
}

/* ui4k_fonts2.dat: одна страница, одна буква — картинка x2 16x16 на месте ячейки */
static int write_dat(const char *dir)
{
    static BYTE f[4096];
    memcpy(f, "U4KF", 4); put32(f + 4, 3); put32(f + 8, 2); put32(f + 12, 1);
    uint64_t h = fp_hash97(g_page, PW, PW, PW * 4);
    put32(f + 16, PW); put32(f + 20, PW); put32(f + 24, (UINT)h); put32(f + 28, (UINT)(h >> 32));
    put32(f + 32, 1); put32(f + 36, 48); put32(f + 40, fp_crc32(g_page, PW, PW, PW * 4)); put32(f + 44, 0);
    BYTE *g = f + 48;
    put16(g, 8); put16(g + 2, 8); put16(g + 4, 8); put16(g + 6, 8); g[8] = 1; g[9] = 0;
    put16(g + 10, 0); put16(g + 12, 0); put16(g + 14, 16); put16(g + 16, 16);
    for (int i = 0; i < 256; i++) g[18 + i] = (BYTE)(i % 16 < 8 ? 255 : 100);
    size_t n = 48 + 18 + 256;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k_fonts2.dat", dir);
    FILE *o = fopen(path, "wb");
    if (!o) return 0;
    fwrite(f, 1, n, o);
    fclose(o);
    return 1;
}

static ID3D11Texture2D *tex(ID3D11Device *d, UINT w, UINT h, UINT bind)
{
    D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(d, &td, NULL, &t);
    return t;
}

/* вершина движка: x, y, z | 12 байт | маска канала | u, v */
static void vtx(BYTE *v, float x, float y, float u, float w)
{
    float p[6] = {x, y, 0.5f, 0, 0, 1};
    memcpy(v, p, 24);
    BYTE mask[4] = {0, 255, 0, 0};
    memcpy(v + 24, mask, 4);
    memcpy(v + 28, &u, 4);
    memcpy(v + 32, &w, 4);
}

static int log_has(const char *dir, const char *what)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k.log", dir);
    static char log[1 << 20];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f) fclose(f);
    log[n] = 0;
    return strstr(log, what) != NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_fontdll <dir>\nFAIL\n"); return 2; }
    const char *dir = argv[1];
    make_page();
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k.ini", dir);
    FILE *f = fopen(path, "wb");
    fprintf(f, "[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=2\nlog=1\nfit_native=0\npresets=0\nlum_fix=0\ndof_radius=0\n"
               "thick_lines=0\nfonts=1\nlabels=0\norigin=0\nui_refresh=0\n");
    fclose(f);
    CHECK(write_dat(dir), "sample file written");
    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    CHECK(SUCCEEDED(hr) && dev && ctx, "CreateDevice hr=0x%08lx", hr);
    if (!dev || !ctx) { printf("FAIL\n"); return 1; }

    /* шейдеры как у TextPS: a = dot(страница, маска) */
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_C comp = dc ? (PFN_C)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    static const char src[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);"
        "struct V { float3 p : POSITION; float3 q : TEXCOORD1; float4 m : COLOR; float2 uv : TEXCOORD0; };"
        "struct O { float4 p : SV_Position; float4 m : COLOR; float2 uv : TEXCOORD0; };"
        "O vs(V v) { O o; o.p = float4(v.p.x / 150 - 1, 1 - v.p.y / 50, 0.5, 1); o.m = v.m; o.uv = v.uv; return o; }"
        "float4 ps(O i) : SV_Target { float a = dot(t.Sample(s, i.uv), i.m); return float4(a, a, a, a); }";
    ID3DBlob *bv = NULL, *bp = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *il = NULL;
    if (comp && SUCCEEDED(comp(src, sizeof src - 1, "f", NULL, NULL, "vs", "vs_4_0", 0, 0, &bv, NULL)) &&
        SUCCEEDED(comp(src, sizeof src - 1, "f", NULL, NULL, "ps", "ps_4_0", 0, 0, &bp, NULL))) {
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &ps);
        D3D11_INPUT_ELEMENT_DESC ie[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ID3D11Device_CreateInputLayout(dev, ie, 4, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), &il);
    }
    CHECK(vs && ps && il, "text shaders made");

    /* п. 5: страница другого мода — тот же отпечаток по каждому 97-му байту, другой байт между ними */
    {
        static BYTE other[PW * PW * 4];
        memcpy(other, g_page, sizeof other);
        other[(9 * PW + 9) * 4 + 1] ^= 0x40;   /* байт внутри буквы; его номер не кратен 97 */
        CHECK(fp_hash97(other, PW, PW, PW * 4) == fp_hash97(g_page, PW, PW, PW * 4), "test page keeps the sampled fingerprint");
        ID3D11Texture2D *stg = tex(dev, PW, PW, 0);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)stg, 0, NULL, other, PW * 4, 0);
        ID3D11Texture2D_Release(stg);
    }
    CHECK(log_has(dir, "has the fingerprint of a sample page but other pixels"), "a page changed by another mod is not taken (CRC32)");

    /* страница игры: виды заранее, заливка в промежуточную, игра отпускает её до копии (п. 6), копия в рабочую */
    ID3D11Texture2D *work = tex(dev, PW, PW, D3D11_BIND_SHADER_RESOURCE);
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)work, NULL, &srv);
    ID3D11Texture2D *stg = tex(dev, PW, PW, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)stg, 0, NULL, g_page, PW * 4, 0);
    ID3D11Texture2D *keep = stg;
    ID3D11Texture2D_AddRef(keep);
    ULONG left = ID3D11Texture2D_Release(stg);   /* ссылка игры ушла; мод держит свою */
    CHECK(left >= 2, "the mod holds the waiting page (refs left %lu)", left);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)work, (ID3D11Resource *)keep);
    ID3D11Texture2D_Release(keep);

    /* кэш окна 300x100 (не квадрат, не степени двойки — x2) */
    ID3D11Texture2D *rt = tex(dev, 300, 100, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, 300, 100, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    /* полоса: одна буква в её ячейку x1 (20, 10)-(28, 18), UV ячейки */
    BYTE v[4 * 36];
    float u0 = 8.0f / PW, u1 = 16.0f / PW;
    vtx(v, 20, 10, u0, u0); vtx(v + 36, 28, 10, u1, u0); vtx(v + 72, 20, 18, u0, u1); vtx(v + 108, 28, 18, u1, u1);
    D3D11_BUFFER_DESC bd = {4096, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    ID3D11Buffer *vb = NULL, *vb2 = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb2);
    D3D11_BOX box = {0, 0, 0, sizeof v, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)vb, 0, &box, v, 0, 0);
    UINT stride = 36, off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
    ID3D11DeviceContext_Flush(ctx);
    CHECK(log_has(dir, "drawn with own letter cells (x2/1; first draw: 1 own"),
          "own letter drawn with presets, lum_fix, dof_radius and thick_lines off (review v3, 1)");

    /* п. 4: тот же текст в другом буфере, залитом отложенным контекстом — копии нет, рисование остаётся игре */
    ID3D11DeviceContext *def = NULL;
    if (SUCCEEDED(ID3D11Device_CreateDeferredContext(dev, 0, &def)) && def) {
        ID3D11DeviceContext_UpdateSubresource(def, (ID3D11Resource *)vb2, 0, &box, v, 0, 0);
        ID3D11CommandList *cl = NULL;
        ID3D11DeviceContext_FinishCommandList(def, FALSE, &cl);
        if (cl) { ID3D11DeviceContext_ExecuteCommandList(ctx, cl, TRUE); ID3D11CommandList_Release(cl); }
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb2, &stride, &off);
        ID3D11DeviceContext_Draw(ctx, 4, 0);
        ID3D11DeviceContext_Release(def);
        CHECK(log_has(dir, "a text draw from a vertex buffer the mod has no copy of"),
              "vertex buffer filled by a deferred context: no copy, the game's letters (review v3, 4)");
    }

    ID3D11Buffer_Release(vb); ID3D11Buffer_Release(vb2);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(rt);
    ID3D11ShaderResourceView_Release(srv); ID3D11Texture2D_Release(work);
    if (il) ID3D11InputLayout_Release(il);
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
