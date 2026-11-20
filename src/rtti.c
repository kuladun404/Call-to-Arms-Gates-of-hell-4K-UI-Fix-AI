/* Э9: классы игры по именам RTTI MSVC (см. rtti.h). */
#include <string.h>
#include "rtti.h"

static unsigned rd32(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long rd64(const unsigned char *p) { return (unsigned long long)rd32(p) | (unsigned long long)rd32(p + 4) << 32; }

int img_sections(const unsigned char *img, ImgSec *out, int max)
{
    if (!img || img[0] != 'M' || img[1] != 'Z') return 0;
    const unsigned char *nt = img + rd32(img + 0x3c);
    if (memcmp(nt, "PE\0\0", 4)) return 0;
    unsigned nsec = (unsigned)nt[6] | (unsigned)nt[7] << 8, opt = (unsigned)nt[20] | (unsigned)nt[21] << 8;
    const unsigned char *sh = nt + 24 + opt;
    int n = 0;
    for (unsigned i = 0; i < nsec && n < max; i++, sh += 40) {
        unsigned vsize = rd32(sh + 8), rva = rd32(sh + 12), raw = rd32(sh + 16), ch = rd32(sh + 36);
        out[n].rva = rva;
        out[n].size = vsize ? vsize : raw;
        out[n].write = (ch & 0x80000000u) != 0;   /* IMAGE_SCN_MEM_WRITE */
        out[n].exec = (ch & 0x20000000u) != 0;    /* IMAGE_SCN_MEM_EXECUTE */
        n++;
    }
    return n;
}

unsigned rtti_vtable(const unsigned char *img, unsigned long long base, const ImgSec *s, int ns, const char *name)
{
    size_t nl = strlen(name) + 1;   /* с нулём в конце: не префикс другого имени */
    unsigned td = 0;
    int ntd = 0;
    for (int i = 0; i < ns; i++) {   /* описатель типа: {таблица type_info, запас, имя} — имя на +16 */
        if (s[i].exec || s[i].size < nl) continue;
        const unsigned char *p = img + s[i].rva, *e = p + s[i].size - nl;
        for (; p <= e; p++) {
            p = memchr(p, name[0], (size_t)(e - p) + 1);
            if (!p) break;
            if (!memcmp(p, name, nl) && (unsigned)(p - img) >= s[i].rva + 16) { td = (unsigned)(p - img) - 16; ntd++; }
        }
    }
    if (ntd != 1) return 0;
    unsigned col = 0;
    int ncol = 0;
    for (int i = 0; i < ns; i++) {   /* локатор: подпись 1, смещение 0, cd, описатель, иерархия, сам себя */
        if (s[i].exec || s[i].write) continue;
        for (unsigned o = 0; o + 24 <= s[i].size; o += 4) {
            const unsigned char *p = img + s[i].rva + o;
            if (rd32(p) == 1 && rd32(p + 4) == 0 && rd32(p + 12) == td && rd32(p + 20) == s[i].rva + o) { col = s[i].rva + o; ncol++; }
        }
    }
    if (ncol != 1) return 0;
    unsigned vt = 0;
    int nvt = 0;
    for (int i = 0; i < ns; i++) {   /* таблица: в ячейке перед ней — адрес локатора */
        if (s[i].exec || s[i].write) continue;
        for (unsigned o = 0; o + 16 <= s[i].size; o += 8)
            if (rd64(img + s[i].rva + o) == base + col) { vt = s[i].rva + o + 8; nvt++; }
    }
    return nvt == 1 ? vt : 0;
}

int img_find_qword(const unsigned char *img, const ImgSec *s, int ns, unsigned long long va, unsigned *out, int max)
{
    int n = 0;
    for (int i = 0; i < ns; i++) {
        if (!s[i].write) continue;
        for (unsigned o = 0; o + 8 <= s[i].size; o += 8)
            if (rd64(img + s[i].rva + o) == va) {
                if (n < max) out[n] = s[i].rva + o;
                n++;
            }
    }
    return n;
}

long code_find(const unsigned char *code, size_t n, const unsigned char *pat, size_t pn)
{
    if (!pn || n < pn) return -1;
    for (size_t i = 0; i + pn <= n; i++)
        if (!memcmp(code + i, pat, pn)) return (long)i;
    return -1;
}

unsigned code_rip_cmp0(const unsigned char *code, size_t n, unsigned code_rva)
{
    unsigned at = 0;
    int k = 0;
    for (size_t i = 0; i + 7 <= n; i++)
        if (code[i] == 0x80 && code[i + 1] == 0x3D && code[i + 6] == 0x00) {
            int rel = (int)rd32(code + i + 2);
            at = (unsigned)((long long)code_rva + (long long)i + 7 + rel);
            k++;
        }
    return k == 1 ? at : 0;
}
