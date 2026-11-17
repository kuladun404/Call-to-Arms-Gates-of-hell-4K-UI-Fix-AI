/* Форматы пикселей и пересчёт картинки процессором (src/fmt.c, чистые функции). Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fmt.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* размер точки */
    CHECK(fmt_bytes(DXGI_FORMAT_R8G8B8A8_UNORM) == 4 && fmt_bytes(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == 4, "8888 = 4 bytes");
    CHECK(fmt_bytes(DXGI_FORMAT_R16G16B16A16_FLOAT) == 8 && fmt_bytes(DXGI_FORMAT_R32G32B32A32_FLOAT) == 16, "float targets");
    CHECK(fmt_bytes(DXGI_FORMAT_R11G11B10_FLOAT) == 4 && fmt_bytes(DXGI_FORMAT_R8_UNORM) == 1 && fmt_bytes(DXGI_FORMAT_R16_FLOAT) == 2,
          "small formats");
    CHECK(fmt_bytes(DXGI_FORMAT_BC1_UNORM) == 0 && fmt_bytes(DXGI_FORMAT_BC3_UNORM) == 0 && fmt_bytes(DXGI_FORMAT_BC7_UNORM) == 0 &&
          fmt_bytes(DXGI_FORMAT_UNKNOWN) == 0, "compressed and unknown formats cannot be converted");

    /* побайтное смешивание — только 8-битные нормализованные каналы */
    CHECK(fmt_lerp8(DXGI_FORMAT_R8G8B8A8_UNORM) && fmt_lerp8(DXGI_FORMAT_B8G8R8X8_UNORM) && fmt_lerp8(DXGI_FORMAT_R8_UNORM), "lerp8 yes");
    CHECK(!fmt_lerp8(DXGI_FORMAT_R16G16B16A16_FLOAT) && !fmt_lerp8(DXGI_FORMAT_R11G11B10_FLOAT) && !fmt_lerp8(DXGI_FORMAT_R8G8B8A8_UINT) &&
          !fmt_lerp8(DXGI_FORMAT_R10G10B10A2_UNORM), "lerp8 no");

    /* формат видов для растяжения шейдером */
    CHECK(fmt_view(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM) == DXGI_FORMAT_R8G8B8A8_UNORM, "same typed");
    CHECK(fmt_view(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
          fmt_view(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS) == DXGI_FORMAT_B8G8R8A8_UNORM, "typeless + typed");
    CHECK(fmt_view(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_TYPELESS) == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "both typeless: family default");
    CHECK(fmt_view(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_UNKNOWN, "sRGB <-> UNORM: no");
    CHECK(fmt_view(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM) == DXGI_FORMAT_UNKNOWN, "different families: no");
    CHECK(fmt_view(DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT) == DXGI_FORMAT_UNKNOWN &&
          fmt_view(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS) == DXGI_FORMAT_UNKNOWN &&
          fmt_view(DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT) == DXGI_FORMAT_UNKNOWN &&
          fmt_view(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_TYPELESS) == DXGI_FORMAT_UNKNOWN &&
          fmt_view(DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN) == DXGI_FORMAT_UNKNOWN, "integer, depth, unknown: no");
    CHECK(fmt_view(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT) == DXGI_FORMAT_R32_FLOAT &&
          fmt_view(DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT) == DXGI_FORMAT_R11G11B10_FLOAT, "R32 float, R11G11B10");

    /* пересчёт: тот же размер — копия строк с учётом шага; сторожевые байты за строками не трогаются и не читаются */
    {
        BYTE src[3 * 8], dst[3 * 6];
        memset(src, 0xEE, sizeof src);
        for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++) src[y * 8 + x] = (BYTE)(y * 10 + x);
        memset(dst, 0xCC, sizeof dst);
        px_resample(src, 5, 3, 8, dst, 5, 3, 6, 1, 0);
        int ok = 1;
        for (int y = 0; y < 3; y++) { for (int x = 0; x < 5; x++) ok &= dst[y * 6 + x] == y * 10 + x; ok &= dst[y * 6 + 5] == 0xCC; }
        CHECK(ok, "same size: rows copied by pitch");
    }
    /* x2 ближайшей точкой: каждая точка повторена 2x2 */
    {
        UINT src[2 * 2] = {1, 2, 3, 4}, dst[4 * 4];
        px_resample((BYTE *)src, 2, 2, 8, (BYTE *)dst, 4, 4, 16, 4, 0);
        const UINT want[16] = {1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4};
        CHECK(!memcmp(dst, want, sizeof want), "nearest x2");
    }
    /* x1.5 ближайшей точкой: 2 -> 3 точки, центры 0.33, 1.0, 1.67: крайние однозначны, средняя ровно на стыке —
     * годится любая из двух соседних */
    {
        BYTE src[2] = {10, 20}, dst[3];
        px_resample(src, 2, 1, 2, dst, 3, 1, 3, 1, 0);
        CHECK(dst[0] == 10 && (dst[1] == 10 || dst[1] == 20) && dst[2] == 20, "nearest x1.5: %u %u %u", dst[0], dst[1], dst[2]);
        BYTE s4[4] = {1, 2, 3, 4}, d6[6];   /* 4 -> 6: 0 0 1 2 3 3 (центры 0.33 1.0 1.67 2.33 3.0 3.67) */
        px_resample(s4, 4, 1, 4, d6, 6, 1, 6, 1, 0);
        CHECK(d6[0] == 1 && d6[2] == 2 && d6[3] == 3 && d6[5] == 4, "nearest 4 -> 6: %u %u %u %u %u %u", d6[0], d6[1], d6[2], d6[3],
              d6[4], d6[5]);
    }
    /* плавно: ровный цвет остаётся ровным, края не выходят за картинку, середина — между соседями */
    {
        BYTE flat[4 * 4 * 4], out[6 * 6 * 4];
        memset(flat, 200, sizeof flat);
        px_resample(flat, 4, 4, 16, out, 6, 6, 24, 4, 1);
        int ok = 1;
        for (size_t i = 0; i < sizeof out; i++) ok &= out[i] == 200;
        CHECK(ok, "bilinear: flat colour stays flat");
        BYTE two[2] = {0, 255}, row[4];
        px_resample(two, 2, 1, 2, row, 4, 1, 4, 1, 1);
        CHECK(row[0] == 0 && row[3] == 255 && row[1] > 0 && row[1] < row[2] && row[2] < 255, "bilinear x2: %u %u %u %u", row[0],
              row[1], row[2], row[3]);
    }
    /* уменьшение 4 -> 2 и вырожденные размеры */
    {
        BYTE src[4] = {1, 2, 3, 4}, dst[2] = {0, 0};
        px_resample(src, 4, 1, 4, dst, 2, 1, 2, 1, 0);
        CHECK(dst[0] == 2 && dst[1] == 4, "nearest /2: %u %u", dst[0], dst[1]);
        BYTE one = 77, big[9];
        px_resample(&one, 1, 1, 1, big, 3, 3, 3, 1, 1);
        int ok = 1;
        for (int i = 0; i < 9; i++) ok &= big[i] == 77;
        CHECK(ok, "1x1 -> 3x3");
        px_resample(src, 0, 1, 4, dst, 2, 1, 2, 1, 0);   /* пустой источник: ничего не делает и не падает */
        px_resample(NULL, 4, 1, 4, dst, 2, 1, 2, 1, 0);
    }
    /* чтение только своих байт: источник в самом конце выделенной памяти, шаг строки больше ширины */
    {
        UINT sw = 7, sh = 5, pitch = 16, bpp = 4;
        size_t need = (size_t)(sh - 1) * pitch + (size_t)sw * bpp;
        BYTE *mem = (BYTE *)malloc(need), *out = (BYTE *)malloc(21 * 15 * 4);
        memset(mem, 0x5A, need);
        px_resample(mem, sw, sh, pitch, out, 21, 15, 21 * 4, bpp, 1);
        int ok = 1;
        for (size_t i = 0; i < 21 * 15 * 4; i++) ok &= out[i] == 0x5A;
        CHECK(ok, "last row shorter than the pitch");
        free(mem); free(out);
    }

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
