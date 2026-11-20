/* Э6: чистая часть чётких букв (см. fontpages.h). Без Windows и Direct3D. */
#include "fontpages.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

#define REC 18   /* заголовок буквы в файле */

int fp_parse(const uint8_t *f, size_t n, FpData *out)
{
    memset(out, 0, sizeof *out);
    if (!f || n < 16 || memcmp(f, "U4KF", 4)) return 0;
    uint32_t ver = rd32(f + 4), K = rd32(f + 8), np = rd32(f + 12);
    size_t es = ver == 3 ? 32 : 24;   /* запись страницы: у версии 3 ещё CRC32 и запас */
    if ((ver != 2 && ver != 3) || K < 1 || K > 8 || np > FP_MAX_PAGES || 16 + (size_t)np * es > n) return 0;
    size_t total = 0;
    for (uint32_t i = 0; i < np; i++) total += rd32(f + 16 + (size_t)i * es + 16);
    if (total > n / 20) return 0;
    out->pages = (FpPage *)calloc(np ? np : 1, sizeof(FpPage));
    out->glyphs = (FpGlyph *)calloc(total ? total : 1, sizeof(FpGlyph));
    if (!out->pages || !out->glyphs) { fp_free(out); return 0; }
    out->K = K;
    out->npages = np;
    size_t gi = 0;
    for (uint32_t i = 0; i < np; i++) {
        const uint8_t *e = f + 16 + (size_t)i * es;
        FpPage *pg = &out->pages[i];
        pg->w = rd32(e);
        pg->h = rd32(e + 4);
        pg->hash = (uint64_t)rd32(e + 8) | (uint64_t)rd32(e + 12) << 32;
        pg->n = rd32(e + 16);
        pg->g = &out->glyphs[gi];
        pg->has_crc = ver == 3;
        pg->crc = ver == 3 ? rd32(e + 24) : 0;
        size_t p = rd32(e + 20);
        if (!pg->w || !pg->h || pg->w > 16384 || pg->h > 16384) { fp_free(out); return 0; }
        for (uint32_t j = 0; j < pg->n; j++, gi++) {
            if (p + REC > n) { fp_free(out); return 0; }
            FpGlyph *g = &out->glyphs[gi];
            g->x = rd16(f + p); g->y = rd16(f + p + 2); g->w = rd16(f + p + 4); g->h = rd16(f + p + 6);
            g->chan = f[p + 8];
            g->ox = (int16_t)rd16(f + p + 10); g->oy = (int16_t)rd16(f + p + 12);
            g->iw = rd16(f + p + 14); g->ih = rd16(f + p + 16);
            size_t sz = (size_t)g->iw * g->ih;
            if (g->chan > 3 || !g->w || !g->h || !g->iw || !g->ih || (uint32_t)g->x + g->w > pg->w ||
                (uint32_t)g->y + g->h > pg->h || p + REC + sz > n) { fp_free(out); return 0; }
            g->px = f + p + REC;
            p = (p + REC + sz + 3) & ~(size_t)3;
        }
    }
    return 1;
}

void fp_free(FpData *d)
{
    free(d->pages);
    free(d->glyphs);
    memset(d, 0, sizeof *d);
}

const FpPage *fp_find(const FpData *d, uint32_t w, uint32_t h, uint64_t hash)
{
    for (uint32_t i = 0; d && i < d->npages; i++)
        if (d->pages[i].hash == hash && d->pages[i].w == w && d->pages[i].h == h) return &d->pages[i];
    return NULL;
}

uint32_t fp_crc32(const uint8_t *data, uint32_t w, uint32_t h, uint32_t rp)
{
    uint32_t t[256];   /* таблица на месте: 2 тыс. операций против сотен тысяч байт страницы, и никакого общего состояния */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *p = data + (size_t)y * rp;
        for (uint32_t x = 0; x < w * 4; x++) c = t[(c ^ p[x]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint64_t fp_hash97(const uint8_t *data, uint32_t w, uint32_t h, uint32_t rp)
{
    uint64_t x = 0xcbf29ce484222325ull;
    size_t row = (size_t)w * 4, total = row * h;
    for (size_t i = 0; i < total; i += 97) {
        x ^= data[(i / row) * rp + i % row];
        x *= 0x100000001b3ull;
    }
    return x;
}

/* ---- атлас ---- */
static int64_t fdiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b && (a < 0) != (b < 0)) ? q - 1 : q; }
static int64_t cdiv(int64_t a, int64_t b) { return -fdiv(-a, b); }

static int inked(const FpGlyph *g)
{
    for (size_t i = 0, n = (size_t)g->iw * g->ih; i < n; i++)
        if (g->px[i]) return 1;
    return 0;
}

static int cmp_desc(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* полки по каналам при ширине AW; 1 — всё влезло, *ah — высота самого высокого канала */
static int pack(FpAtlas *at, const uint64_t *ord, uint32_t no, uint32_t AW, uint32_t *ah)
{
    uint32_t hmax = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t x = FP_GAP, y = FP_GAP, shelf = 0;
        for (uint32_t k = 0; k < no; k++) {
            uint32_t i = (uint32_t)(ord[k] & 0xffffffffu);
            if (at->pg->g[i].chan != c) continue;
            FpPlace *p = &at->pl[i];
            uint32_t w = (uint32_t)(p->X1 - p->X0), h = (uint32_t)(p->Y1 - p->Y0);
            if (w + 2 * FP_GAP > AW) return 0;
            if (x + w + FP_GAP > AW) { y += shelf + FP_GAP; x = FP_GAP; shelf = 0; }
            p->ax = x;
            p->ay = y;
            x += w + FP_GAP;
            if (h > shelf) shelf = h;
        }
        if (y + shelf + FP_GAP > hmax) hmax = y + shelf + FP_GAP;
    }
    *ah = hmax;
    return 1;
}

int fp_atlas_layout(const FpPage *pg, uint32_t K, uint32_t num, uint32_t den, FpAtlas *at)
{
    memset(at, 0, sizeof *at);
    if (!pg || !K || !num || !den) return 0;
    at->pl = (FpPlace *)calloc(pg->n ? pg->n : 1, sizeof(FpPlace));
    uint64_t *ord = (uint64_t *)malloc((pg->n ? pg->n : 1) * sizeof(uint64_t));
    if (!at->pl || !ord) { free(ord); fp_atlas_free(at); return 0; }
    at->K = K; at->num = num; at->den = den; at->n = pg->n; at->pg = pg;
    int64_t KD = (int64_t)K * den, o = (int64_t)FP_OVERHANG * num;
    uint32_t no = 0;
    for (uint32_t i = 0; i < pg->n; i++) {
        const FpGlyph *g = &pg->g[i];
        if (!inked(g)) { at->pl[i].kind = FP_BLANK; continue; }
        int64_t X0 = fdiv((int64_t)g->ox * num, KD), X1 = cdiv(((int64_t)g->ox + g->iw) * num, KD);
        int64_t Y0 = fdiv((int64_t)g->oy * num, KD), Y1 = cdiv(((int64_t)g->oy + g->ih) * num, KD);
        /* дальше FP_OVERHANG логических px от ячейки — образец испорчен, букву оставить игре */
        if (X0 * den < -o || Y0 * den < -o || X1 * den > (int64_t)g->w * num + o || Y1 * den > (int64_t)g->h * num + o ||
            X1 - X0 > 4096 || Y1 - Y0 > 4096) continue;
        FpPlace *p = &at->pl[i];
        p->X0 = (int32_t)X0; p->X1 = (int32_t)X1; p->Y0 = (int32_t)Y0; p->Y1 = (int32_t)Y1;
        p->kind = FP_OWN;
        ord[no++] = (uint64_t)(Y1 - Y0) << 48 | (uint64_t)(X1 - X0) << 32 | i;   /* выше — раньше */
    }
    qsort(ord, no, sizeof *ord, cmp_desc);
    uint32_t AW = 64, AH = 0;
    for (;; AW *= 2) {
        if (pack(at, ord, no, AW, &AH) && (AH <= AW || AW >= 4096)) break;
        if (AW >= 4096) { free(ord); fp_atlas_free(at); return 0; }
    }
    free(ord);
    if (AH > 4096) { fp_atlas_free(at); return 0; }
    at->AW = AW;
    at->AH = (AH + 3) & ~3u;
    if (at->AH < 4) at->AH = 4;
    at->own = no;
    return 1;
}

void fp_atlas_free(FpAtlas *at)
{
    free(at->pl);
    memset(at, 0, sizeof *at);
}

/* сумма v[j*stride] на отрезке [a, b) с долями крайних, j в [0, len) */
static double cover8(const uint8_t *v, size_t stride, uint32_t len, double a, double b)
{
    if (a < 0) a = 0;
    if (b > len) b = len;
    double s = 0;
    for (uint32_t j = (uint32_t)floor(a); j < len && j < b; j++) {
        double lo = j < a ? a : j, hi = j + 1 > b ? b : j + 1;
        if (hi > lo) s += (hi - lo) * v[(size_t)j * stride];
    }
    return s;
}
static double coverf(const float *v, size_t stride, uint32_t len, double a, double b)
{
    if (a < 0) a = 0;
    if (b > len) b = len;
    double s = 0;
    for (uint32_t j = (uint32_t)floor(a); j < len && j < b; j++) {
        double lo = j < a ? a : j, hi = j + 1 > b ? b : j + 1;
        if (hi > lo) s += (hi - lo) * v[(size_t)j * stride];
    }
    return s;
}

void fp_glyph_phys(const FpAtlas *at, uint32_t i, uint8_t *out, float *tmp)
{
    const FpGlyph *g = &at->pg->g[i];
    const FpPlace *p = &at->pl[i];
    uint32_t ow = (uint32_t)(p->X1 - p->X0), oh = (uint32_t)(p->Y1 - p->Y0);
    if (at->num == at->K * at->den) {   /* q == K: образец как есть (X0 == ox) */
        for (uint32_t y = 0; y < oh; y++) memcpy(out + (size_t)y * ow, g->px + (size_t)y * g->iw, ow);
        return;
    }
    /* физический px X (от угла ячейки) = образец [X*K/q, (X+1)*K/q) от угла ячейки; s — ширина в пикселях образца */
    double s = (double)at->K * at->den / at->num;
    for (uint32_t r = 0; r < g->ih; r++)
        for (uint32_t j = 0; j < ow; j++) {
            double a = (p->X0 + (int32_t)j) * s - g->ox;
            tmp[(size_t)r * ow + j] = (float)cover8(g->px + (size_t)r * g->iw, 1, g->iw, a, a + s);
        }
    for (uint32_t y = 0; y < oh; y++) {
        double a = (p->Y0 + (int32_t)y) * s - g->oy;
        for (uint32_t j = 0; j < ow; j++) {
            double v = coverf(tmp + j, ow, g->ih, a, a + s) / (s * s);
            out[(size_t)y * ow + j] = (uint8_t)(v >= 255 ? 255 : v <= 0 ? 0 : floor(v + 0.5));
        }
    }
}

int fp_atlas_pixels(const FpAtlas *at, uint8_t *out)
{
    size_t big = 1, bigt = 1;
    for (uint32_t i = 0; i < at->n; i++) {
        const FpPlace *p = &at->pl[i];
        if (p->kind != FP_OWN) continue;
        size_t ow = (size_t)(p->X1 - p->X0), a = ow * (size_t)(p->Y1 - p->Y0), b = ow * at->pg->g[i].ih;
        if (a > big) big = a;
        if (b > bigt) bigt = b;
    }
    uint8_t *img = (uint8_t *)malloc(big);
    float *tmp = (float *)malloc(bigt * sizeof(float));
    if (!img || !tmp) { free(img); free(tmp); return 0; }
    memset(out, 0, (size_t)at->AW * at->AH * 4);
    for (uint32_t i = 0; i < at->n; i++) {
        const FpPlace *p = &at->pl[i];
        if (p->kind != FP_OWN) continue;
        uint32_t ow = (uint32_t)(p->X1 - p->X0), oh = (uint32_t)(p->Y1 - p->Y0), c = at->pg->g[i].chan;
        fp_glyph_phys(at, i, img, tmp);
        for (uint32_t y = 0; y < oh; y++) {
            uint8_t *o = out + ((size_t)(p->ay + y) * at->AW + p->ax) * 4 + c;
            for (uint32_t x = 0; x < ow; x++) o[(size_t)x * 4] = img[(size_t)y * ow + x];
        }
    }
    free(img);
    free(tmp);
    return 1;
}

/* ---- вершины ---- */
typedef struct {
    float x0, x1, y0, y1, u0, u1, v0, v1;
    int mb;                        /* байт маски (единственный ненулевой) или -1 */
    int t1;                        /* целые тексели и 1 тексель = 1 логический px */
    int32_t tx0, ty0, tx1, ty1;
} Quad;

static float fget(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static void fput(uint8_t *p, float f) { memcpy(p, &f, 4); }

static int texel(double a, int32_t *t)
{
    double r = floor(a + 0.5);
    *t = (int32_t)r;
    return fabs(a - r) < 0.02;
}

/* прямоугольник буквы из 4 вершин (порядок любой); 0 — не прямоугольник с прямыми UV */
static int quad_parse(const uint8_t *q, uint32_t W, uint32_t H, Quad *o)
{
    float x[4], y[4], u[4], v[4];
    for (int k = 0; k < 4; k++) {
        const uint8_t *p = q + (size_t)k * FP_VSTRIDE;
        x[k] = fget(p); y[k] = fget(p + 4); u[k] = fget(p + FP_VUV); v[k] = fget(p + FP_VUV + 4);
    }
    o->x0 = o->x1 = x[0]; o->y0 = o->y1 = y[0];
    for (int k = 1; k < 4; k++) {
        if (x[k] < o->x0) o->x0 = x[k];
        if (x[k] > o->x1) o->x1 = x[k];
        if (y[k] < o->y0) o->y0 = y[k];
        if (y[k] > o->y1) o->y1 = y[k];
    }
    if (!(o->x1 > o->x0 && o->y1 > o->y0)) return 0;
    int corners = 0, hu[2] = {0, 0}, hv[2] = {0, 0};
    float uu[2] = {0, 0}, vv[2] = {0, 0};
    for (int k = 0; k < 4; k++) {
        int r = x[k] == o->x1, b = y[k] == o->y1;
        if (!r && x[k] != o->x0) return 0;
        if (!b && y[k] != o->y0) return 0;
        corners |= 1 << (r + 2 * b);
        if (hu[r] && uu[r] != u[k]) return 0;
        if (hv[b] && vv[b] != v[k]) return 0;
        uu[r] = u[k]; hu[r] = 1;
        vv[b] = v[k]; hv[b] = 1;
    }
    if (corners != 15 || !(uu[1] > uu[0] && vv[1] > vv[0])) return 0;
    o->u0 = uu[0]; o->u1 = uu[1]; o->v0 = vv[0]; o->v1 = vv[1];
    const uint8_t *c = q + FP_VCOLOR;
    int nz = 0;
    o->mb = -1;
    for (int b = 0; b < 4; b++)
        if (c[b]) { nz++; o->mb = b; }
    if (nz != 1) o->mb = -1;
    o->t1 = texel((double)o->u0 * W, &o->tx0) & texel((double)o->u1 * W, &o->tx1) & texel((double)o->v0 * H, &o->ty0) &
            texel((double)o->v1 * H, &o->ty1);
    o->t1 = o->t1 && o->tx1 > o->tx0 && o->ty1 > o->ty0 && fabs((double)(o->x1 - o->x0) - (o->tx1 - o->tx0)) < 0.02 &&
            fabs((double)(o->y1 - o->y0) - (o->ty1 - o->ty0)) < 0.02;
    return 1;
}

static int chan_of(int mode, int mb) { return mode == 1 && (mb == 0 || mb == 2) ? 2 - mb : mb; }

/* своя буква страницы в канале c, чья ячейка содержит тексели квадрата; exact — ячейка ровно они. -1 — нет */
static int find(const FpAtlas *at, int c, const Quad *q, int exact)
{
    for (uint32_t i = 0; i < at->n; i++) {
        const FpGlyph *g = &at->pg->g[i];
        if (g->chan != c) continue;
        if (exact ? (g->x == q->tx0 && g->y == q->ty0 && g->x + g->w == q->tx1 && g->y + g->h == q->ty1)
                  : (g->x <= q->tx0 && g->y <= q->ty0 && g->x + g->w >= q->tx1 && g->y + g->h >= q->ty1))
            return (int)i;
    }
    return -1;
}

/* дописать 4 вершины квадрата в полосу (перед ним — 2 повторённые: последняя прежнего и первая нового) */
static void emit(uint8_t *out, uint32_t *n, const uint8_t *quad)
{
    if (*n) {
        memcpy(out + (size_t)*n * FP_VSTRIDE, out + (size_t)(*n - 1) * FP_VSTRIDE, FP_VSTRIDE);
        memcpy(out + (size_t)(*n + 1) * FP_VSTRIDE, quad, FP_VSTRIDE);
        *n += 2;
    }
    memcpy(out + (size_t)*n * FP_VSTRIDE, quad, 4 * FP_VSTRIDE);
    *n += 4;
}

int fp_rewrite(const FpAtlas *at, const FpMap *m, int *mode, const uint8_t *in, uint32_t n, uint8_t *own, uint32_t *no,
               uint8_t *oth, uint32_t *nt, FpCount *cnt)
{
    *no = *nt = 0;
    if (cnt) memset(cnt, 0, sizeof *cnt);
    if (!at || !at->own || n < 4 || (n + 2) % 6 || !(m->rx > 0) || !(m->ry > 0)) return 0;
    uint32_t nq = (n + 2) / 6, W = at->pg->w, H = at->pg->h;
    /* полоса: между буквами повторены последняя вершина прежней и первая следующей */
    for (uint32_t i = 0; i + 1 < nq; i++) {
        const uint8_t *a = in + (size_t)(6 * i + 3) * FP_VSTRIDE, *b = in + (size_t)(6 * i + 6) * FP_VSTRIDE;
        if (memcmp(a, a + FP_VSTRIDE, 8) || memcmp(b - FP_VSTRIDE, b, 8)) return 0;
    }
    Quad stackq[64], *qs = nq <= 64 ? stackq : (Quad *)malloc(nq * sizeof(Quad));
    if (!qs) return 0;
    int ok = 1;
    for (uint32_t i = 0; i < nq && ok; i++) ok = quad_parse(in + (size_t)6 * i * FP_VSTRIDE, W, H, &qs[i]);
    int md = *mode;
    if (ok && md < 0) {   /* какой байт маски какой канал: по ячейкам, совпавшим ровно */
        uint32_t v0 = 0, v1 = 0;
        for (uint32_t i = 0; i < nq; i++)
            if (qs[i].t1 && qs[i].mb >= 0 && (qs[i].mb == 0 || qs[i].mb == 2)) {
                v0 += find(at, chan_of(0, qs[i].mb), &qs[i], 1) >= 0;
                v1 += find(at, chan_of(1, qs[i].mb), &qs[i], 1) >= 0;
            }
        md = v0 > v1 ? 0 : v1 > v0 ? 1 : -1;
        if ((v0 >= 3 && !v1) || (v1 >= 3 && !v0)) *mode = md;
        if (md < 0) {   /* только каналы 1 и 3 — им всё равно */
            int any13 = 0;
            for (uint32_t i = 0; i < nq; i++) any13 |= qs[i].mb == 1 || qs[i].mb == 3;
            md = any13 && !v0 && !v1 ? 0 : -1;
        }
        ok = md >= 0;
    }
    /* рамка обрезки (param_6) одна на рисование: её края видны по буквам, которые игра обрезала (квадрат не доходит до
     * края ячейки). Тот же край — и для своих картинок, которые шире ячейки: иначе они выступали бы за рамку на 1-3 px */
    double cl[4] = {-1e30, -1e30, 1e30, 1e30};
    for (uint32_t i = 0; i < nq && ok; i++) {
        const Quad *q = &qs[i];
        int gi = q->t1 && q->mb >= 0 ? find(at, chan_of(md, q->mb), q, 0) : -1;
        if (gi < 0) continue;
        const FpGlyph *g = &at->pg->g[gi];
        if (q->tx0 > g->x && q->x0 > cl[0]) cl[0] = q->x0;
        if (q->ty0 > g->y && q->y0 > cl[1]) cl[1] = q->y0;
        if (q->tx1 < g->x + g->w && q->x1 < cl[2]) cl[2] = q->x1;
        if (q->ty1 < g->y + g->h && q->y1 < cl[3]) cl[3] = q->y1;
    }
    FpCount c = {0, 0, 0};
    uint8_t quad[4 * FP_VSTRIDE];
    for (uint32_t i = 0; i < nq && ok; i++) {
        const Quad *q = &qs[i];
        const uint8_t *src = in + (size_t)6 * i * FP_VSTRIDE;
        int gi = q->t1 && q->mb >= 0 ? find(at, chan_of(md, q->mb), q, 0) : -1;
        const FpPlace *p = gi >= 0 ? &at->pl[gi] : NULL;
        if (p && p->kind == FP_BLANK) { c.dropped++; continue; }
        if (!p || p->kind != FP_OWN) { emit(oth, nt, src); c.other++; continue; }
        const FpGlyph *g = &at->pg->g[gi];
        /* угол ячейки (логический) — по месту текселей квадрата в ячейке: у обрезанной буквы квадрат меньше ячейки */
        double cx = q->x0 - (q->tx0 - g->x), cy = q->y0 - (q->ty0 - g->y);
        double SX = floor(m->rx * cx + m->bx + 0.5), SY = floor(m->ry * cy + m->by + 0.5);
        double L = SX + p->X0, R = SX + p->X1, T = SY + p->Y0, B = SY + p->Y1;
        double uL = p->ax, uR = p->ax + (double)(p->X1 - p->X0), vT = p->ay, vB = p->ay + (double)(p->Y1 - p->Y0);
        /* стороны по рамке обрезки игры */
        { double e = m->rx * cl[0] + m->bx; if (e > L) { uL += e - L; L = e; } }
        { double e = m->rx * cl[2] + m->bx; if (e < R) { uR -= R - e; R = e; } }
        { double e = m->ry * cl[1] + m->by; if (e > T) { vT += e - T; T = e; } }
        { double e = m->ry * cl[3] + m->by; if (e < B) { vB -= B - e; B = e; } }
        if (R <= L || B <= T) { c.dropped++; continue; }
        float lx0 = (float)((L - m->bx) / m->rx), lx1 = (float)((R - m->bx) / m->rx);
        float ly0 = (float)((T - m->by) / m->ry), ly1 = (float)((B - m->by) / m->ry);
        memcpy(quad, src, sizeof quad);
        for (int k = 0; k < 4; k++) {
            uint8_t *v = quad + (size_t)k * FP_VSTRIDE;
            int r = fget(v) == q->x1, b = fget(v + 4) == q->y1;
            fput(v, r ? lx1 : lx0);
            fput(v + 4, b ? ly1 : ly0);
            fput(v + FP_VUV, (float)((r ? uR : uL) / at->AW));
            fput(v + FP_VUV + 4, (float)((b ? vB : vT) / at->AH));
        }
        emit(own, no, quad);
        c.own++;
    }
    if (qs != stackq) free(qs);
    if (cnt) *cnt = c;
    if (!ok || (!c.own && !c.dropped)) { *no = *nt = 0; return 0; }
    return 1;
}
