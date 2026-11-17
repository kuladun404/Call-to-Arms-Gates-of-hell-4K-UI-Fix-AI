/* Копии шейдеров автоэкспозиции (lum.c) на WARP, без мода.
 * Картинка мира 64x64: левая верхняя четверть чёрная, остальное — яркость 1. Игра считает, что размер 32x32
 * (сетка 1 группа 32x32). Копии должны увидеть всю картинку: средняя 0.75, гистограмма 256 в корзине 0 и 768
 * в корзине яркости 1. Шейдер игры увидел бы только чёрный угол. И для картинки 32x32 — то же самое (без
 * растяжения отсчёты совпадают с игрой пиксель в пиксель). Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include "lum_cs.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11Texture2D *tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void *data, UINT pitch)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    D3D11_SUBRESOURCE_DATA s = {data, pitch, 0};
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, data ? &s : NULL, &t);
    return t;
}

static ID3D11UnorderedAccessView *uav(ID3D11Texture2D *t)
{
    ID3D11UnorderedAccessView *u = NULL;
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)t, NULL, &u);
    return u;
}

static void readback(ID3D11Texture2D *t, UINT w, UINT h, DXGI_FORMAT f, void *out, UINT rowbytes)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *s = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &s);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m);
    for (UINT y = 0; y < h; y++) memcpy((char *)out + y * rowbytes, (char *)m.pData + y * m.RowPitch, rowbytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0);
    ID3D11Texture2D_Release(s);
}

static void run(UINT sw, const char *what)
{
    static float img[64 * 64 * 4];
    for (UINT y = 0; y < sw; y++)
        for (UINT x = 0; x < sw; x++) {
            float v = (x < sw / 2 && y < sw / 2) ? 0.f : 1.f;
            for (int c = 0; c < 3; c++) img[(y * sw + x) * 4 + c] = v;
            img[(y * sw + x) * 4 + 3] = 1;
        }
    ID3D11Texture2D *scene = tex(sw, sw, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, img, sw * 16);
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)scene, NULL, &srv);
    float cbd[8] = {32, 32, 32, 32, 0, 0, 0, 0};
    D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA bs = {cbd, 0, 0};
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &bs, &cb);

    /* шаг 1: плитка — средняя и наибольшая яркость */
    ID3D11Texture2D *bed = tex(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, NULL, 0);
    ID3D11UnorderedAccessView *ub = uav(bed);
    ID3D11ComputeShader *cs = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, lum_tile, sizeof lum_tile, NULL, &cs)), "%s: create tile", what);
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 2, 1, &ub, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    UINT v = 0;
    readback(bed, 1, 1, DXGI_FORMAT_R32_UINT, &v, 4);
    CHECK(v >> 16 == 6553, "%s: tile max %u, want 6553", what, v >> 16);
    CHECK((v & 0xffff) == 4915, "%s: tile average %u (=%.3f), want 4915 (=0.75)", what, v & 0xffff, (v & 0xffff) / 6553.0);
    ID3D11ComputeShader_Release(cs);

    /* шаг 2: гистограмма, 1024 корзины; масштаб 1024/10 -> яркость 1 в корзине 102 */
    static UINT zeros[1024];
    float meta[4] = {10, 0, 0, 0};
    ID3D11Texture2D *bins = tex(1024, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, zeros, 4096);
    ID3D11Texture2D *yr = tex(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, zeros, 4);
    ID3D11Texture2D *mt = tex(4, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, meta, 16);
    ID3D11UnorderedAccessView *u[4] = {uav(bins), uav(yr), NULL, uav(mt)};
    cbd[2] = 1;  /* histoData.z > 0: считать наибольшую корзину */
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, cbd, 0, 0);
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, lum_hist, sizeof lum_hist, NULL, &cs)), "%s: create hist", what);
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 4, u, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    static UINT h[1024];
    readback(bins, 1024, 1, DXGI_FORMAT_R32_UINT, h, 4096);
    UINT total = 0;
    for (int i = 0; i < 1024; i++) total += h[i];
    CHECK(total == 1024, "%s: histogram total %u, want 1024", what, total);
    CHECK(h[0] == 256 && h[102] == 768, "%s: bins[0]=%u bins[102]=%u, want 256 / 768", what, h[0], h[102]);
    readback(yr, 1, 1, DXGI_FORMAT_R32_UINT, &v, 4);
    CHECK(v > 0 && v <= 768, "%s: y range %u", what, v);
    ID3D11ComputeShader_Release(cs);
    ID3D11UnorderedAccessView *nu[4] = {0};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 4, nu, NULL);
    for (int i = 0; i < 4; i++) if (u[i]) ID3D11UnorderedAccessView_Release(u[i]);
    ID3D11UnorderedAccessView_Release(ub);
    ID3D11Texture2D_Release(bins); ID3D11Texture2D_Release(yr); ID3D11Texture2D_Release(mt); ID3D11Texture2D_Release(bed);
    ID3D11Buffer_Release(cb); ID3D11ShaderResourceView_Release(srv); ID3D11Texture2D_Release(scene);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    run(64, "scene x2");
    run(32, "scene x1");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
