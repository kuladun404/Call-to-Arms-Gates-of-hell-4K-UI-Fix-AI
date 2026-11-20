/* Э2 (план v3, по реверсу): кэши окон по происхождению.
 * Игра создаёт буфер окна с флагом Buffered (eDialog, отрисовка 0xac0eae), вид GUI2::eViewport и подпись у курсора одной
 * функцией движка с меткой «t:create» (реверс 18.11, шаги 17 и Э2). Форма буфера = форма окна: 20x20, 512x512 — любая.
 * Правило формы v2 (не квадрат и не степени двойки) такие пропускает, и под xk они рисуются «кубиками».
 * Здесь: найти эту функцию в образе exe без зашитых адресов — строка -> ссылка на неё в коде -> границы функции по
 * таблице раскрутки; при создании цели — есть ли возврат в эту функцию в стеке вызова. */
#include "origin.h"
#include <string.h>

static const IMAGE_NT_HEADERS64 *nt_of(const BYTE *img, SIZE_T size)
{
    if (!img || size < sizeof(IMAGE_DOS_HEADER)) return NULL;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || (SIZE_T)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size)
        return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return NULL;
    return nt;
}

/* секция в пределах образа: [va, va+len) */
static int sec_span(const IMAGE_SECTION_HEADER *s, SIZE_T size, DWORD *va, DWORD *len)
{
    DWORD v = s->VirtualAddress, l = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
    if (v >= size) return 0;
    if ((SIZE_T)v + l > size) l = (DWORD)(size - v);
    *va = v;
    *len = l;
    return l > 0;
}

/* функция, содержащая rva, по .pdata (двоичный поиск; запись-продолжение -> основная по UNW_FLAG_CHAININFO) */
static int func_of(const BYTE *img, SIZE_T size, const IMAGE_DATA_DIRECTORY *pd, DWORD rva, OriginRange *out)
{
    if (!pd->VirtualAddress || pd->Size < sizeof(RUNTIME_FUNCTION) || (SIZE_T)pd->VirtualAddress + pd->Size > size) return 0;
    const RUNTIME_FUNCTION *rf = (const RUNTIME_FUNCTION *)(img + pd->VirtualAddress);
    long lo = 0, hi = (long)(pd->Size / sizeof(RUNTIME_FUNCTION)) - 1;
    while (lo <= hi) {
        long m = (lo + hi) / 2;
        if (rva < rf[m].BeginAddress) hi = m - 1;
        else if (rva >= rf[m].EndAddress) lo = m + 1;
        else {
            RUNTIME_FUNCTION f = rf[m];
            for (int guard = 0; guard < 8; guard++) {
                DWORD u = f.UnwindData & ~1u;
                if ((SIZE_T)u + 4 > size) return 0;
                const BYTE *ui = img + u;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) { out->begin = f.BeginAddress; out->end = f.EndAddress; return 1; }
                SIZE_T co = (SIZE_T)u + 4 + (SIZE_T)((ui[2] + 1) & ~1) * 2;
                if (co + sizeof(RUNTIME_FUNCTION) > size) return 0;
                memcpy(&f, img + co, sizeof f);
            }
            return 0;
        }
    }
    return 0;
}

/* основная запись для записи .pdata (по цепочке UNW_FLAG_CHAININFO); 0 — битая */
static int primary_of(const BYTE *img, SIZE_T size, RUNTIME_FUNCTION f, RUNTIME_FUNCTION *out)
{
    for (int guard = 0; guard < 8; guard++) {
        DWORD u = f.UnwindData & ~1u;
        if ((SIZE_T)u + 4 > size) return 0;
        const BYTE *ui = img + u;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) { *out = f; return 1; }
        SIZE_T co = (SIZE_T)u + 4 + (SIZE_T)((ui[2] + 1) & ~1) * 2;
        if (co + sizeof(RUNTIME_FUNCTION) > size) return 0;
        memcpy(&f, img + co, sizeof f);
    }
    return 0;
}

int origin_parts(const BYTE *img, SIZE_T img_size, const OriginRange *f, OriginRange *out, int *n, int max)
{
    const IMAGE_NT_HEADERS64 *nt = nt_of(img, img_size);
    if (!nt || !f || !n) return 0;
    const IMAGE_DATA_DIRECTORY *pd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!pd->VirtualAddress || pd->Size < sizeof(RUNTIME_FUNCTION) || (SIZE_T)pd->VirtualAddress + pd->Size > img_size) return 0;
    const RUNTIME_FUNCTION *rf = (const RUNTIME_FUNCTION *)(img + pd->VirtualAddress);
    DWORD cnt = pd->Size / sizeof(RUNTIME_FUNCTION);
    int added = 0;
    for (DWORD i = 0; i < cnt && *n < max; i++) {
        RUNTIME_FUNCTION p;
        if (!rf[i].BeginAddress || !primary_of(img, img_size, rf[i], &p) || p.BeginAddress != f->begin) continue;
        int dup = 0;
        for (int k = 0; k < *n; k++) dup |= out[k].begin == rf[i].BeginAddress;
        if (dup) continue;
        out[*n].begin = rf[i].BeginAddress;
        out[*n].end = rf[i].EndAddress;
        (*n)++;
        added++;
    }
    return added;
}

int origin_find(const BYTE *img, SIZE_T img_size, const char *label, OriginRange *out, int max, DWORD *str_rva, int *nrefs)
{
    if (str_rva) *str_rva = 0;
    if (nrefs) *nrefs = 0;
    const IMAGE_NT_HEADERS64 *nt = nt_of(img, img_size);
    if (!nt || !label || !label[0] || max <= 0) return 0;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    WORD ns = nt->FileHeader.NumberOfSections;
    SIZE_T ll = strlen(label) + 1;   /* с нулём: «t:create», а не начало длинной строки */

    /* строки — в секциях данных (не исполняемых) */
    DWORD str[8];
    int nstr = 0;
    for (WORD i = 0; i < ns && nstr < 8; i++) {
        DWORD va, len;
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sec_span(&sec[i], img_size, &va, &len) || len < ll) continue;
        const BYTE *p = img + va, *end = img + va + len - ll;
        while (p <= end && nstr < 8) {
            const BYTE *q = memchr(p, (unsigned char)label[0], (SIZE_T)(end - p) + 1);
            if (!q) break;
            if (!memcmp(q, label, ll) && (q == img + va || q[-1] == 0)) str[nstr++] = (DWORD)(q - img);
            p = q + 1;
        }
    }
    if (!nstr) return 0;
    if (str_rva) *str_rva = str[0];

    /* ссылки: REX.W (48 / 4C) + LEA или MOV (8D / 8B) + modrm «[rip + disp32]» (mod 00, r/m 101) */
    const IMAGE_DATA_DIRECTORY *pd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    int n = 0, refs = 0;
    for (WORD i = 0; i < ns; i++) {
        DWORD va, len;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sec_span(&sec[i], img_size, &va, &len) || len < 8) continue;
        const BYTE *b = img + va;
        for (DWORD o = 3; o + 4 <= len; o++) {
            if ((b[o - 1] & 0xC7) != 0x05 || (b[o - 2] != 0x8D && b[o - 2] != 0x8B) || (b[o - 3] != 0x48 && b[o - 3] != 0x4C))
                continue;
            INT32 disp;
            memcpy(&disp, b + o, 4);
            long long t = (long long)va + o + 4 + disp;
            int hit = 0;
            for (int s = 0; s < nstr; s++) hit |= t == (long long)str[s];
            if (!hit) continue;
            refs++;
            OriginRange f;
            if (!func_of(img, img_size, pd, va + o - 3, &f)) continue;
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= out[k].begin == f.begin;
            if (!dup && n < max) out[n++] = f;
        }
    }
    if (nrefs) *nrefs = refs;
    return n;
}

static int in_ranges(void *p, const BYTE *img, const OriginRange *r, int n)
{
    UINT_PTR a = (UINT_PTR)p;
    if (a <= (UINT_PTR)img) return 0;
    UINT_PTR rva = a - (UINT_PTR)img;
    for (int k = 0; k < n; k++)
        if (rva > r[k].begin && rva <= r[k].end) return 1;   /* возврат — сразу после call, может быть и на конце */
    return 0;
}

int origin_chain(void *const *fr, int n, const BYTE *img, const OriginRange *a, int na, const OriginRange *b, int nb, int within)
{
    int hit = 0;
    for (int i = 0; i < n; i++) {
        if (!in_ranges(fr[i], img, a, na)) continue;
        hit = 1;
        for (int j = i + 1; j < n && j <= i + within; j++)
            if (nb && in_ranges(fr[j], img, b, nb)) return 2;
    }
    return hit;
}

int origin_stack(const BYTE *img, const OriginRange *a, int na, const OriginRange *b, int nb, int depth, int within)
{
    void *fr[48];
    if (depth > 48) depth = 48;
    USHORT got = RtlCaptureStackBackTrace(1, (DWORD)depth, fr, NULL);
    return origin_chain(fr, got, img, a, na, b, nb, within);
}

int origin_on_stack(const BYTE *img, const OriginRange *r, int n, int depth)
{
    void *fr[48];
    if (depth > 48) depth = 48;
    USHORT got = RtlCaptureStackBackTrace(1, (DWORD)depth, fr, NULL);
    for (USHORT i = 0; i < got; i++) {
        UINT_PTR a = (UINT_PTR)fr[i];
        if (a <= (UINT_PTR)img) continue;
        UINT_PTR rva = a - (UINT_PTR)img;
        for (int k = 0; k < n; k++)
            if (rva > r[k].begin && rva <= r[k].end) return 1;   /* возврат — сразу после call, может быть и на конце */
    }
    return 0;
}
