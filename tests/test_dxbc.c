/* Г4: dxbc.c — переменная буфера констант по имени из отражения шейдера. Шейдеры собираются системным
 * d3dcompiler_47 с той же раскладкой, что у шейдера размытия игры (postprocess/dof/dof_blur.hlsl):
 *   cbuffer dofb : register(b0) { float maxCoCRadiusPixels : packoffset(c24.x); float2 textureDimensions : packoffset(c24.y); }
 * Проверки: ps_5_0 и ps_4_0 (разный размер описания переменной), другой регистр, шейдер без отражения, обрезанный байткод
 * на каждой длине — память за концом закрыта, чтение за край = падение. Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3dcommon.h>
#include "dxbc.h"

typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                     UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *PFN_STRIP)(const void *, SIZE_T, UINT, ID3DBlob **);
static PFN_COMPILE comp;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ID3DBlob *compile(const char *src, const char *prof)
{
    ID3DBlob *b = NULL, *e = NULL;
    if (FAILED(comp(src, strlen(src), "t", NULL, NULL, "main", prof, 0, 0, &b, &e))) {
        printf("compile %s: %s\n", prof, e ? (const char *)ID3D10Blob_GetBufferPointer(e) : "?");
        return NULL;
    }
    return b;
}

static const char *DOF =
    "cbuffer dofb : register(b%d) { float maxCoCRadiusPixels : packoffset(c24.x); float2 textureDimensions : packoffset(c24.y); }"
    "Texture2D t : register(t0); SamplerState s : register(s0);"
    "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target"
    "{ return t.Sample(s, uv) * maxCoCRadiusPixels + textureDimensions.xyxy; }";

static void case_ok(const char *prof, int slot)
{
    char src[1024];
    snprintf(src, sizeof src, DOF, slot);
    ID3DBlob *b = compile(src, prof);
    CHECK(b, "%s b%d: not compiled", prof, slot);
    if (!b) return;
    const void *p = ID3D10Blob_GetBufferPointer(b);
    size_t n = ID3D10Blob_GetBufferSize(b);
    DxbcVar r = {0}, w = {0}, x;
    CHECK(dxbc_find_var(p, n, "maxCoCRadiusPixels", &r), "%s: radius not found", prof);
    CHECK(r.slot == (unsigned)slot && r.offset == 384 && r.size == 4, "%s: radius slot %u offset %u size %u", prof, r.slot, r.offset, r.size);
    CHECK(dxbc_find_var(p, n, "textureDimensions", &w), "%s: size not found", prof);
    CHECK(w.slot == (unsigned)slot && w.offset == 388 && w.size == 8, "%s: size slot %u offset %u size %u", prof, w.slot, w.offset, w.size);
    CHECK(r.cb_bytes >= 396, "%s: buffer %u bytes", prof, r.cb_bytes);
    CHECK(!dxbc_find_var(p, n, "nearScale", &x), "%s: a missing name was found", prof);
    CHECK(!dxbc_find_var(p, n, "maxCoCRadiusPixel", &x), "%s: a prefix of the name matched", prof);
    printf("%s b%d: radius float[%u], width float[%u], buffer %u bytes\n", prof, slot, r.offset / 4, w.offset / 4, r.cb_bytes);

    /* обрезанный байткод: каждая длина, сразу за концом — закрытая страница */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t pages = (n + si.dwPageSize - 1) / si.dwPageSize + 1;
    BYTE *mem = VirtualAlloc(NULL, pages * si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(mem + (pages - 1) * si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &old);
    BYTE *end = mem + (pages - 1) * si.dwPageSize;
    int found_short = 0;
    for (size_t len = 0; len < n; len++) {
        memcpy(end - len, p, len);
        found_short += dxbc_find_var(end - len, len, "maxCoCRadiusPixels", &x);
    }
    /* заголовок говорит «всего n байт» — короче не верим; искажённые смещения частей */
    CHECK(!found_short, "%s: found in a truncated shader %d times", prof, found_short);
    memcpy(end - n, p, n);
    BYTE *q = end - n;
    unsigned nch;
    memcpy(&nch, q + 28, 4);
    for (unsigned i = 0; i < nch; i++) {
        unsigned save;
        memcpy(&save, q + 32 + 4 * i, 4);
        unsigned bad = (unsigned)n - 2;
        memcpy(q + 32 + 4 * i, &bad, 4);
        dxbc_find_var(q, n, "maxCoCRadiusPixels", &x);   /* не должен падать */
        memcpy(q + 32 + 4 * i, &save, 4);
    }
    CHECK(dxbc_find_var(q, n, "maxCoCRadiusPixels", &x), "%s: not found right at the end of memory", prof);
    VirtualFree(mem, 0, MEM_RELEASE);
    ID3D10Blob_Release(b);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE dc = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    comp = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    PFN_STRIP strip = dc ? (PFN_STRIP)GetProcAddress(dc, "D3DStripShader") : NULL;
    if (!comp || !strip) { printf("no d3dcompiler_47\nFAIL\n"); return 1; }
    case_ok("ps_5_0", 0);
    case_ok("ps_4_0", 0);
    case_ok("ps_5_0", 3);

    char src[1024];
    snprintf(src, sizeof src, DOF, 0);
    ID3DBlob *b = compile(src, "ps_5_0"), *s = NULL;
    if (b && SUCCEEDED(strip(ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), 1 /* STRIP_REFLECTION_DATA */, &s))) {
        DxbcVar x;
        CHECK(!dxbc_find_var(ID3D10Blob_GetBufferPointer(s), ID3D10Blob_GetBufferSize(s), "maxCoCRadiusPixels", &x),
              "found in a shader without reflection");
        printf("shader without reflection: not found\n");
    } else CHECK(0, "strip failed");
    DxbcVar x;
    CHECK(!dxbc_find_var("not a shader at all, just text....", 34, "maxCoCRadiusPixels", &x), "text taken for a shader");
    CHECK(!dxbc_find_var(NULL, 100, "maxCoCRadiusPixels", &x), "NULL");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
