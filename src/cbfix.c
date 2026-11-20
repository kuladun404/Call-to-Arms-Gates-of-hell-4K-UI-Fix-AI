#include <stddef.h>
#include "cbfix.h"

static float fabs_(float a) { return a < 0 ? -a : a; }

static const struct { float k; int inv; } P[] = {
    {1, 0}, {1, 1}, {0.5f, 0}, {0.5f, 1}, {2, 1}, {0.25f, 0}, {0.25f, 1},
};
/* inv: величина = 1/(k*W); при k=2,inv -> 0.5/W; k=0.5,inv -> 2/W */

/* какой ширине экрана W соответствует значение v для шаблона (k, inv) */
static float est(float v, float k, int inv) { return inv ? (v != 0 ? 1.0f / (v * k) : 0) : v / k; }

int cb_fix_follow(float *f, unsigned n, unsigned idx, unsigned ref, float ref_before)
{
    if (idx >= n || ref >= n || idx == ref || ref_before == 0 || f[ref] == 0 || f[ref] == ref_before) return 0;
    f[idx] *= ref_before / f[ref];   /* ref — обратная высота: 1/1080 -> 1/2160 значит высота выросла вдвое */
    return 1;
}

int cb_hbao_radius_at(const float *f, unsigned n, unsigned i)
{
    if (i + 2 >= n) return 0;
    float r2 = f[i + 1], prod = f[i + 2] * r2 + 1;
    return f[i] > 0 && r2 > 0 && prod < 1e-3f && prod > -1e-3f;
}

/* 20.11 (кусок мира мигает при смене FOV в прямом управлении): то же устройство «x > 0, y > 0, z = −1/y» дают и
 * [7] 1/высота, [8] UVToViewA.x, [9] UVToViewA.y, когда A.x·A.y = −1 (вертикальный угол ≈ 41°; в прямом управлении игра
 * проходит его при начале и конце бега: 0,95 -> 1,08). Первое место было [7] — 1/высота удваивалась вместо радиуса.
 * Мест два — не гадать */
int cb_hbao_radius_index(const float *f, unsigned n)
{
    int found = -1;
    for (unsigned i = 0; i + 2 < n; i++)
        if (cb_hbao_radius_at(f, n, i)) {
            if (found >= 0) return -1;
            found = (int)i;
        }
    return found;
}

/* пара f[i], f[i+1]: 1 — экранная, переписана; exact_only — без допуска ±1 пиксель (только точное значение экрана) */
static int fix_pair(float *f, unsigned i, long lw, long lh, long pw, long ph, int exact_only)
{
        for (unsigned k = 0; k < sizeof P / sizeof P[0]; k++) {
            float kk = P[k].k;
            int inv = P[k].inv;
            float a = est(f[i], kk, inv), b = est(f[i + 1], kk, inv);
            /* точное совпадение — точные физические; иначе допуск: игра считает размеры через свои картинки половинного /
             * четвертного размера (535 вместо 535.5, 1/1070 вместо 1/1071) — ±1 пиксель такой картинки */
            float ex = 1e-4f;
            int exact = fabs_(a - (float)lw) <= (float)lw * ex && fabs_(b - (float)lh) <= (float)lh * ex;
            float tol = (kk < 1 ? 1.01f / kk : 1.01f);
            if (!exact && (exact_only || !(fabs_(a - (float)lw) <= tol && fabs_(b - (float)lh) <= tol))) continue;
            if (exact) {
                float A = (float)pw * kk, B = (float)ph * kk;
                if (inv) { A = 1.0f / A; B = 1.0f / B; }
                f[i] = A;
                f[i + 1] = B;
            } else {   /* пропорционально: сохраняем поправку игры на ±1 пиксель */
                float sx = (float)pw / (float)lw, sy = (float)ph / (float)lh;
                if (inv) { f[i] /= sx; f[i + 1] /= sy; } else { f[i] *= sx; f[i + 1] *= sy; }
            }
            return 1;
        }
    return 0;
}

int cb_fix_floats(float *f, unsigned n, long lw, long lh, long pw, long ph)
{
    if (!lw || !lh || !pw || !ph || (lw == pw && lh == ph)) return 0;
    int fixed = 0;
    for (unsigned i = 0; i + 1 < n; i++)
        if (fix_pair(f, i, lw, lh, pw, ph, 0)) { fixed++; i++; }
    return fixed;
}

int cb_fix_at_x(float *f, unsigned n, const unsigned *at, const unsigned char *exact, int nat, long lw, long lh, long pw,
                long ph)
{
    if (!lw || !lh || !pw || !ph || (lw == pw && lh == ph)) return 0;
    int fixed = 0;
    for (int j = 0; j < nat; j++)
        if (at[j] + 1 < n) fixed += fix_pair(f, at[j], lw, lh, pw, ph, exact ? exact[j] : 0);
    return fixed;
}

int cb_fix_at(float *f, unsigned n, const unsigned *at, int nat, long lw, long lh, long pw, long ph)
{
    return cb_fix_at_x(f, n, at, NULL, nat, lw, lh, pw, ph);
}
