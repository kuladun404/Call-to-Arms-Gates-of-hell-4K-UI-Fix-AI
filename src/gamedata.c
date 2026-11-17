/* Архив шейдеров игры: см. gamedata.h. Файл отображается в память целиком (51 МБ адресов, читаются только нужные
 * страницы) и остаётся отображённым до конца работы — указатели на исходники и байткод действуют всегда. */
#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#include "gamedata.h"

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

int pak_parse(const unsigned char *b, size_t len, Pak *p)
{
    ZeroMemory(p, sizeof *p);
    if (!b || len < 22) return 0;
    size_t eocd = 0;
    for (size_t i = len - 22 + 1; i-- > 0 && len - i <= 22 + 65535;)
        if (rd32(b + i) == 0x06054b50) { eocd = i + 1; break; }
    if (!eocd--) return 0;
    unsigned cnt = rd16(b + eocd + 10);
    size_t cd = rd32(b + eocd + 16), cdsize = rd32(b + eocd + 12);
    if (cd > len || cdsize > len - cd) return 0;
    PakEnt *e = (PakEnt *)HeapAlloc(GetProcessHeap(), 0, (cnt ? cnt : 1) * sizeof *e);
    if (!e) return 0;
    int n = 0;
    size_t q = cd;
    for (unsigned i = 0; i < cnt; i++) {
        if (q + 46 > len || rd32(b + q) != 0x02014b50) break;
        unsigned method = rd16(b + q + 10), nl = rd16(b + q + 28), xl = rd16(b + q + 30), cl = rd16(b + q + 32);
        size_t csz = rd32(b + q + 20), lo = rd32(b + q + 42);
        if (q + 46 + nl > len) break;
        const char *nm = (const char *)b + q + 46;
        q += 46 + (size_t)nl + xl + cl;
        if (method != 0 || lo + 30 > len || rd32(b + lo) != 0x04034b50) continue;   /* только без сжатия */
        size_t data = lo + 30 + rd16(b + lo + 26) + rd16(b + lo + 28);
        if (data > len || csz > len - data) continue;
        e[n].name = nm; e[n].nlen = nl; e[n].off = data; e[n].size = csz;
        n++;
    }
    p->base = b; p->size = len; p->e = e; p->n = n;
    return n > 0;
}

void pak_free(Pak *p)
{
    if (p->e) HeapFree(GetProcessHeap(), 0, p->e);
    ZeroMemory(p, sizeof *p);
}

const PakEnt *pak_find(const Pak *p, const char *path)
{
    size_t l = strlen(path);
    for (int i = 0; p && i < p->n; i++)
        if (p->e[i].nlen == l && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, p->e[i].name, (int)l, path, (int)l) == CSTR_EQUAL)
            return &p->e[i];
    return NULL;
}

static int md5(const void *d, ULONG n, BYTE out[16])
{
    BCRYPT_ALG_HANDLE a = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_MD5_ALGORITHM, NULL, 0) != 0) return 0;
    int ok = BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) == 0 && BCryptHashData(h, (PUCHAR)d, n, 0) == 0 &&
             BCryptFinishHash(h, out, 16, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

const unsigned char *pak_shader(const Pak *p, const char *name, size_t *len)
{
    char low[512];
    size_t l = strlen(name);
    if (!p || !l || l >= sizeof low || l > 255) return NULL;
    for (size_t i = 0; i <= l; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    BYTE h[16];
    if (!md5(low, (ULONG)l, h)) return NULL;
    char hex[34];
    for (int i = 0; i < 16; i++) { static const char d[] = "0123456789abcdef"; hex[2 * i] = d[h[i] >> 4]; hex[2 * i + 1] = d[h[i] & 15]; }
    hex[32] = '.'; hex[33] = 0;
    /* «.../<md5>.?so» в любой папке кэша: каталог (dx11.0) и расширение (vso, pso, cso...) не важны */
    for (int i = 0; i < p->n; i++) {
        const PakEnt *e = &p->e[i];
        if (e->nlen < 37) continue;
        const char *s = e->name + e->nlen - 37;   /* "/" + 32 + "." + 3 */
        if (s[0] != '/' || memcmp(s + 1, hex, 33) != 0) continue;
        const unsigned char *d = p->base + e->off;
        /* внутри — то же имя: защита от чужого файла с похожим именем */
        if (e->size < 1 + l + 32 || d[0] != l || CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, (const char *)d + 1, (int)l, name,
                                                               (int)l) != CSTR_EQUAL)
            continue;
        const unsigned char *code = d + 1 + l;
        size_t n = e->size - 1 - l;
        if (n < 32 || memcmp(code, "DXBC", 4) || rd32(code + 24) > n) continue;
        if (len) *len = rd32(code + 24);
        return code;
    }
    return NULL;
}

int pak_cs_name(const Pak *p, const void *code, size_t n, char *name, size_t cap)
{
    if (!p || !code || n < 32) return 0;
    for (int i = 0; i < p->n; i++) {
        const PakEnt *e = &p->e[i];
        if (e->nlen < 5 || CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, e->name + e->nlen - 4, 4, ".cso", 4) != CSTR_EQUAL)
            continue;
        const unsigned char *d = p->base + e->off;
        if (e->size < 1 || e->size < 1 + (size_t)d[0] + n) continue;
        const unsigned char *c = d + 1 + d[0];
        if (rd32(c + 24) != n || memcmp(c, code, 32) || memcmp(c, code, n)) continue;   /* размер, отпечаток, целиком */
        if ((size_t)d[0] + 1 > cap) return 0;
        memcpy(name, d + 1, d[0]);
        name[d[0]] = 0;
        return 1;
    }
    return 0;
}

int pak_scene_groups(const Pak *p)
{
    const PakEnt *e = pak_find(p, "shader/dx10/shader_constants.h");
    if (!e) return 0;
    const char *t = (const char *)p->base + e->off, *end = t + e->size;
    static const char key[] = "#define pscSceneGroup";
    for (const char *s = t; s + sizeof key < end; s++) {
        if (memcmp(s, key, sizeof key - 1) || (s[sizeof key - 1] != ' ' && s[sizeof key - 1] != '\t')) continue;
        const char *c = s + sizeof key - 1;
        while (c < end && *c != '\n' && *c != ',') c++;   /* _PSG(pscgScene, 39) — число после запятой */
        if (c >= end || *c != ',') return 0;
        c++;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        int v = 0;
        while (c < end && *c >= '0' && *c <= '9' && v < 100000) v = v * 10 + (*c++ - '0');
        return v;
    }
    return 0;
}
