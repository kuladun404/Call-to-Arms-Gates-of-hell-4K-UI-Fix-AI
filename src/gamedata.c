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

/* Э3 (реверс 18.11): постобработка кладёт в буфер сцены свои числа по своей раскладке, и один регистр в разных проходах
 * значит разное (c14 — размер текстуры яркости и сила лучей линзы). Таблица — имена переменных с размером экрана или
 * его частью; место — из packoffset в исходнике игры. Правка всё равно сверяет значение (cbfix.c). */
static const struct { const char *file, *var; int add, exact; } SLOT_VARS[] = {
    {"shader/dx10/postprocess/gauss_blur.hlsl", "halfPixel", 0, 0},                      /* c0.xy: полпикселя размываемой */
    {"shader/dx10/postprocess/copy.hlsl", "src_size", 0, 0},                             /* c1.xy */
    {"shader/dx10/postprocess/copy.hlsl", "one_pixel", 0, 0},                            /* c1.zw */
    {"shader/dx10/postprocess/lenseffects/heat_mask.hlsl", "textureDim", 0, 0},
    {"shader/dx10/postprocess/luma.hlsl", "textureDimension", 0, 0},                     /* pscLEParams.xy */
    {"shader/dx10/postprocess/lenseffects/lens_effects.hlsl", "textureDims_LR", 0, 0},  /* pscLELightPos.zw */
    {"shader/dx10/postprocess/lenseffects/lens_effects.hlsl", "fxaaNormPixelWidthHeight", 0, 0},   /* pscFXAAParams.zw */
    {"shader/dx10/postprocess/lenseffects_v2/lens_effects.hlsl", "textureDims_LR", 0, 0},
    {"shader/dx10/postprocess/dof/dof_blur.hlsl", "textureDimensions", 0, 0},           /* pscDOFParams.yz */
    {"shader/dx10/sky.hlsl", "sunMoonPosRes", 2, 0},                                     /* .zw — разрешение устройства */
    /* экранные проходы ставят «камеру» в центр экрана (960, 540, 2000) — то же место, что cameraOrigin мировых;
     * v2 правила его поиском по значению, проверено в игре — оставляем, значение сверяется.
     * 20.11: только точное совпадение. В мировых проходах здесь настоящая позиция камеры (тысячи единиц, -4474, 4477);
     * допуск ±1 пиксель половинной/четвертной картинки (±2 у 960x540, ±4 у 480x270) принимал камеру в этих точках карты
     * за центр экрана, и освещение кадра портилось. Центр экрана игра ставит точно — половина логического экрана */
    {"shader/dx10/postprocess/ssao/SSAO.hlsl", "cameraOrigin", 0, 1},
};

static int is_id(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* «#define name _PSC(pscgScene, N)» в shader_constants.h -> N; -1 — нет */
static int scene_reg(const Pak *p, const char *name, size_t nl)
{
    const PakEnt *e = pak_find(p, "shader/dx10/shader_constants.h");
    if (!e) return -1;
    const char *t = (const char *)p->base + e->off, *end = t + e->size;
    for (const char *s = t; s + 8 + nl < end; s++) {
        if (memcmp(s, "#define", 7) || (s[7] != ' ' && s[7] != '\t')) continue;
        const char *c = s + 7;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if ((size_t)(end - c) <= nl || memcmp(c, name, nl) || is_id(c[nl])) continue;
        c += nl;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        static const char psc[] = "_PSC(pscgScene,";
        if ((size_t)(end - c) < sizeof psc || memcmp(c, psc, sizeof psc - 1)) return -1;
        c += sizeof psc - 1;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        int v = 0, d = 0;
        while (c < end && *c >= '0' && *c <= '9' && v < 10000) { v = v * 10 + (*c++ - '0'); d++; }
        return d ? v : -1;
    }
    return -1;
}

/* «var : packoffset(REG[.c])» в тексте -> номер числа; -1 — нет */
static int var_slot(const Pak *p, const char *t, const char *end, const char *var)
{
    size_t vl = strlen(var);
    for (const char *s = t; s + vl < end; s++) {
        if (memcmp(s, var, vl) || (s > t && is_id(s[-1])) || is_id(s[vl])) continue;
        const char *c = s + vl;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c >= end || *c != ':') continue;
        c++;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        static const char po[] = "packoffset";
        if ((size_t)(end - c) < sizeof po || memcmp(c, po, sizeof po - 1)) continue;
        c += sizeof po - 1;
        while (c < end && (*c == ' ' || *c == '\t' || *c == '(')) c++;
        const char *r = c;
        while (c < end && is_id(*c)) c++;
        size_t rl = (size_t)(c - r);
        if (!rl) return -1;
        int reg = -1;
        if (r[0] == 'c' && rl > 1 && r[1] >= '0' && r[1] <= '9') {
            reg = 0;
            for (size_t i = 1; i < rl && r[i] >= '0' && r[i] <= '9'; i++) reg = reg * 10 + (r[i] - '0');
        } else {
            char nm[64];
            if (rl >= sizeof nm) return -1;
            memcpy(nm, r, rl);
            nm[rl] = 0;
            reg = scene_reg(p, nm, rl);
        }
        if (reg < 0) return -1;
        int comp = 0;
        if (c < end && *c == '.' && c + 1 < end) {
            const char *xyzw = "xyzw";
            for (int k = 0; k < 4; k++) if (c[1] == xyzw[k]) comp = k;
        }
        return reg * 4 + comp;
    }
    return -1;
}

int pak_screen_slots(const Pak *p, unsigned *out, int max, int *names, unsigned char *exact)
{
    int n = 0, found = 0;
    for (size_t i = 0; i < sizeof SLOT_VARS / sizeof SLOT_VARS[0]; i++) {
        const PakEnt *e = pak_find(p, SLOT_VARS[i].file);
        if (!e) continue;
        const char *t = (const char *)p->base + e->off;
        int v = var_slot(p, t, t + e->size, SLOT_VARS[i].var);
        if (v < 0) continue;
        found++;
        unsigned at = (unsigned)(v + SLOT_VARS[i].add);
        int dup = -1, k;
        for (k = 0; k < n; k++) if (out[k] == at) dup = k;
        if (dup >= 0) {   /* то же место у другой переменной: допуск остаётся, если его просит хоть одна */
            if (exact && !SLOT_VARS[i].exact) exact[dup] = 0;
            continue;
        }
        if (n >= max) continue;
        for (k = n; k > 0 && out[k - 1] > at; k--) {   /* по возрастанию */
            out[k] = out[k - 1];
            if (exact) exact[k] = exact[k - 1];
        }
        out[k] = at;
        if (exact) exact[k] = (unsigned char)(SLOT_VARS[i].exact != 0);
        n++;
    }
    if (names) *names = found;
    return n;
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
