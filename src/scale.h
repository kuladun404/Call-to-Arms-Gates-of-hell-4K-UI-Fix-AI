/* Масштаб как дробь num/den (3/2, 2/1). Чистая арифметика без Windows — тестируется отдельно (tests/test_scale.c).
 * Правило округления одно на весь мод:
 *   логическое -> физическое: round half up   phys = floor((2*x*num + den) / (2*den))
 *   физическое -> логическое: обратное к краям  log = max x: to_phys(x) <= p      (мышь, координаты)
 *   размер экрана физ. -> лог.: floor         size_to_log(p) = floor(p*den / num)
 * Для k >= 1 выполняется to_log(to_phys(x)) == x. Прямоугольники переводятся по краям, а не по ширине,
 * поэтому соседние прямоугольники остаются стык в стык. */
#pragma once

typedef struct { int num, den; } Ratio;

#define RATIO_MAX_DEN 16

/* "2", "1.5", "1.25", "3/2"; результат сокращён, 1 <= k <= 4, den <= RATIO_MAX_DEN. 1 = ок, 0 = ошибка */
int ratio_parse(const char *s, Ratio *out);

static inline long long floor_div(long long a, long long b)
{
    long long q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static inline long long to_phys(Ratio r, long long x) { return floor_div(2 * x * r.num + r.den, 2LL * r.den); }
/* наибольший x с to_phys(x) <= p: физический пиксель p лежит в логическом пикселе x (так же, как его рисуем) */
static inline long long to_log(Ratio r, long long p) { return floor_div((2 * p + 1) * r.den - 1, 2LL * r.num); }
static inline long long size_to_log(Ratio r, long long p) { return floor_div(p * r.den, r.num); }
static inline float to_phys_f(Ratio r, float x) { return x * (float)r.num / (float)r.den; }
static inline float to_log_f(Ratio r, float p) { return p * (float)r.den / (float)r.num; }
static inline int ratio_is_one(Ratio r) { return r.num == r.den; }
static inline int ratio_eq(Ratio a, Ratio b) { return (long long)a.num * b.den == (long long)b.num * a.den; }
/* a * p / q, сокращённая */
Ratio ratio_mul(Ratio a, int p, int q);

/* края прямоугольника по отдельности: [l, r) -> [to_phys(l), to_phys(r)) */
static inline void rect_to_phys(Ratio k, long *l, long *t, long *r, long *b)
{
    *l = (long)to_phys(k, *l); *t = (long)to_phys(k, *t);
    *r = (long)to_phys(k, *r); *b = (long)to_phys(k, *b);
}

/* F03: точный масштаб для родного экрана pw x ph. Ближайший к k коэффициент g/t (g = НОД сторон, t целое), при котором
 * экран делится без остатка по обеим осям: 4K x1.5 -> 3/2, 2560x1440 x1.5 -> 160/107 (1712x963).
 * Отклонение больше 2% от k (1366x768) -> k как есть. Знаменатель может быть больше RATIO_MAX_DEN. */
Ratio ratio_fit(Ratio k, long pw, long ph);
/* физический режим -> логический, только если делится точно (иначе режим игре не показываем); 1 = да */
int mode_to_log(Ratio r, long pw, long ph, long *lw, long *lh);
