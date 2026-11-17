/* F09: копии через собранную d3d11.dll при ×1.5 (WARP).
 *   test_copy.exe <папка с d3d11.dll и ui4k.ini>
 * Масштабированные цели — класс UI_RT (не квадрат, не pow2): экран не нужен. Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <d3d11.h>

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11DeviceContext *real_of(ID3D11DeviceContext *w) { return *(ID3D11DeviceContext **)((char *)w + 8); }

static ID3D11Texture2D *tex(UINT w, UINT h, UINT bind, const UINT *fill)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {fill, w * 4, 0};
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, fill ? &sd : NULL, &t);
    CHECK(SUCCEEDED(hr), "create %ux%u hr=0x%08lx", w, h, hr);
    return t;
}

/* настоящие пиксели текстуры физического размера pw x ph */
static UINT *read(ID3D11Texture2D *t, UINT pw, UINT ph)
{
    D3D11_TEXTURE2D_DESC d = {pw, ph, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *st = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &st);   /* staging не экранного размера — не масштабируется */
    ID3D11DeviceContext_CopyResource(real_of(ctx), (ID3D11Resource *)st, (ID3D11Resource *)t);   /* мимо хуков */
    UINT *px = (UINT *)calloc((size_t)pw * ph, 4);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < ph; y++) memcpy(px + y * pw, (BYTE *)m.pData + y * m.RowPitch, pw * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return px;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_copy <dir>\nFAIL\n"); return 2; }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed\nFAIL\n"); return 1; }
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");
    if (FAILED(cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    const UINT RED = 0xFF0000FF, BLUE = 0xFFFF0000, RT = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    /* 1. иконка инвентаря: игра рисует предмет с запасом в обычную 512x512 (квадрат — не масштабируется), область
     *    222x76 (левая половина красная, правая синяя) копирует в промежуточную 222x76 — её мы увеличили до 333x114.
     *    Копия растягивается на всю физическую область, а не ложится в угол. */
    UINT *src = (UINT *)malloc(512 * 512 * 4);
    for (int y = 0; y < 512; y++)
        for (int x = 0; x < 512; x++) src[y * 512 + x] = x < 111 ? RED : BLUE;
    ID3D11Texture2D *s512 = tex(512, 512, RT, src);
    ID3D11Texture2D *icon = tex(222, 76, RT, NULL);
    D3D11_VIEWPORT keep = {3, 4, 50, 60, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &keep);
    D3D11_VIEWPORT rk[1];
    UINT rn = 1;
    ID3D11DeviceContext_RSGetViewports(real_of(ctx), &rn, rk);
    D3D11_BOX box = {0, 0, 0, 222, 76, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)icon, 0, 0, 0, 0, (ID3D11Resource *)s512, 0, &box);
    UINT *p = read(icon, 333, 114);
    CHECK(p[0] == RED && p[113 * 333 + 332] == BLUE && p[57 * 333 + 80] == RED && p[57 * 333 + 250] == BLUE,
          "stretched icon: corners %08X %08X mid %08X %08X", p[0], p[113 * 333 + 332], p[57 * 333 + 80], p[57 * 333 + 250]);
    free(p);
    /* состояние игры восстановлено */
    D3D11_VIEWPORT after[1];
    UINT an = 1;
    ID3D11DeviceContext_RSGetViewports(real_of(ctx), &an, after);
    CHECK(an == rn && after[0].TopLeftX == rk[0].TopLeftX && after[0].Width == rk[0].Width, "viewport restored");

    /* 2. обе масштабированы: box и точка в физику по краям */
    ID3D11Texture2D *a = tex(858, 348, RT, NULL), *b = tex(858, 348, RT, NULL);
    ID3D11RenderTargetView *va = NULL, *vb = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)a, NULL, &va);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)b, NULL, &vb);
    const float red[4] = {1, 0, 0, 1}, black[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, va, red);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, vb, black);
    D3D11_BOX b2 = {0, 0, 0, 50, 20, 1};
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)b, 0, 100, 10, 0, (ID3D11Resource *)a, 0, &b2);
    p = read(b, 1287, 522);
    /* x: 100 -> 150, 150 -> 225; y: 10 -> 15, 30 -> 45 */
    CHECK(p[15 * 1287 + 150] == RED && p[44 * 1287 + 224] == RED && p[15 * 1287 + 149] == 0 && p[15 * 1287 + 225] == 0 &&
          p[45 * 1287 + 150] == 0 && p[14 * 1287 + 150] == 0, "scaled->scaled region");
    free(p);

    /* 3. CopyResource обычная -> масштабированная того же логического размера: растяжение целиком */
    UINT *fill = (UINT *)malloc(858 * 348 * 4);
    for (int i = 0; i < 858 * 348; i++) fill[i] = BLUE;
    ID3D11Texture2D *plain = tex(858, 348, RT, fill);   /* с данными — не масштабируется */
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)a, (ID3D11Resource *)plain);
    p = read(a, 1287, 522);
    CHECK(p[0] == BLUE && p[521 * 1287 + 1286] == BLUE && p[260 * 1287 + 643] == BLUE, "CopyResource stretched whole");
    free(p);

    ID3D11RenderTargetView_Release(va); ID3D11RenderTargetView_Release(vb);
    ID3D11Texture2D_Release(a); ID3D11Texture2D_Release(b); ID3D11Texture2D_Release(plain);
    ID3D11Texture2D_Release(s512); ID3D11Texture2D_Release(icon);
    free(src); free(fill);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
