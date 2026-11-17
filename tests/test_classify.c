/* F05: правила классификации текстур (чистая функция). Последняя строка PASS/FAIL. */
#include <stdio.h>
#include "classify.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static D3D11_TEXTURE2D_DESC tex(UINT w, UINT h, UINT bind, D3D11_USAGE usage)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, usage, bind, 0, 0};
    if (usage == D3D11_USAGE_STAGING) d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return d;
}

static void expect(const char *name, D3D11_TEXTURE2D_DESC d, int init, UINT lw, UINT lh, Ratio k, TexClass want)
{
    TexClass c = tex_classify(&d, init, lw, lh, k);
    CHECK(c == want, "%s: %ux%u bind=0x%x -> %d, want %d", name, d.Width, d.Height, d.BindFlags, c, want);
}

int main(void)
{
    Ratio k15 = {3, 2}, k2 = {2, 1}, k1 = {1, 1};
    const UINT RT = D3D11_BIND_RENDER_TARGET, SRV = D3D11_BIND_SHADER_RESOURCE, DS = D3D11_BIND_DEPTH_STENCIL,
               UAV = D3D11_BIND_UNORDERED_ACCESS;
    UINT lw = 2560, lh = 1440;   /* 4K при ×1.5 */

    expect("screen RT", tex(2560, 1440, RT | SRV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_SCREEN);
    expect("half RT", tex(1280, 720, RT | SRV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_SCREEN);
    expect("1/16 UAV", tex(160, 90, UAV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_SCREEN);
    expect("screen DS", tex(2560, 1440, DS, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_SCREEN);
    /* В3: то, что игра читает или пишет процессором, остаётся её размера */
    expect("screen staging", tex(2560, 1440, 0, D3D11_USAGE_STAGING), 0, lw, lh, k15, TEX_KEEP);
    D3D11_TEXTURE2D_DESC dyn = tex(2560, 1440, SRV, D3D11_USAGE_DYNAMIC);
    dyn.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    expect("screen dynamic", dyn, 0, lw, lh, k15, TEX_KEEP);
    D3D11_TEXTURE2D_DESC cpu = tex(858, 348, RT, D3D11_USAGE_DEFAULT);
    cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    expect("RT with CPU access", cpu, 0, lw, lh, k15, TEX_KEEP);
    expect("screen SRV only", tex(2560, 1440, SRV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_KEEP);
    expect("screen RT with data", tex(2560, 1440, RT, D3D11_USAGE_DEFAULT), 1, lw, lh, k15, TEX_KEEP);
    /* нечётный экран: /2 с округлением вверх и вниз */
    expect("odd /2 ceil", tex(853, 480, RT, D3D11_USAGE_DEFAULT), 0, 1706, 960, k15, TEX_SCREEN);
    expect("odd /4 floor", tex(426, 240, RT, D3D11_USAGE_DEFAULT), 0, 1706, 960, k15, TEX_SCREEN);

    expect("unit preview", tex(858, 348, RT | SRV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_UI_RT);
    expect("unit preview depth", tex(858, 348, DS, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_UI_RT);
    expect("shadow map square", tex(2048, 2048, DS, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_KEEP);
    expect("pow2 x pow2 RT", tex(1024, 512, RT, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_KEEP);
    expect("UI image (SRV)", tex(144, 72, SRV, D3D11_USAGE_DEFAULT), 0, lw, lh, k15, TEX_KEEP);
    D3D11_TEXTURE2D_DESC arr = tex(858, 348, RT, D3D11_USAGE_DEFAULT);
    arr.ArraySize = 2;
    expect("RT array", arr, 0, lw, lh, k15, TEX_KEEP);
    D3D11_TEXTURE2D_DESC ms = tex(858, 348, RT, D3D11_USAGE_DEFAULT);
    ms.SampleDesc.Count = 4;
    expect("RT MSAA: same as its plain pair", ms, 0, lw, lh, k15, TEX_UI_RT);
    D3D11_TEXTURE2D_DESC sms = tex(2560, 1440, RT, D3D11_USAGE_DEFAULT);
    sms.SampleDesc.Count = 8;
    expect("screen RT MSAA", sms, 0, lw, lh, k15, TEX_SCREEN);

    /* экран неизвестен: только UI_RT-правило */
    expect("no screen yet", tex(1920, 1080, RT, D3D11_USAGE_DEFAULT), 0, 0, 0, k2, TEX_UI_RT);
    /* масштаб 1: ничего */
    expect("scale 1", tex(2560, 1440, RT, D3D11_USAGE_DEFAULT), 0, lw, lh, k1, TEX_KEEP);
    /* физический размер вышел бы за 16384 */
    expect("too big", tex(4097, 4095, RT, D3D11_USAGE_DEFAULT), 0, 4097, 4095, (Ratio){4, 1}, TEX_KEEP);

    /* физическое описание */
    D3D11_TEXTURE2D_DESC p = tex_phys_desc(&(D3D11_TEXTURE2D_DESC){1281, 721, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                                                 D3D11_USAGE_DEFAULT, RT, 0, 0}, k15);
    CHECK(p.Width == 1922 && p.Height == 1082 && p.MipLevels == 11, "phys desc %ux%u mips %u", p.Width, p.Height, p.MipLevels);

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
