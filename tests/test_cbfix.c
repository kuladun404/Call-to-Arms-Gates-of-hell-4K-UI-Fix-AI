/* F08: замена экранных пар в CB (чистая функция). Последняя строка PASS/FAIL. */
#include <string.h>
#include <stdio.h>
#include "cbfix.h"

static int near_(float a, float b, float tol) { float d = a - b; return (d < 0 ? -d : d) <= tol; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* 4K при ×1.5: 2560x1440 -> 3840x2160 */
    float f[20] = {2560, 1440, 1.0f / 2560, 1.0f / 1440, 1280, 720, 2.0f / 2560, 2.0f / 1440, 0.5f / 2560, 0.5f / 1440,
                   640, 360, 4.0f / 2560, 4.0f / 1440, 2560, 7, 1, 1, 1440, 2560};
    int n = cb_fix_floats(f, 20, 2560, 1440, 3840, 2160);
    CHECK(n == 7, "fixed pairs %d, want 7", n);
    CHECK(f[0] == 3840 && f[1] == 2160, "W,H (%g %g)", f[0], f[1]);
    CHECK(f[2] == 1.0f / 3840 && f[3] == 1.0f / 2160, "1/W");
    CHECK(f[4] == 1920 && f[5] == 1080, "W/2 (%g %g)", f[4], f[5]);
    CHECK(f[6] == 2.0f / 3840 && f[7] == 2.0f / 2160, "2/W");
    CHECK(f[8] == 0.5f / 3840 && f[9] == 0.5f / 2160, "0.5/W");
    CHECK(f[10] == 960 && f[11] == 540, "W/4");
    CHECK(f[12] == 4.0f / 3840 && f[13] == 4.0f / 2160, "4/W");
    CHECK(f[14] == 2560 && f[15] == 7, "W without H stays");
    CHECK(f[18] == 1440 && f[19] == 2560, "swapped pair (H,W) stays");

    /* нечётный логический экран: точные физические, не умножение */
    float g[2] = {1281, 721};
    cb_fix_floats(g, 2, 1281, 721, 1922, 1082);
    CHECK(g[0] == 1922 && g[1] == 1082, "odd (%g %g)", g[0], g[1]);

    /* L8: нечётный экран 1904x1071 -> мир 2560x1440: игра даёт размеры через половинные картинки (535, 1/1070) */
    float q[8] = {1.0f / 1904, 1.0f / 1070, 952, 535, 1904, 1071, 1.0f / 1904, 1.0f / 1080};
    int nq = cb_fix_floats(q, 8, 1904, 1071, 2560, 1440);
    CHECK(nq == 3, "near pairs fixed %d, want 3", nq);
    CHECK(q[0] > 1.0f / 2561 && q[0] < 1.0f / 2559 && q[1] > 1.0f / 1440 && q[1] < 1.0f / 1437, "1/W,1/(H-1) (%g %g)", 1 / q[0], 1 / q[1]);
    CHECK(q[2] > 1279 && q[2] < 1281 && q[3] > 718.5f && q[3] < 720.5f, "half ints (%g %g)", q[2], q[3]);
    CHECK(q[4] == 2560 && q[5] == 1440, "exact W,H (%g %g)", q[4], q[5]);
    CHECK(q[6] == 1.0f / 1904 && q[7] == 1.0f / 1080, "far value stays (%g %g)", 1 / q[6], 1 / q[7]);

    /* экран неизвестен или масштаб 1 — ничего */
    float h[2] = {1920, 1080};
    CHECK(cb_fix_floats(h, 2, 0, 0, 0, 0) == 0 && h[0] == 1920, "unknown screen");
    CHECK(cb_fix_floats(h, 2, 1920, 1080, 1920, 1080) == 0, "scale 1");
    /* последнее одиночное значение не читается за границей */
    float one[1] = {1920};
    CHECK(cb_fix_floats(one, 1, 1920, 1080, 3840, 2160) == 0, "n=1");

    /* Г2: HBAO+ (84 float). Числа из снимка кадра игры на строке «3840x2160 x2»: игра считает кадр 1920x1080.
     * [4,5] 1/четверть, [6,7] 1/полный размер — пары; [12] RadiusToScreen — одиночное число, пропорциональное
     * высоте кадра (на «x1» игра даёт 62576.4). Его надо умножить на то же, на что изменилась высота. */
    float hb[84] = {0};
    hb[4] = 0.00208333f; hb[5] = 0.0037037f; hb[6] = 0.000520833f; hb[7] = 0.000925926f;
    hb[12] = 31288.2f; hb[13] = 324; hb[14] = -0.00308642f;
    float ref = hb[7];
    CHECK(cb_fix_floats(hb, 84, 1920, 1080, 3840, 2160) == 2, "HBAO+: two pairs fixed");
    CHECK(cb_fix_follow(hb, 84, 12, 7, ref) == 1, "HBAO+: radius follows the frame height");
    CHECK(near_(hb[12], 62576.4f, 0.5f), "HBAO+: RadiusToScreen %g, want 62576.4 (as the game gives at 3840x2160)", hb[12]);
    CHECK(hb[13] == 324 && near_(hb[14], -0.00308642f, 1e-7f), "HBAO+: R2 and NegInvR2 untouched");
    /* пара не менялась (масштаб 1 или чужой буфер) — радиус не трогаем */
    float hc[84] = {0};
    hc[6] = 0.000260417f; hc[7] = 0.000462963f; hc[12] = 62576.4f;
    ref = hc[7];
    cb_fix_floats(hc, 84, 3840, 2160, 3840, 2160);   /* строка «x1»: мод ничего не увеличивает */
    CHECK(cb_fix_follow(hc, 84, 12, 7, ref) == 0 && hc[12] == 62576.4f, "HBAO+: no pair change - radius untouched");
    CHECK(cb_fix_follow(hb, 84, 200, 7, ref) == 0 && cb_fix_follow(hb, 84, 12, 7, 0) == 0, "bad index / empty reference");
    /* мир меньше экрана: кадр 1856x1044 -> 2560x1440 */
    float hd[84] = {0};
    hd[6] = 1.0f / 1856; hd[7] = 1.0f / 1044; hd[12] = 30000;
    ref = hd[7];
    cb_fix_floats(hd, 84, 1856, 1044, 2560, 1440);
    cb_fix_follow(hd, 84, 12, 7, ref);
    CHECK(near_(hd[12], 30000.0f * 1440 / 1044, 1.0f), "HBAO+ at world size: %g, want %g", hd[12], 30000.0 * 1440 / 1044);

    /* устойчивость к обновлениям: номер радиуса HBAO+ — по устройству буфера (за ним R² и −1/R²) */
    float he[84] = {0};
    he[6] = 1.0f / 1920; he[7] = 1.0f / 1080; he[12] = 31288.2f; he[13] = 324; he[14] = -0.00308642f;   /* как в игре 16.11 */
    CHECK(cb_hbao_radius_index(he, 84) == 12, "radius at [12] in the buffer of 16.11: %d", cb_hbao_radius_index(he, 84));
    float hf[100] = {0};   /* новая версия библиотеки: буфер больше, радиус сдвинут */
    hf[30] = 1.0f / 1920; hf[31] = 1.0f / 1080; hf[44] = 25000; hf[45] = 400; hf[46] = -1.0f / 400;
    CHECK(cb_hbao_radius_index(hf, 100) == 44, "shifted radius: %d, want 44", cb_hbao_radius_index(hf, 100));
    float hg[84] = {0};
    hg[12] = 31288.2f; hg[13] = 324; hg[14] = 0.5f;   /* −1/R² нет — не тот буфер */
    CHECK(cb_hbao_radius_index(hg, 84) == -1, "no R2 / -1/R2 pair - must not guess: %d", cb_hbao_radius_index(hg, 84));
    CHECK(cb_hbao_radius_index(he, 14) == -1, "pair beyond the end of the buffer must not be read");
    /* 20.11: буфер игры в прямом управлении (flash_ui4k.log), угол обзора на переходе бега: UVToViewA.x·A.y = −1 —
     * [7] 1/высота, [8] A.x, [9] A.y устроены как «радиус, R², −1/R²». Раньше находилось [7] */
    float hx[84] = {0};
    hx[4] = 1.0f / 480; hx[5] = 1.0f / 270; hx[6] = 1.0f / 1920; hx[7] = 1.0f / 1080;
    hx[8] = 1.33333f; hx[9] = -0.75f; hx[10] = -0.666665f; hx[11] = 0.375f;
    hx[12] = 25500; hx[13] = 324; hx[14] = -0.00308642f; hx[15] = 0.3f;
    CHECK(cb_hbao_radius_at(hx, 84, 7) && cb_hbao_radius_at(hx, 84, 12), "both places look like the radius at this angle");
    CHECK(cb_hbao_radius_index(hx, 84) == -1, "two candidates - must not guess: %d", cb_hbao_radius_index(hx, 84));
    hx[8] = 1.38818f; hx[9] = -0.780851f;   /* обычный кадр прямого управления */
    CHECK(cb_hbao_radius_index(hx, 84) == 12, "ordinary direct-control frame: %d", cb_hbao_radius_index(hx, 84));
    CHECK(!cb_hbao_radius_at(hx, 84, 82) && !cb_hbao_radius_at(hx, 84, 7), "end of buffer / not a radius");

    /* Э3: только заданные места; та же пара в другом месте не трогается */
    {
        float f[16] = {0};
        f[4] = 1920; f[5] = 1080;        /* место 4 — в списке */
        f[10] = 1920; f[11] = 1080;      /* место 10 — нет */
        f[12] = 0.5f; f[13] = 4316.5f;   /* место 12 — в списке, но не экранная пара */
        const unsigned at[] = {4, 12, 15};   /* 15 — пара вылезла бы за конец */
        int k = cb_fix_at(f, 16, at, 3, 1920, 1080, 3840, 2160);
        CHECK(k == 1, "one pair fixed at named places: %d", k);
        CHECK(f[4] == 3840 && f[5] == 2160, "named place fixed: %g %g", f[4], f[5]);
        CHECK(f[10] == 1920 && f[11] == 1080, "same pair elsewhere left alone: %g %g", f[10], f[11]);
        CHECK(f[12] == 0.5f && f[13] == 4316.5f, "named place with other numbers left alone");
        float g[16] = {0};
        g[0] = 1.0f / 1920; g[1] = 1.0f / 1080;
        const unsigned a0[] = {0};
        CHECK(cb_fix_at(g, 16, a0, 1, 1920, 1080, 3840, 2160) == 1 && g[0] == 1.0f / 3840, "inverse pair at a named place");
        CHECK(cb_fix_at(g, 16, a0, 1, 1920, 1080, 1920, 1080) == 0, "scale 1 - nothing");
    }
    /* 20.11: место центра экрана (c3) — в мировых проходах позиция камеры. С допуском камера в (961, 541) принималась
     * за центр экрана 960x540 и удваивалась; с exact — только точное значение */
    {
        const unsigned at[] = {12};
        const unsigned char ex[] = {1};
        float c[16] = {0};
        c[12] = 961; c[13] = 541; c[14] = 300;
        float d[16];
        memcpy(d, c, sizeof d);
        CHECK(cb_fix_at(d, 16, at, 1, 1920, 1080, 3840, 2160) == 1, "with tolerance the camera near (960, 540) is taken for the screen (old way)");
        CHECK(cb_fix_at_x(c, 16, at, ex, 1, 1920, 1080, 3840, 2160) == 0 && c[12] == 961 && c[13] == 541,
              "exact place: camera at (961, 541) left alone: %g %g", c[12], c[13]);
        c[12] = 960; c[13] = 540;
        CHECK(cb_fix_at_x(c, 16, at, ex, 1, 1920, 1080, 3840, 2160) == 1 && c[12] == 1920 && c[13] == 1080,
              "exact place: screen centre (960, 540) still fixed: %g %g", c[12], c[13]);
        c[12] = 1920.4f; c[13] = 1080.4f;
        CHECK(cb_fix_at_x(c, 16, at, ex, 1, 1920, 1080, 3840, 2160) == 0, "exact place: (1920.4, 1080.4) left alone");
    }
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
