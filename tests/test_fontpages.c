/* Э6: чистая часть чётких букв (fontpages_core.c): файл образцов, атлас, свои ячейки букв. Последняя строка PASS/FAIL.
 * Аргумент (если есть): настоящий out/fontpages/ui4k_fonts2.dat (из архива выпуска) — разбор и атласы всех страниц. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fontpages.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* буква образца: ячейка на странице, канал, картинка ×K (ox, oy от угла ячейки, iw x ih), яркость val(i, x, y) */
typedef struct { uint16_t x, y, w, h; uint8_t chan; int16_t ox, oy; uint16_t iw, ih; } G;
static uint8_t v_full(int i, int x, int y) { (void)i; (void)x; (void)y; return 255; }
static uint8_t v_ramp(int i, int x, int y) { return (uint8_t)(1 + i * 40 + x * 3 + y); }
static uint8_t v_zero(int i, int x, int y) { (void)i; (void)x; (void)y; return 0; }

static size_t make_file(uint8_t *f, uint32_t ver, uint32_t K, uint32_t W, uint32_t H, uint64_t hash, const G *g, int n,
                        uint8_t (*val)(int, int, int))
{
    memcpy(f, "U4KF", 4); put32(f + 4, ver); put32(f + 8, K); put32(f + 12, 1);
    size_t p = 16 + (ver == 3 ? 32 : 24);   /* версия 3: запись страницы + CRC32 и запасное поле */
    put32(f + 16, W); put32(f + 20, H); put32(f + 24, (uint32_t)hash); put32(f + 28, (uint32_t)(hash >> 32));
    put32(f + 32, (uint32_t)n); put32(f + 36, (uint32_t)p);
    if (ver == 3) { put32(f + 40, 0xC0FFEE11u); put32(f + 44, 0); }
    for (int i = 0; i < n; i++) {
        put16(f + p, g[i].x); put16(f + p + 2, g[i].y); put16(f + p + 4, g[i].w); put16(f + p + 6, g[i].h);
        f[p + 8] = g[i].chan; f[p + 9] = 0;
        put16(f + p + 10, (uint16_t)g[i].ox); put16(f + p + 12, (uint16_t)g[i].oy);
        put16(f + p + 14, g[i].iw); put16(f + p + 16, g[i].ih);
        p += 18;
        uint8_t (*v)(int, int, int) = i == 2 ? v_zero : val;   /* буква 2 — пустая (пробел) */
        for (int y = 0; y < g[i].ih; y++)
            for (int x = 0; x < g[i].iw; x++) f[p++] = v(i, x, y);
        while (p % 4) f[p++] = 0;
    }
    return p;
}

/* страница 256x128: буква 0 и 1 — канал 0 и 2 в одной ячейке (как BMFont по каналам); 2 — пустая; 3 — шире ячейки
 * (краткая выше на 3 px ×2, справа на 1 px); 4 — картинка далеко от ячейки (порча, не своя); 5 и 6 — каналы 0 и 2,
 * ячейки разные (по ним узнаётся, какой байт маски какой канал) */
static const G GS[] = {
    {10, 20, 6, 9, 0, -1, 0, 13, 18},
    {10, 20, 6, 9, 2, 0, -2, 12, 21},
    {40, 20, 3, 9, 0, 0, 0, 1, 1},
    {60, 20, 5, 9, 1, -2, -3, 14, 21},
    {80, 20, 4, 4, 3, 100, 0, 4, 4},
    {100, 20, 5, 9, 0, 0, 0, 10, 18},
    {120, 20, 5, 9, 2, 0, 0, 10, 18},
};
#define NG 7

/* ---- вершины как у движка (eFontBMF слот 12): углы клетки, UV клетки, маска канала ---- */
static void vtx(uint8_t *v, float x, float y, float u, float w, const uint8_t mask[4])
{
    float f[6] = {x, y, 0.5f, 0, 0, 1};
    memcpy(v, f, 24);
    memcpy(v + FP_VCOLOR, mask, 4);
    memcpy(v + FP_VUV, &u, 4);
    memcpy(v + FP_VUV + 4, &w, 4);
}
static float fget(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }

/* полоса: буквы (x, y — угол на экране, логич.; ячейка gi страницы W x H; clipL — сколько px срезано слева) */
typedef struct { float x, y; int gi; int maskbyte; int clipL; } L;
static uint32_t make_strip(uint8_t *out, const L *ls, int n, uint32_t W, uint32_t H, int order)
{
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        const G *g = &GS[ls[i].gi];
        uint8_t m[4] = {0, 0, 0, 0};
        if (ls[i].maskbyte >= 0) m[ls[i].maskbyte] = 255;
        float x0 = ls[i].x + ls[i].clipL, x1 = ls[i].x + g->w, y0 = ls[i].y, y1 = ls[i].y + g->h;
        float u0 = (float)(g->x + ls[i].clipL) / W, u1 = (float)(g->x + g->w) / W, v0 = (float)g->y / H, v1 = (float)(g->y + g->h) / H;
        uint8_t q[4][FP_VSTRIDE];
        if (order == 0) {   /* TL TR BL BR */
            vtx(q[0], x0, y0, u0, v0, m); vtx(q[1], x1, y0, u1, v0, m); vtx(q[2], x0, y1, u0, v1, m); vtx(q[3], x1, y1, u1, v1, m);
        } else {            /* TL BL TR BR */
            vtx(q[0], x0, y0, u0, v0, m); vtx(q[1], x0, y1, u0, v1, m); vtx(q[2], x1, y0, u1, v0, m); vtx(q[3], x1, y1, u1, v1, m);
        }
        if (i) {
            memcpy(out + (size_t)k * FP_VSTRIDE, out + (size_t)(k - 1) * FP_VSTRIDE, FP_VSTRIDE);
            memcpy(out + (size_t)(k + 1) * FP_VSTRIDE, q[0], FP_VSTRIDE);
            k += 2;
        }
        memcpy(out + (size_t)k * FP_VSTRIDE, q, sizeof q);
        k += 4;
    }
    return k;
}

/* прямоугольник квадрата j полосы (min/max x, y; u, v) */
static void rect_of(const uint8_t *s, int j, double r[8])
{
    r[0] = r[1] = 1e9; r[2] = r[3] = -1e9; r[4] = r[5] = 1e9; r[6] = r[7] = -1e9;
    for (int k = 0; k < 4; k++) {
        const uint8_t *v = s + (size_t)(6 * j + k) * FP_VSTRIDE;
        double x = fget(v), y = fget(v + 4), u = fget(v + FP_VUV), w = fget(v + FP_VUV + 4);
        if (x < r[0]) r[0] = x;
        if (y < r[1]) r[1] = y;
        if (x > r[2]) r[2] = x;
        if (y > r[3]) r[3] = y;
        if (u < r[4]) r[4] = u;
        if (w < r[5]) r[5] = w;
        if (u > r[6]) r[6] = u;
        if (w > r[7]) r[7] = w;
    }
}

static int overlap_free(const FpAtlas *at)
{
    for (uint32_t i = 0; i < at->n; i++) {
        const FpPlace *a = &at->pl[i];
        if (a->kind != FP_OWN) continue;
        uint32_t aw = (uint32_t)(a->X1 - a->X0), ah = (uint32_t)(a->Y1 - a->Y0);
        if (a->ax < FP_GAP || a->ay < FP_GAP || a->ax + aw + FP_GAP > at->AW || a->ay + ah + FP_GAP > at->AH) return 0;
        for (uint32_t j = i + 1; j < at->n; j++) {
            const FpPlace *b = &at->pl[j];
            if (b->kind != FP_OWN || at->pg->g[i].chan != at->pg->g[j].chan) continue;
            uint32_t bw = (uint32_t)(b->X1 - b->X0), bh = (uint32_t)(b->Y1 - b->Y0);
            if (a->ax < b->ax + bw + FP_GAP && b->ax < a->ax + aw + FP_GAP && a->ay < b->ay + bh + FP_GAP && b->ay < a->ay + ah + FP_GAP)
                return 0;
        }
    }
    return 1;
}

static void test_parse(void)
{
    static uint8_t f[1 << 16];
    size_t n = make_file(f, 2, 2, 256, 128, 0x1122334455667788ull, GS, NG, v_ramp);
    FpData d;
    CHECK(fp_parse(f, n, &d), "parse v2");
    CHECK(d.K == 2 && d.npages == 1 && d.pages[0].n == NG && d.pages[0].hash == 0x1122334455667788ull, "header");
    const FpGlyph *g = &d.pages[0].g[1];
    CHECK(g->x == 10 && g->chan == 2 && g->ox == 0 && g->oy == -2 && g->iw == 12 && g->ih == 21 && g->px[0] == 41, "glyph 1");
    CHECK(d.pages[0].g[3].ox == -2 && d.pages[0].g[3].oy == -3, "negative offsets");
    CHECK(fp_find(&d, 256, 128, 0x1122334455667788ull) == &d.pages[0] && !fp_find(&d, 256, 64, 0x1122334455667788ull), "find");
    fp_free(&d);
    size_t m = make_file(f, 1, 2, 256, 128, 1, GS, NG, v_ramp);
    CHECK(!fp_parse(f, m, &d), "old version 1 refused");
    /* ревизия v3, п. 5: версия 3 — с CRC32 всей страницы; версия 2 читается без неё */
    m = make_file(f, 3, 2, 256, 128, 0x1122334455667788ull, GS, NG, v_ramp);
    CHECK(fp_parse(f, m, &d) && d.npages == 1 && d.pages[0].has_crc && d.pages[0].crc == 0xC0FFEE11u && d.pages[0].n == NG &&
          d.pages[0].g[3].iw == 14, "version 3 parsed, CRC32 read");
    fp_free(&d);
    m = make_file(f, 2, 2, 256, 128, 1, GS, NG, v_ramp);
    CHECK(fp_parse(f, m, &d) && !d.pages[0].has_crc, "version 2: no CRC32, no check");
    fp_free(&d);
    m = make_file(f, 4, 2, 256, 128, 1, GS, NG, v_ramp);
    CHECK(!fp_parse(f, m, &d), "unknown version 4 refused");
    {
        uint8_t px[8] = {'1', '2', '3', '4', 'x', 'x', 'x', 'x'};   /* CRC32("1234") = 9BE3E0A3, шаг строки не считается */
        CHECK(fp_crc32(px, 1, 1, 8) == 0x9BE3E0A3u, "CRC32 of a 1x1 page: %08x", fp_crc32(px, 1, 1, 8));
        uint8_t two[16] = {'1', '2', '3', '4', 0, 0, 0, 0, '5', '6', '7', '8', 0, 0, 0, 0}, dense[8] = {'1', '2', '3', '4', '5', '6', '7', '8'};
        CHECK(fp_crc32(two, 1, 2, 8) == fp_crc32(dense, 1, 2, 4), "CRC32 ignores row pitch");
    }
    m = make_file(f, 2, 2, 256, 128, 1, GS, NG, v_ramp);
    CHECK(!fp_parse(f, m - 9, &d), "truncated refused");
    G bad[1] = {{250, 0, 10, 4, 0, 0, 0, 4, 4}};
    m = make_file(f, 2, 2, 256, 128, 1, bad, 1, v_ramp);
    CHECK(!fp_parse(f, m, &d), "cell outside the page refused");
    bad[0].x = 0; bad[0].chan = 4;
    m = make_file(f, 2, 2, 256, 128, 1, bad, 1, v_ramp);
    CHECK(!fp_parse(f, m, &d), "channel 4 refused");
    /* отпечаток не зависит от шага строки */
    uint8_t a[64 * 64 * 4], b[64 * 80 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 256; x++) { a[y * 256 + x] = (uint8_t)(x * 7 + y * 13); b[y * 320 + x] = a[y * 256 + x]; }
    CHECK(fp_hash97(a, 64, 64, 256) == fp_hash97(b, 64, 64, 320), "hash ignores row pitch");
}

static void test_atlas(void)
{
    static uint8_t f[1 << 16];
    size_t n = make_file(f, 2, 2, 256, 128, 7, GS, NG, v_ramp);
    FpData d;
    fp_parse(f, n, &d);
    FpAtlas at;
    CHECK(fp_atlas_layout(&d.pages[0], 2, 2, 1, &at), "layout x2");
    CHECK(at.own == 5 && at.pl[0].kind == FP_OWN && at.pl[2].kind == FP_BLANK && at.pl[4].kind == FP_NOT_OWN, "kinds %u", at.own);
    CHECK(at.pl[1].X0 == 0 && at.pl[1].Y0 == -2 && at.pl[1].X1 == 12 && at.pl[1].Y1 == 19, "x2 place = sample place");
    CHECK(at.pl[3].X0 == -2 && at.pl[3].Y0 == -3 && at.pl[3].X1 == 12 && at.pl[3].Y1 == 18, "wider than the cell kept whole");
    CHECK(overlap_free(&at), "no overlaps in a channel, gaps kept");
    uint8_t *px = (uint8_t *)malloc((size_t)at.AW * at.AH * 4);
    CHECK(px && fp_atlas_pixels(&at, px), "pixels");
    const FpPlace *p = &at.pl[1];
    int ok = 1;
    for (int y = 0; y < 21; y++)
        for (int x = 0; x < 12; x++) {
            const uint8_t *t = px + ((size_t)(p->ay + y) * at.AW + p->ax + x) * 4;
            ok &= t[2] == v_ramp(1, x, y);   /* в других каналах здесь могут лежать другие буквы */
        }
    CHECK(ok, "x2 picture as is, in its channel");
    free(px);
    fp_atlas_free(&at);

    /* ×1.5 из образца ×4: сетка от угла ячейки, площадь сохраняется */
    G g4[1] = {{0, 0, 8, 8, 0, -3, 1, 30, 28}};
    n = make_file(f, 2, 4, 64, 64, 9, g4, 1, v_full);
    fp_free(&d);
    fp_parse(f, n, &d);
    CHECK(fp_atlas_layout(&d.pages[0], 4, 3, 2, &at), "layout x1.5 from x4");
    p = &at.pl[0];
    /* физический px X = образец [X*8/3, (X+1)*8/3) от угла: -3 -> X0 = floor(-3*3/8) = -2; 27 -> X1 = ceil(27*3/8) = 11 */
    CHECK(p->X0 == -2 && p->X1 == 11 && p->Y0 == 0 && p->Y1 == 11, "x1.5 extents %d %d %d %d", p->X0, p->X1, p->Y0, p->Y1);
    uint8_t img[16 * 16];
    float tmp[64 * 16];
    fp_glyph_phys(&at, 0, img, tmp);
    double s = 0;
    for (int i = 0; i < (p->X1 - p->X0) * (p->Y1 - p->Y0); i++) s += img[i];
    CHECK(fabs(s * (8.0 / 3) * (8.0 / 3) - 255.0 * 30 * 28) < 255.0 * 30, "ink kept: %.0f vs %.0f", s * 64 / 9, 255.0 * 30 * 28);
    CHECK(img[5 * 13 + 5] == 255 && img[0] < 255, "inside full, edge partial");
    fp_atlas_free(&at);
    fp_free(&d);
}

static void test_rewrite(void)
{
    static uint8_t f[1 << 16];
    size_t nf = make_file(f, 2, 2, 256, 128, 7, GS, NG, v_ramp);
    FpData d;
    fp_parse(f, nf, &d);
    FpAtlas at;
    fp_atlas_layout(&d.pages[0], 2, 2, 1, &at);
    static uint8_t in[64 * FP_VSTRIDE], own[64 * FP_VSTRIDE], oth[64 * FP_VSTRIDE];
    uint32_t no, nt;
    FpCount c;
    FpMap m2 = {2, 0, 2, 0};
    for (int order = 0; order < 2; order++) {
        /* 0 (канал 0), 2 (пустая), 1 (канал 2), 3 (канал 1, шире ячейки), 4 (не своя) */
        L ls[5] = {{5, 7, 0, 0, 0}, {11, 7, 2, 0, 0}, {14, 7, 1, 2, 0}, {20, 7, 3, 1, 0}, {26, 7, 4, 3, 0}};
        uint32_t n = make_strip(in, ls, 5, 256, 128, order);
        int mode = -1;
        CHECK(n == 28, "strip n");
        CHECK(fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c), "rewritten (order %d)", order);
        CHECK(c.own == 3 && c.dropped == 1 && c.other == 1 && no == 16 && nt == 4, "counts %u %u %u n %u %u", c.own, c.dropped, c.other, no, nt);
        CHECK(mode == -1, "mode not learned: cells of channel 0 and 2 coincide");
        double r[8];
        rect_of(own, 0, r);   /* буква 0: угол ячейки (5, 7) -> физ. (10, 14); картинка (-1, 0) 13x18 */
        CHECK(r[0] == 4.5 && r[1] == 7 && r[2] == 11 && r[3] == 16, "letter 0 place %g %g %g %g", r[0], r[1], r[2], r[3]);
        CHECK(fabs(r[4] * at.AW - at.pl[0].ax) < 1e-3 && fabs(r[6] * at.AW - at.pl[0].ax - 13) < 1e-3 &&
              fabs(r[5] * at.AH - at.pl[0].ay) < 1e-3 && fabs(r[7] * at.AH - at.pl[0].ay - 18) < 1e-3, "letter 0 uv");
        rect_of(own, 2, r);   /* буква 3: угол (20, 7) -> (40, 14); картинка (-2, -3) 14x21: выше ячейки на 1.5 логич. */
        CHECK(r[0] == 19 && r[1] == 5.5 && r[2] == 26 && r[3] == 16, "wide letter whole %g %g %g %g", r[0], r[1], r[2], r[3]);
        /* полоса своих: повторённые вершины между буквами, цвет (маска) сохранён */
        CHECK(!memcmp(own + 3 * FP_VSTRIDE, own + 4 * FP_VSTRIDE, FP_VSTRIDE) &&
              !memcmp(own + 5 * FP_VSTRIDE, own + 6 * FP_VSTRIDE, FP_VSTRIDE), "degenerate vertices");
        CHECK(own[6 * FP_VSTRIDE + FP_VCOLOR + 2] == 255, "mask kept");
        CHECK(!memcmp(oth, in + 24 * FP_VSTRIDE, 4 * FP_VSTRIDE), "other letter unchanged");
    }
    /* три совпадения ячеек — способ маски запоминается; переставленные байты (R8G8B8A8) — тоже */
    {
        L ls[3] = {{5, 7, 5, 0, 0}, {14, 7, 6, 2, 0}, {30, 7, 5, 0, 0}};
        uint32_t n = make_strip(in, ls, 3, 256, 128, 0);
        int mode = -1;
        fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c);
        CHECK(mode == 0 && c.own == 3, "mode 0 learned (%d)", mode);
        L sw[3] = {{5, 7, 5, 2, 0}, {14, 7, 6, 0, 0}, {30, 7, 5, 2, 0}};
        n = make_strip(in, sw, 3, 256, 128, 0);
        mode = -1;
        fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c);
        CHECK(mode == 1 && c.own == 3, "mode 1 learned (%d)", mode);
        double r[8];
        rect_of(own, 1, r);   /* маска байт 0 при mode 1 -> канал 2 -> буква 6 */
        CHECK(r[0] == 14 && r[1] == 7 && fabs(r[4] * at.AW - at.pl[6].ax) < 1e-3, "swapped mask picks channel 2 letter %g %g", r[0], r[1]);
    }
    /* обрезка рамкой: слева срезано 2 px — край там же; у соседа, не задетого рамкой, картинка шире ячейки слева —
     * тоже по этому краю */
    {
        L ls[2] = {{5, 7, 0, 0, 2}, {7, 20, 3, 1, 0}};
        uint32_t n = make_strip(in, ls, 2, 256, 128, 0);
        int mode = 0;
        CHECK(fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c) && c.own == 2, "clipped rewritten");
        double r[8];
        rect_of(own, 0, r);
        CHECK(r[0] == 7 && fabs(r[4] * at.AW - (at.pl[0].ax + 5)) < 1e-3, "clip edge kept, uv moved %g", r[0]);
        rect_of(own, 1, r);   /* буква 3 в (7, 20), ячейка не задета: картинка от 6 — рамка с 7 */
        CHECK(r[0] == 7 && fabs(r[4] * at.AW - (at.pl[3].ax + 2)) < 1e-3, "neighbour clipped by the same edge %g", r[0]);
    }
    /* ×1.5: угол ячейки к целому физическому px, картинка 1:1 */
    {
        FpAtlas a15;
        fp_atlas_layout(&d.pages[0], 2, 3, 2, &a15);
        L ls[1] = {{5, 7, 0, 0, 0}};
        uint32_t n = make_strip(in, ls, 1, 256, 128, 0);
        int mode = 0;
        FpMap m15 = {1.5, 0, 1.5, 0};
        CHECK(fp_rewrite(&a15, &m15, &mode, in, n, own, &no, oth, &nt, &c) && no == 4, "x1.5 rewritten");
        double r[8];
        rect_of(own, 0, r);
        const FpPlace *p = &a15.pl[0];
        double L0 = r[0] * 1.5, T0 = r[1] * 1.5, R0 = r[2] * 1.5, B0 = r[3] * 1.5;
        CHECK(fabs(L0 - floor(L0 + .5)) < 1e-4 && fabs(T0 - floor(T0 + .5)) < 1e-4 && fabs(R0 - L0 - (p->X1 - p->X0)) < 1e-4 &&
              fabs(B0 - T0 - (p->Y1 - p->Y0)) < 1e-4, "integer physical px, 1:1 (%g %g %g %g)", L0, T0, R0, B0);
        CHECK(fabs(L0 - (8 + p->X0)) < 1e-4 && fabs(T0 - (11 + p->Y0)) < 1e-4, "corner 7.5,10.5 rounded to 8,11");
        fp_atlas_free(&a15);
    }
    /* не полоса букв — как есть */
    {
        L ls[2] = {{5, 7, 0, 0, 0}, {14, 7, 1, 2, 0}};
        uint32_t n = make_strip(in, ls, 2, 256, 128, 0);
        int mode = 0;
        CHECK(!fp_rewrite(&at, &m2, &mode, in, n - 1, own, &no, oth, &nt, &c), "n not 6q-2");
        float bump = 1;
        memcpy(in + 4 * FP_VSTRIDE, &bump, 4);
        CHECK(!fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c), "broken degenerate");
        L ot[1] = {{5, 7, 4, 3, 0}};
        n = make_strip(in, ot, 1, 256, 128, 0);
        CHECK(!fp_rewrite(&at, &m2, &mode, in, n, own, &no, oth, &nt, &c), "nothing own - drawn as is");
    }
    fp_atlas_free(&at);
    fp_free(&d);
}

static void test_real(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { CHECK(0, "cannot open %s", path); return; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *f = (uint8_t *)malloc((size_t)n);
    size_t got = fread(f, 1, (size_t)n, fp);
    fclose(fp);
    FpData d;
    CHECK(got == (size_t)n && fp_parse(f, (size_t)n, &d), "parse real file");
    uint32_t pages = 0, own = 0, blank = 0, letters = 0, bad = 0;
    for (uint32_t i = 0; i < d.npages; i++) {
        static const uint32_t S[3][2] = {{2, 1}, {3, 2}, {7, 4}};
        for (int s = 0; s < 3; s++) {
            FpAtlas at;
            if (!fp_atlas_layout(&d.pages[i], d.K, S[s][0], S[s][1], &at)) { bad++; continue; }
            uint8_t *px = (uint8_t *)malloc((size_t)at.AW * at.AH * 4);
            if (!px || !fp_atlas_pixels(&at, px) || !overlap_free(&at)) bad++;
            if (s == 0) {
                pages++;
                letters += at.n;
                own += at.own;
                for (uint32_t k = 0; k < at.n; k++) blank += at.pl[k].kind == FP_BLANK;
            }
            free(px);
            fp_atlas_free(&at);
        }
    }
    printf("real x%u: %u pages, %u letters: own %u, blank %u, left to the game %u\n", d.K, pages, letters, own, blank,
           letters - own - blank);
    CHECK(!bad, "%u atlases failed", bad);
    CHECK(own + blank == letters, "every letter of the samples is own or blank");
    fp_free(&d);
    free(f);
}

int main(int argc, char **argv)
{
    test_parse();
    test_atlas();
    test_rewrite();
    if (argc > 1) test_real(argv[1]);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
