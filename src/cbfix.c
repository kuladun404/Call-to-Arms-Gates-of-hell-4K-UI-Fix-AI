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

int cb_hbao_radius_index(const float *f, unsigned n)
{
    for (unsigned i = 0; i + 2 < n; i++) {
        float r2 = f[i + 1], prod = f[i + 2] * r2 + 1;
        if (f[i] > 0 && r2 > 0 && prod < 1e-3f && prod > -1e-3f) return (int)i;
    }
    return -1;
}

int cb_fix_floats(float *f, unsigned n, long lw, long lh, long pw, long ph)
{
    if (!lw || !lh || !pw || !ph || (lw == pw && lh == ph)) return 0;
    int fixed = 0;
    for (unsigned i = 0; i + 1 < n; i++)
        for (unsigned k = 0; k < sizeof P / sizeof P[0]; k++) {
            float kk = P[k].k;
            int inv = P[k].inv;
            float a = est(f[i], kk, inv), b = est(f[i + 1], kk, inv);
            /* точное совпадение — точные физические; иначе допуск: игра считает размеры через свои картинки половинного /
             * четвертного размера (535 вместо 535.5, 1/1070 вместо 1/1071) — ±1 пиксель такой картинки */
            float ex = 1e-4f;
            int exact = fabs_(a - (float)lw) <= (float)lw * ex && fabs_(b - (float)lh) <= (float)lh * ex;
            float tol = (kk < 1 ? 1.01f / kk : 1.01f);
            if (!exact && !(fabs_(a - (float)lw) <= tol && fabs_(b - (float)lh) <= tol)) continue;
            if (exact) {
                float A = (float)pw * kk, B = (float)ph * kk;
                if (inv) { A = 1.0f / A; B = 1.0f / B; }
                f[i] = A;
                f[i + 1] = B;
            } else {   /* пропорционально: сохраняем поправку игры на ±1 пиксель */
                float sx = (float)pw / (float)lw, sy = (float)ph / (float)lh;
                if (inv) { f[i] /= sx; f[i + 1] /= sy; } else { f[i] *= sx; f[i + 1] *= sy; }
            }
            fixed++;
            i++;
            break;
        }
    return fixed;
}
