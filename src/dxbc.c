/* Отражение шейдера DXBC: контейнер «DXBC» (заголовок 32 байта, затем смещения частей), часть «RDEF»:
 *   заголовок: число буферов, их смещение, число привязок, их смещение, версия (младший байт — minor, следующий — major);
 *   буфер (24 байта): имя, число переменных, смещение переменных, размер, флаги, тип;
 *   переменная: имя, смещение, размер, флаги, тип, значение по умолчанию (24 байта; с версии 5 ещё 4 числа — 40);
 *   привязка (32 байта): имя, тип (0 — буфер констант), тип значения, измерение, выборки, регистр, число, флаги.
 * Все смещения внутри RDEF — от начала её данных. Каждое чтение проверяется на выход за границы. */
#include <string.h>
#include "dxbc.h"

static int rd(const unsigned char *p, size_t n, size_t off, unsigned *v)
{
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static const char *str_at(const unsigned char *p, size_t n, unsigned off)
{
    if (off >= n || !memchr(p + off, 0, n - off)) return NULL;
    return (const char *)p + off;
}

static int find_in_rdef(const unsigned char *c, size_t n, const char *name, DxbcVar *out)
{
    unsigned ncb, cboff, nbind, bindoff, ver;
    if (!rd(c, n, 0, &ncb) || !rd(c, n, 4, &cboff) || !rd(c, n, 8, &nbind) || !rd(c, n, 12, &bindoff) || !rd(c, n, 16, &ver))
        return 0;
    size_t vsz = ((ver >> 8) & 0xff) >= 5 ? 40 : 24;
    for (unsigned b = 0; b < ncb && b < 64; b++) {
        size_t d = (size_t)cboff + 24u * b;
        unsigned cbname, nvar, varoff, cbsize;
        if (!rd(c, n, d, &cbname) || !rd(c, n, d + 4, &nvar) || !rd(c, n, d + 8, &varoff) || !rd(c, n, d + 12, &cbsize))
            return 0;
        for (unsigned v = 0; v < nvar && v < 1024; v++) {
            size_t e = (size_t)varoff + vsz * v;
            unsigned vname, start, size, vflags;
            if (!rd(c, n, e, &vname) || !rd(c, n, e + 4, &start) || !rd(c, n, e + 8, &size) || !rd(c, n, e + 12, &vflags)) return 0;
            const char *s = str_at(c, n, vname);
            if (!s || strcmp(s, name)) continue;
            const char *bn = str_at(c, n, cbname);
            for (unsigned i = 0; bn && i < nbind && i < 256; i++) {
                size_t g = (size_t)bindoff + 32u * i;
                unsigned gname, type, point;
                if (!rd(c, n, g, &gname) || !rd(c, n, g + 4, &type) || !rd(c, n, g + 20, &point)) return 0;
                const char *gs = str_at(c, n, gname);
                if (type == 0 && gs && !strcmp(gs, bn)) {
                    out->slot = point;
                    out->cb_bytes = cbsize;
                    out->offset = start;
                    out->size = size;
                    out->used = (vflags & 2) != 0;   /* D3D_SVF_USED */
                    return 1;
                }
            }
            return 0;   /* переменная есть, а привязки её буфера нет */
        }
    }
    return 0;
}

static const unsigned char *rdef(const void *code, size_t len, size_t *n)
{
    const unsigned char *p = (const unsigned char *)code;
    unsigned total, nch;
    if (!p || len < 32 || memcmp(p, "DXBC", 4) || !rd(p, len, 24, &total) || !rd(p, len, 28, &nch)) return NULL;
    if (total > len || total < 32) return NULL;
    len = total;
    for (unsigned i = 0; i < nch && i < 64; i++) {
        unsigned off, size;
        if (!rd(p, len, 32 + 4u * i, &off) || !rd(p, len, (size_t)off + 4, &size)) return NULL;
        if ((size_t)off + 8 > len || len - off - 8 < size) return NULL;
        if (!memcmp(p + off, "RDEF", 4)) { *n = size; return p + off + 8; }
    }
    return NULL;
}

int dxbc_find_binding(const void *code, size_t len, const char *name)
{
    size_t n;
    const unsigned char *c = rdef(code, len, &n);
    unsigned nbind, bindoff;
    if (!c || !name || !rd(c, n, 8, &nbind) || !rd(c, n, 12, &bindoff)) return -1;
    for (unsigned i = 0; i < nbind && i < 256; i++) {
        size_t g = (size_t)bindoff + 32u * i;
        unsigned gname, point;
        if (!rd(c, n, g, &gname) || !rd(c, n, g + 20, &point)) return -1;
        const char *s = str_at(c, n, gname);
        if (s && !strcmp(s, name)) return (int)point;
    }
    return -1;
}

int dxbc_same_bindings(const void *a, size_t an, const void *b, size_t bn)
{
    size_t na, nb;
    const unsigned char *ca = rdef(a, an, &na), *cb = rdef(b, bn, &nb);
    unsigned ka, oa, kb, ob;
    if (!ca || !cb || !rd(ca, na, 8, &ka) || !rd(ca, na, 12, &oa) || !rd(cb, nb, 8, &kb) || !rd(cb, nb, 12, &ob)) return 0;
    for (unsigned i = 0; i < ka && i < 256; i++) {
        size_t g = (size_t)oa + 32u * i;
        unsigned nm, type, point;
        if (!rd(ca, na, g, &nm) || !rd(ca, na, g + 4, &type) || !rd(ca, na, g + 20, &point)) return 0;
        const char *s = str_at(ca, na, nm);
        if (!s) return 0;
        int ok = 0;
        for (unsigned j = 0; j < kb && j < 256 && !ok; j++) {
            size_t h = (size_t)ob + 32u * j;
            unsigned nm2, type2, point2;
            if (!rd(cb, nb, h, &nm2) || !rd(cb, nb, h + 4, &type2) || !rd(cb, nb, h + 20, &point2)) return 0;
            const char *s2 = str_at(cb, nb, nm2);
            ok = s2 && !strcmp(s, s2) && type == type2 && point == point2;
        }
        if (!ok) return 0;
    }
    return 1;
}

int dxbc_find_var(const void *code, size_t len, const char *name, DxbcVar *out)
{
    const unsigned char *p = (const unsigned char *)code;
    unsigned total, nch;
    if (!p || !name || !out || len < 32 || memcmp(p, "DXBC", 4) || !rd(p, len, 24, &total) || !rd(p, len, 28, &nch)) return 0;
    if (total > len || total < 32) return 0;   /* обрезанный байткод (D3D такой и не примет) */
    len = total;
    for (unsigned i = 0; i < nch && i < 64; i++) {
        unsigned off, size;
        if (!rd(p, len, 32 + 4u * i, &off) || !rd(p, len, (size_t)off + 4, &size)) return 0;
        if ((size_t)off + 8 > len || len - off - 8 < size) return 0;
        if (!memcmp(p + off, "RDEF", 4)) return find_in_rdef(p + off + 8, size, name, out);
    }
    return 0;
}
