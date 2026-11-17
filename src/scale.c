#include "scale.h"

static int gcd(int a, int b)
{
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

int ratio_parse(const char *s, Ratio *out)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    long long num = 0, den = 1;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        num = num * 10 + (*s - '0');
        if (num > 1000000) return 0;
    }
    if (!digits) return 0;
    if (*s == '.' || *s == ',') {
        s++;
        int fd = 0;
        for (; *s >= '0' && *s <= '9'; s++, fd++) {
            if (fd >= 4) return 0;
            num = num * 10 + (*s - '0');
            den *= 10;
        }
        if (!fd) return 0;
    } else if (*s == '/') {
        s++;
        long long d = 0;
        int dd = 0;
        for (; *s >= '0' && *s <= '9'; s++, dd++) {
            d = d * 10 + (*s - '0');
            if (d > 1000000) return 0;
        }
        if (!dd || !d) return 0;
        den = d;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s) return 0;
    int g = gcd((int)num, (int)den);
    if (g <= 0) return 0;
    num /= g; den /= g;
    if (den > RATIO_MAX_DEN || num < den || num > 4 * den) return 0;
    out->num = (int)num;
    out->den = (int)den;
    return 1;
}

Ratio ratio_fit(Ratio k, long pw, long ph)
{
    if (pw <= 0 || ph <= 0 || ratio_is_one(k)) return k;
    long long g = gcd((int)pw, (int)ph);
    long long t = floor_div(2 * g * k.den + k.num, 2LL * k.num);   /* round(g / k) */
    if (t < 1) return k;
    long long diff = g * k.den - t * k.num;                          /* |g/t - k| <= 2% k */
    if (diff < 0) diff = -diff;
    if (diff * 50 > t * k.num) return k;
    int q = gcd((int)g, (int)t);
    Ratio r = {(int)(g / q), (int)(t / q)};
    return r.num < r.den ? k : r;
}

int mode_to_log(Ratio r, long pw, long ph, long *lw, long *lh)
{
    long long w = (long long)pw * r.den, h = (long long)ph * r.den;
    if (pw <= 0 || ph <= 0 || w % r.num || h % r.num) return 0;
    *lw = (long)(w / r.num);
    *lh = (long)(h / r.num);
    return 1;
}

Ratio ratio_mul(Ratio a, int p, int q)
{
    long long n = (long long)a.num * p, d = (long long)a.den * q;
    while (n > 0x3fffffff || d > 0x3fffffff) { n >>= 1; d >>= 1; }
    int g = gcd((int)n, (int)d);
    Ratio r = {(int)(n / g), (int)(d / g)};
    return r;
}
