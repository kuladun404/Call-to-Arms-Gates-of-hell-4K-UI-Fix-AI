/* F01/F07: дробный масштаб — разбор, округление, стыковка прямоугольников. Последняя строка PASS/FAIL. */
#include <stdio.h>
#include "scale.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void parse_ok(const char *s, int n, int d)
{
    Ratio r = {0, 0};
    int ok = ratio_parse(s, &r);
    CHECK(ok && r.num == n && r.den == d, "parse(\"%s\") = %d %d/%d, want %d/%d", s, ok, r.num, r.den, n, d);
}

static void parse_bad(const char *s)
{
    Ratio r;
    CHECK(!ratio_parse(s, &r), "parse(\"%s\") should fail", s);
}

int main(void)
{
    parse_ok("2", 2, 1);
    parse_ok("1.5", 3, 2);
    parse_ok("1,5", 3, 2);
    parse_ok(" 1.50 ", 3, 2);
    parse_ok("3/2", 3, 2);
    parse_ok("6/4", 3, 2);
    parse_ok("1.25", 5, 4);
    parse_ok("1.75", 7, 4);
    parse_ok("4/3", 4, 3);
    parse_ok("1", 1, 1);
    parse_ok("4", 4, 1);
    parse_bad(""); parse_bad("abc"); parse_bad("0.5"); parse_bad("5"); parse_bad("1.333"); /* 1333/1000: den > 16 */
    parse_bad("3/0"); parse_bad("1.5x"); parse_bad("."); parse_bad("2/"); parse_bad("-1.5");

    Ratio k15 = {3, 2}, k2 = {2, 1};
    /* экраны */
    CHECK(to_phys(k15, 2560) == 3840 && to_phys(k15, 1440) == 2160, "2560x1440*1.5");
    CHECK(to_phys(k2, 1920) == 3840 && to_phys(k2, 1080) == 2160, "1920x1080*2");
    CHECK(to_phys(k15, 1280) == 1920 && to_phys(k15, 720) == 1080, "1280x720*1.5");
    CHECK(size_to_log(k15, 3840) == 2560 && size_to_log(k15, 2560) == 1706 && size_to_log(k15, 1440) == 960,
          "screen size phys->log 1.5");
    CHECK(to_log(k15, 0) == 0 && to_log(k15, 1) == 0 && to_log(k15, 2) == 1 && to_log(k15, 3) == 2,
          "phys pixels 0..3 -> %lld %lld %lld %lld", to_log(k15, 0), to_log(k15, 1), to_log(k15, 2), to_log(k15, 3));
    /* мышь согласована с рисованием: физический пиксель p попадает в логический x, чей прямоугольник [phys(x), phys(x+1)) его содержит */
    for (unsigned i = 0; i < 2; i++) {
        Ratio k = i ? k15 : (Ratio){7, 4};
        for (long long p = -20; p < 4000; p++) {
            long long x = to_log(k, p);
            if (!(to_phys(k, x) <= p && p < to_phys(k, x + 1))) { CHECK(0, "k=%d/%d p=%lld -> x=%lld", k.num, k.den, p, x); break; }
        }
    }
    CHECK(to_log(k2, 3839) == 1919 && to_log(k15, 3839) == 2559, "last pixel maps inside the screen");
    /* round half up */
    CHECK(to_phys(k15, 1) == 2 && to_phys(k15, 3) == 5 && to_phys(k15, -1) == -1, "half up: 1->%lld 3->%lld -1->%lld",
          to_phys(k15, 1), to_phys(k15, 3), to_phys(k15, -1));

    /* to_log(to_phys(x)) == x для всех поддерживаемых k */
    Ratio ks[] = {{1, 1}, {5, 4}, {4, 3}, {3, 2}, {7, 4}, {2, 1}, {5, 2}, {3, 1}, {4, 1}};
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++)
        for (long long x = -50; x <= 8192; x++)
            if (to_log(ks[i], to_phys(ks[i], x)) != x) {
                CHECK(0, "roundtrip k=%d/%d x=%lld", ks[i].num, ks[i].den, x);
                break;
            }

    /* соседние прямоугольники после масштаба стык в стык, без щелей и наложений */
    for (long x = 0; x < 2000; x += 7) {
        long l1 = 0, t1 = 0, r1 = x, b1 = 10, l2 = x, t2 = 0, r2 = x + 13, b2 = 10;
        rect_to_phys(k15, &l1, &t1, &r1, &b1);
        rect_to_phys(k15, &l2, &t2, &r2, &b2);
        if (r1 != l2) { CHECK(0, "gap at x=%ld: %ld vs %ld", x, r1, l2); break; }
    }
    /* весь экран 2560x1440 -> ровно 3840x2160 */
    long l = 0, t = 0, r = 2560, b = 1440;
    rect_to_phys(k15, &l, &t, &r, &b);
    CHECK(l == 0 && t == 0 && r == 3840 && b == 2160, "full screen rect");
    CHECK(to_phys_f(k15, 2560.f) == 3840.f && to_log_f(k15, 3840.f) == 2560.f, "float");

    /* F03: точный масштаб под родной экран */
    struct { int pw, ph; Ratio k; int num, den, lw, lh; } fit[] = {
        {3840, 2160, {3, 2}, 3, 2, 2560, 1440},   {3840, 2160, {2, 1}, 2, 1, 1920, 1080},
        {2560, 1440, {3, 2}, 160, 107, 1712, 963}, {2560, 1440, {2, 1}, 2, 1, 1280, 720},
        {2560, 1440, {5, 4}, 5, 4, 2048, 1152},   {2560, 1440, {4, 3}, 4, 3, 1920, 1080},
        {1920, 1080, {3, 2}, 3, 2, 1280, 720},    {1920, 1080, {5, 4}, 5, 4, 1536, 864},
        {3440, 1440, {3, 2}, 80, 53, 2279, 954},  {1920, 1200, {3, 2}, 3, 2, 1280, 800},
    };
    for (size_t i = 0; i < sizeof fit / sizeof fit[0]; i++) {
        Ratio f = ratio_fit(fit[i].k, fit[i].pw, fit[i].ph);
        long lw = 0, lh = 0;
        int ok = mode_to_log(f, fit[i].pw, fit[i].ph, &lw, &lh);
        CHECK(f.num == fit[i].num && f.den == fit[i].den && ok && lw == fit[i].lw && lh == fit[i].lh &&
                  to_phys(f, lw) == fit[i].pw && to_phys(f, lh) == fit[i].ph,
              "fit %dx%d k=%d/%d -> %d/%d %ldx%ld", fit[i].pw, fit[i].ph, fit[i].k.num, fit[i].k.den, f.num, f.den, lw, lh);
    }
    Ratio odd = ratio_fit(k15, 1366, 768);   /* НОД 2: точного близкого нет -> k */
    CHECK(odd.num == 3 && odd.den == 2, "1366x768 keeps k: %d/%d", odd.num, odd.den);
    long a, c;
    CHECK(!mode_to_log(k15, 2560, 1440, &a, &c) && mode_to_log(k15, 1920, 1080, &a, &c) && a == 1280 && c == 720,
          "mode_to_log exact only");

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
