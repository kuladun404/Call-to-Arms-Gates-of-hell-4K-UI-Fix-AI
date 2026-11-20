/* Э2: поиск функции по метке (origin.c) — чистые функции.
 *   test_origin_find.exe [путь к call_to_arms.exe]
 * 1) искусственный образ: строка, ложная строка (хвост другой), LEA и MOV по RIP, функция из двух частей (цепочка .pdata);
 * 2) стек: проверка видит функцию, только пока она в стеке;
 * 3) настоящий exe игры (если дан): «t:create» находится, ссылка одна или больше, время поиска.
 * Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <string.h>
#include "origin.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static BYTE img[0x6000];

static void put32(DWORD at, DWORD v) { memcpy(img + at, &v, 4); }
static void rip_ref(DWORD at, BYTE op, DWORD target)   /* REX.W op modrm(rip) disp32 */
{
    img[at] = 0x48;
    img[at + 1] = op;
    img[at + 2] = 0x0D;   /* rcx, [rip+disp] */
    put32(at + 3, target - (at + 7));
}

static void build_image(void)
{
    memset(img, 0xCC, sizeof img);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)img;
    memset(dos, 0, sizeof *dos);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(img + 0x80);
    memset(nt, 0, sizeof *nt);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 3;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = sizeof img;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    memset(s, 0, 3 * sizeof *s);
    memcpy(s[0].Name, ".text", 5);  s[0].VirtualAddress = 0x1000; s[0].Misc.VirtualSize = 0x1000;
    s[0].Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE;
    memcpy(s[1].Name, ".rdata", 6); s[1].VirtualAddress = 0x3000; s[1].Misc.VirtualSize = 0x800;
    s[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    memcpy(s[2].Name, ".pdata", 6); s[2].VirtualAddress = 0x4000; s[2].Misc.VirtualSize = 0x100;
    s[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    memset(img + 0x3000, 0, 0x800);
    memcpy(img + 0x3008, "xt:create", 10);   /* ложная: «t:create» — хвост другой строки */
    memcpy(img + 0x3020, "t:create", 9);     /* настоящая */
    memcpy(img + 0x3040, "t:created", 10);   /* длиннее — не она */
    img[0x3400] = 0x01;                       /* UNWIND_INFO: версия 1, без флагов, 0 кодов */
    img[0x3401] = img[0x3402] = img[0x3403] = 0;
    img[0x3410] = 0x01 | (UNW_FLAG_CHAININFO << 3);   /* часть функции: цепочка к основной записи */
    img[0x3411] = img[0x3412] = img[0x3413] = 0;
    RUNTIME_FUNCTION prim = {0x1200, 0x1240, 0x3400};
    memcpy(img + 0x3414, &prim, sizeof prim);

    rip_ref(0x1120, 0x8D, 0x3020);   /* A: LEA на строку */
    rip_ref(0x1310, 0x8B, 0x3020);   /* часть B: MOV на строку */
    rip_ref(0x1410, 0x8D, 0x3009);   /* C: LEA на ложную */
    rip_ref(0x1420, 0x8D, 0x3040);   /* C: LEA на длинную */
    RUNTIME_FUNCTION rf[4] = {{0x1100, 0x1180, 0x3400}, {0x1200, 0x1240, 0x3400}, {0x1300, 0x1340, 0x3410},
                              {0x1400, 0x1440, 0x3400}};
    memset(img + 0x4000, 0, 0x100);
    memcpy(img + 0x4000, rf, sizeof rf);
    nt->OptionalHeader.NumberOfRvaAndSizes = 16;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x4000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = sizeof rf;
}

/* 19.11: цепочка возвратов — «t:create» (a), над ней не дальше within — функция воды (b) */
static int chain_tests(void)
{
    static BYTE img2[0x100];
    const BYTE *b0 = img2;
    OriginRange a = {0x10, 0x20}, w = {0x40, 0x50}, x = {0x80, 0x90};   /* x — чужая функция, не в стеке */
    void *f1[] = {(void *)(b0 + 0x08), (void *)(b0 + 0x15), (void *)(b0 + 0x65), (void *)(b0 + 0x45)};   /* hook, t:create, creator, water */
    void *f2[] = {(void *)(b0 + 0x15), (void *)(b0 + 0x65), (void *)(b0 + 0x66), (void *)(b0 + 0x67), (void *)(b0 + 0x45)};
    void *f3[] = {(void *)(b0 + 0x45), (void *)(b0 + 0x15)};   /* вода ниже «t:create» — не она позвала */
    void *f4[] = {(void *)(b0 + 0x65), (void *)(b0 + 0x66)};
    int bad = 0;
    bad += origin_chain(f1, 4, b0, &a, 1, &w, 1, 3) != 2;
    bad += origin_chain(f2, 5, b0, &a, 1, &w, 1, 3) != 1;   /* дальше 3 возвратов */
    bad += origin_chain(f3, 2, b0, &a, 1, &w, 1, 3) != 1;
    bad += origin_chain(f4, 2, b0, &a, 1, &w, 1, 3) != 0;
    bad += origin_chain(f1, 4, b0, &a, 1, NULL, 0, 3) != 1;  /* функции воды нет — просто «t:create» */
    bad += origin_chain(f1, 4, b0, &a, 1, &x, 1, 3) != 1;
    return bad;
}

/* стек: inner смотрит, есть ли в стеке probe */
static OriginRange g_probe;
static const BYTE *g_base;
__declspec(noinline) static int inner(void) { return origin_on_stack(g_base, &g_probe, 1, 16); }
__declspec(noinline) static int probe(void) { volatile int r = inner(); return r; }

/* цена проверки: стек глубиной 30 кадров, функции в нём нет — смотрим все 24 возврата (худший случай) */
static double g_us;
__declspec(noinline) static int deep(int k)
{
    if (k > 0) { volatile int r = deep(k - 1); return r + 1; }
    OriginRange none = {1, 2};
    LARGE_INTEGER f, a, z;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    int hit = 0;
    for (int i = 0; i < 2000; i++) hit += origin_on_stack(g_base, &none, 1, 24);
    QueryPerformanceCounter(&z);
    g_us = (double)(z.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / 2000;
    return hit;
}

int main(int argc, char **argv)
{
    {
        int bad = chain_tests();
        printf("origin_chain: %s\n", bad ? "FAIL" : "ok");
        if (bad) { printf("FAIL line %d: origin_chain %d wrong\n", __LINE__, bad); fails++; }
    }
    build_image();
    OriginRange r[ORIGIN_MAX];
    DWORD str = 0;
    int refs = 0;
    int n = origin_find(img, sizeof img, "t:create", r, ORIGIN_MAX, &str, &refs);
    CHECK(n == 2, "two functions refer to the label: %d", n);
    CHECK(str == 0x3020, "the label itself, not the tail of another string: 0x%lx", str);
    CHECK(refs == 2, "two references (LEA and MOV), the false strings are ignored: %d", refs);
    CHECK(n >= 1 && r[0].begin == 0x1100 && r[0].end == 0x1180, "function A 0x%lx..0x%lx", n ? r[0].begin : 0, n ? r[0].end : 0);
    CHECK(n >= 2 && r[1].begin == 0x1200 && r[1].end == 0x1240, "part of B leads to its main entry: 0x%lx..0x%lx",
          n > 1 ? r[1].begin : 0, n > 1 ? r[1].end : 0);
    {   /* 19.11: все части функции B — основная и продолжение (её тело с вызовами) */
        OriginRange parts[8];
        int np = 0, add = n >= 2 ? origin_parts(img, sizeof img, &r[1], parts, &np, 8) : 0;
        int has1300 = 0;
        for (int i = 0; i < np; i++) has1300 |= parts[i].begin == 0x1300 && parts[i].end == 0x1340;
        CHECK(add == 2 && np == 2 && has1300, "parts of B: %d (0x1300..0x1340 among them: %d)", np, has1300);
        np = 0;
        CHECK(origin_parts(img, sizeof img, &r[0], parts, &np, 8) == 1 && np == 1, "A has one part: %d", np);
    }
    CHECK(origin_find(img, sizeof img, "t:destroy", r, ORIGIN_MAX, &str, &refs) == 0 && str == 0, "no such label");
    CHECK(origin_find(img, 0x40, "t:create", r, ORIGIN_MAX, &str, &refs) == 0, "too small to be an image");
    img[0] = 'X';
    CHECK(origin_find(img, sizeof img, "t:create", r, ORIGIN_MAX, &str, &refs) == 0, "not an image");
    img[0] = 'M';

    /* стек */
    g_base = (const BYTE *)GetModuleHandleW(NULL);
    DWORD64 base;
    PRUNTIME_FUNCTION pf = RtlLookupFunctionEntry((DWORD64)(UINT_PTR)probe, &base, NULL);
    CHECK(pf != NULL, "probe() has an unwind entry");
    if (pf) {
        g_probe.begin = pf->BeginAddress;
        g_probe.end = pf->EndAddress;
        CHECK(probe() == 1, "probe() is on the stack while it calls inner()");
        CHECK(inner() == 0, "probe() is not on the stack when main() calls inner()");
    }
    CHECK(deep(30) == 30, "no false hit in a deep stack");
    printf("stack check, worst case (24 frames, no hit): %.2f us per texture\n", g_us);
    CHECK(g_us <= 5.0, "stack check costs %.2f us, plan limit 5 us per created texture", g_us);

    /* настоящий exe игры */
    if (argc > 1) {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
        HMODULE h = LoadLibraryExW(w, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(h != NULL, "game exe mapped as an image: %lu", GetLastError());
        if (h) {
            const BYTE *b = (const BYTE *)((UINT_PTR)h & ~(UINT_PTR)3);
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
            LARGE_INTEGER f, a, z;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            n = origin_find(b, nt->OptionalHeader.SizeOfImage, "t:create", r, ORIGIN_MAX, &str, &refs);
            QueryPerformanceCounter(&z);
            double ms = (double)(z.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
            printf("game exe: \"t:create\" at 0x%lx, %d reference(s), %d function(s), first 0x%lx..0x%lx, %.1f ms\n", str, refs, n,
                   n ? r[0].begin : 0, n ? r[0].end : 0, ms);
            CHECK(n >= 1 && refs >= 1, "the label and the function that refers to it are found in the game exe");
            CHECK(ms < 200, "search time %.1f ms (once at start)", ms);
            FreeLibrary(h);
        }
    } else {
        printf("game exe not given - only the artificial image\n");
    }
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
