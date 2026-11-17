/* Форматы пикселей и пересчёт картинки процессором (В0: граница «процессор <-> видеокарта»). Без состояния. */
#include <string.h>
#include "fmt.h"

UINT fmt_bytes(DXGI_FORMAT f)
{
    switch ((int)f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 4;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 0;
    }
}

int fmt_lerp8(DXGI_FORMAT f)
{
    switch ((int)f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 0;
    }
}

/* семейства, которые умеем растягивать шейдером: {typeless, формат вида по умолчанию, типизированные...} */
static const DXGI_FORMAT FAM[][5] = {
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0},
    {DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, 0},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 0},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM, 0},
    {DXGI_FORMAT_R32G32B32A32_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0},
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_UNORM, 0},
    {DXGI_FORMAT_R32G32_TYPELESS, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, 0, 0},
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, 0, 0},
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, 0, 0},
    /* R16 и R32 typeless бывают глубиной — по умолчанию не растягиваем (0), типизированные можно */
    {DXGI_FORMAT_R16_TYPELESS, 0, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UNORM, 0},
    {DXGI_FORMAT_R32_TYPELESS, 0, DXGI_FORMAT_R32_FLOAT, 0, 0},
    {0, 0, DXGI_FORMAT_R11G11B10_FLOAT, 0, 0},
    {0, 0, DXGI_FORMAT_A8_UNORM, 0, 0},
};
#define NFAM ((int)(sizeof FAM / sizeof *FAM))

/* семейство типизированного формата (-1 — не растягиваем) */
static int fam_typed(DXGI_FORMAT f)
{
    if (!f) return -1;
    for (int i = 0; i < NFAM; i++)
        for (int j = 2; j < 5; j++)
            if (FAM[i][j] == f) return i;
    return -1;
}

static int fam_typeless(DXGI_FORMAT f)
{
    if (!f) return -1;
    for (int i = 0; i < NFAM; i++)
        if (FAM[i][0] == f) return i;
    return -1;
}

DXGI_FORMAT fmt_view(DXGI_FORMAT a, DXGI_FORMAT b)
{
    int ta = fam_typed(a), tb = fam_typed(b), la = fam_typeless(a), lb = fam_typeless(b);
    if (ta >= 0 && tb >= 0) return a == b ? a : DXGI_FORMAT_UNKNOWN;   /* sRGB <-> UNORM шейдером исказит цвета */
    if (ta >= 0 && lb == ta) return a;
    if (tb >= 0 && la == tb) return b;
    if (la >= 0 && la == lb) return FAM[la][1];
    return DXGI_FORMAT_UNKNOWN;
}

void px_resample(const BYTE *src, UINT sw, UINT sh, UINT spitch, BYTE *dst, UINT dw, UINT dh, UINT dpitch, UINT bpp, int lerp8)
{
    if (!src || !dst || !sw || !sh || !dw || !dh || !bpp) return;
    if (sw == dw && sh == dh) {
        for (UINT y = 0; y < sh; y++) memcpy(dst + (size_t)y * dpitch, src + (size_t)y * spitch, (size_t)sw * bpp);
        return;
    }
    /* центр точки назначения в координатах источника: (i + 0.5) * s / d - 0.5, в 32.32 */
    const long long stx = (long long)(((unsigned long long)sw << 32) / dw), sty = (long long)(((unsigned long long)sh << 32) / dh);
    for (UINT y = 0; y < dh; y++) {
        long long py = (long long)y * sty + sty / 2;
        BYTE *o = dst + (size_t)y * dpitch;
        if (!lerp8) {
            UINT sy = (UINT)(py >> 32);
            if (sy >= sh) sy = sh - 1;
            const BYTE *row = src + (size_t)sy * spitch;
            for (UINT x = 0; x < dw; x++, o += bpp) {
                UINT sx = (UINT)(((long long)x * stx + stx / 2) >> 32);
                if (sx >= sw) sx = sw - 1;
                memcpy(o, row + (size_t)sx * bpp, bpp);
            }
            continue;
        }
        py -= 1LL << 31;
        if (py < 0) py = 0;
        UINT y0 = (UINT)(py >> 32), wy = (UINT)((py >> 24) & 0xff);
        if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
        const BYTE *r0 = src + (size_t)y0 * spitch, *r1 = wy ? r0 + spitch : r0;
        for (UINT x = 0; x < dw; x++, o += bpp) {
            long long px = (long long)x * stx + stx / 2 - (1LL << 31);
            if (px < 0) px = 0;
            UINT x0 = (UINT)(px >> 32), wx = (UINT)((px >> 24) & 0xff);
            if (x0 >= sw - 1) { x0 = sw - 1; wx = 0; }
            const BYTE *a = r0 + (size_t)x0 * bpp, *c = r1 + (size_t)x0 * bpp;
            const BYTE *b = wx ? a + bpp : a, *d = wx ? c + bpp : c;
            for (UINT i = 0; i < bpp; i++) {
                UINT top = a[i] * (256 - wx) + b[i] * wx, bot = c[i] * (256 - wx) + d[i] * wx;
                o[i] = (BYTE)((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
}
