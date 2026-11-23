/* Автоэкспозиция при картинке мира другого размера (F14, масштаб интерфейса).
 * Игра считает яркость кадра вычислительными шейдерами: поток (x,y) читает пиксель (x,y) картинки сцены scene_Buffer,
 * сетка — по логическому размеру. Картинка мира у нас больше/меньше — читался бы только угол.
 *
 * Устойчивость к обновлениям игры: любой вычислительный шейдер игры, который читает scene_Buffer, мод находит в кэше
 * шейдеров игры по байткоду (gamedata.h), берёт его ТЕКУЩИЙ исходник из того же архива и меняет в нём одно — каждое
 * чтение scene_Buffer[p] берёт пиксель p, растянутый на всю картинку (ui4k_px). Во сколько раз картинка больше сетки,
 * мод кладёт в свой буфер констант b13 перед каждым запуском такого шейдера. Собирает системный d3dcompiler_47.
 * Изменит игра расчёт яркости — мод соберёт уже новый расчёт.
 * Не вышло (архива нет, исходник не нашёлся или не собрался) — прежние копии двух шейдеров 16.11 (lum_*.hlsl), если
 * шейдер игры тот же; иначе — шейдер игры как есть и строка в логе. */
#include <stdio.h>
#include <string.h>
#include "common.h"
#include "lum_cs.h"
#include "dxbc.h"
#include "gamedata.h"

#define LUM_CB 13   /* свой буфер: float4 ui4k_k (во сколько раз картинка больше сетки по x, y) */

static VtHook h_ccs;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSH)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, void **);
typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, const char *,
                                     const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

/* {8b4f2d61-7a3e-4c19-b5d2-0e6f9a1c3b77}: наш пересобранный шейдер; данные — регистр scene_Buffer */
static const GUID GUID_LUM = {0x8b4f2d61, 0x7a3e, 0x4c19, {0xb5, 0xd2, 0x0e, 0x6f, 0x9a, 0x1c, 0x3b, 0x77}};
#define LUM_PS 16
static void *volatile g_cs[LUM_PS];   /* быстрый отсев в CSSetShader; правду говорит метка */
static volatile LONG g_ncs;

/* ---- правка исходника (чистая функция, тест — test_update) ---- */
static const char HELPER[] =
    "\ncbuffer ui4k_map : register(b13) { float4 ui4k_k; };\n"
    "uint2 ui4k_px(uint2 p) { float2 k = ui4k_k.x > 0 ? ui4k_k.xy : float2(1, 1); uint w, h; scene_Buffer.GetDimensions(w, h);"
    " return min((uint2)(((float2)p + 0.5) * k), uint2(w - 1, h - 1)); }\n";

static int ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* после объявления scene_Buffer — помощник; каждое scene_Buffer[x] -> scene_Buffer[ui4k_px(x)]. Число правок; 0 — не вышло */
int lum_patch(const char *s, size_t n, char **out, size_t *on)
{
    static const char T[] = "scene_Buffer";
    const size_t tl = sizeof T - 1;
    size_t decl = (size_t)-1;
    for (size_t i = 0; i + tl < n; i++) {   /* объявление: «scene_Buffer :» или «scene_Buffer;» */
        if (memcmp(s + i, T, tl) || (i && ident(s[i - 1])) || ident(s[i + tl])) continue;
        size_t j = i + tl;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && (s[j] == ':' || s[j] == ';')) {
            while (j < n && s[j] != ';') j++;
            if (j < n) decl = j + 1;
            break;
        }
    }
    if (decl == (size_t)-1) return 0;
    char *b = (char *)HeapAlloc(GetProcessHeap(), 0, n * 2 + sizeof HELPER + 64);
    if (!b) return 0;
    size_t o = 0;
    memcpy(b, s, decl); o = decl;
    memcpy(b + o, HELPER, sizeof HELPER - 1); o += sizeof HELPER - 1;
    int k = 0;
    for (size_t i = decl; i < n;) {
        if (i + tl < n && !memcmp(s + i, T, tl) && !ident(s[i - 1]) && s[i + tl] == '[') {
            size_t j = i + tl + 1, d = 1;
            while (j < n && d) { if (s[j] == '[') d++; else if (s[j] == ']') d--; if (d) j++; }
            if (j >= n) break;   /* скобка не закрыта — исходник не тот */
            memcpy(b + o, "scene_Buffer[ui4k_px(", 21); o += 21;
            memcpy(b + o, s + i + tl + 1, j - (i + tl + 1)); o += j - (i + tl + 1);
            memcpy(b + o, ")]", 2); o += 2;
            i = j + 1;
            k++;
            continue;
        }
        b[o++] = s[i++];
    }
    if (!k) { HeapFree(GetProcessHeap(), 0, b); return 0; }
    *out = b;
    *on = o;
    return k;
}

/* ---- сборка из исходника игры ---- */
typedef struct { ID3DInclude base; const Pak *p; char dir[MAX_PATH]; } Inc;

static void join(char *out, size_t cap, const char *dir, const char *f)
{
    char t[MAX_PATH];
    snprintf(t, sizeof t, "%s%s", dir, f);
    for (char *q = t; *q; q++) if (*q == '\\') *q = '/';
    /* «папка/../файл» -> «файл» */
    char *r;
    while ((r = strstr(t, "/../"))) {
        char *a = r;
        while (a > t && a[-1] != '/') a--;
        memmove(a, r + 4, strlen(r + 4) + 1);
    }
    lstrcpynA(out, t, (int)cap);
}

static HRESULT STDMETHODCALLTYPE inc_open(ID3DInclude *self, D3D_INCLUDE_TYPE t, LPCSTR file, LPCVOID parent, LPCVOID *data, UINT *bytes)
{
    (void)t;
    Inc *in = (Inc *)self;
    char dir[MAX_PATH], path[MAX_PATH];
    lstrcpynA(dir, in->dir, MAX_PATH);
    for (int i = 0; parent && i < in->p->n; i++)   /* папка того, кто включает */
        if (in->p->base + in->p->e[i].off == (const unsigned char *)parent) {
            int l = (int)in->p->e[i].nlen;
            while (l > 0 && in->p->e[i].name[l - 1] != '/') l--;
            lstrcpynA(dir, in->p->e[i].name, l + 1);
            break;
        }
    join(path, sizeof path, dir, file);
    const PakEnt *e = pak_find(in->p, path);
    if (!e) { join(path, sizeof path, "shader/dx10/", file); e = pak_find(in->p, path); }
    if (!e) return E_FAIL;
    *data = in->p->base + e->off;
    *bytes = (UINT)e->size;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE inc_close(ID3DInclude *self, LPCVOID data) { (void)self; (void)data; return S_OK; }
static ID3DIncludeVtbl g_incvt = {inc_open, inc_close};

/* профиль байткода игры (cs_5_0...) — по токену версии в SHEX/SHDR */
static int profile_of(const BYTE *code, SIZE_T n, char *out)
{
    if (!code || n < 32) return 0;
    unsigned nch = *(const unsigned *)(code + 28);
    for (unsigned i = 0; i < nch && 32 + 4 * (SIZE_T)i + 4 <= n; i++) {
        unsigned off = *(const unsigned *)(code + 32 + 4 * i);
        if (off > n || n - off < 12) return 0;   /* ревизия кода 4, п. 6: без переполнения off + 12 */
        if (!memcmp(code + off, "SHEX", 4) || !memcmp(code + off, "SHDR", 4)) {
            unsigned v = *(const unsigned *)(code + off + 8);
            if ((v >> 16) != 5) return 0;   /* 5 — вычислительный */
            sprintf(out, "cs_%u_%u", (v >> 4) & 15, v & 15);
            return 1;
        }
    }
    return 0;
}

static PFN_COMPILE compiler(void)
{
    static PFN_COMPILE c;
    if (!c) {
        HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll");
        if (!m) m = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        c = m ? (PFN_COMPILE)GetProcAddress(m, "D3DCompile") : NULL;
    }
    return c;
}

/* имя кэша «файл[функция]$настройки» -> пересобранный байткод; NULL — не вышло (причина — в why) */
static ID3DBlob *rebuild(const char *name, const BYTE *game, SIZE_T gn, const char **why)
{
    const Pak *p = gd_pak();
    PFN_COMPILE comp = compiler();
    char file[256], func[128], prof[16];
    const char *lb = strchr(name, '['), *rb = lb ? strchr(lb, ']') : NULL;
    *why = "";
    if (!p) { *why = "no game shader archive"; return NULL; }
    if (!comp) { *why = "no d3dcompiler_47"; return NULL; }
    if (!lb || !rb || lb - name >= 200 || rb - lb - 1 >= 120) { *why = "unexpected shader name"; return NULL; }
    if (!profile_of(game, gn, prof)) { *why = "not a compute shader"; return NULL; }
    snprintf(file, sizeof file, "shader/dx10/%.*s.hlsl", (int)(lb - name), name);
    snprintf(func, sizeof func, "%.*s", (int)(rb - lb - 1), lb + 1);
    D3D_SHADER_MACRO mac[34];
    char defs[512];
    int nm = 0;
    mac[nm].Name = "COMPUTE_SHADER"; mac[nm].Definition = "1"; nm++;   /* движок задаёт стадию: «#ifdef COMPUTE_SHADER» */
    lstrcpynA(defs, rb + 1, sizeof defs);
    for (char *q = defs; *q == '$' && nm < 33;) {   /* «$A$B#2» -> A, B=2 */
        *q++ = 0;
        mac[nm].Name = q;
        mac[nm].Definition = "1";
        while (*q && *q != '$') { if (*q == '#') { *q = 0; mac[nm].Definition = q + 1; } q++; }
        nm++;
    }
    mac[nm].Name = NULL; mac[nm].Definition = NULL;
    const PakEnt *e = pak_find(p, file);
    if (!e) { *why = "source not in the archive"; return NULL; }
    char *src;
    size_t sn;
    if (!lum_patch((const char *)p->base + e->off, e->size, &src, &sn)) { *why = "no scene_Buffer[...] read in the source"; return NULL; }
    Inc in;
    in.base.lpVtbl = &g_incvt;
    in.p = p;
    int dl = (int)strlen(file);
    while (dl > 0 && file[dl - 1] != '/') dl--;
    lstrcpynA(in.dir, file, dl + 1);
    ID3DBlob *b = NULL, *err = NULL;
    HRESULT hr = comp(src, sn, file, mac, &in.base, func, prof, (1 << 15) /* OPTIMIZATION_LEVEL3 */, 0, &b, &err);
    HeapFree(GetProcessHeap(), 0, src);
    if (FAILED(hr)) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 2)
            LOG("exposure: rebuilding %s failed: %.300s", name, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        *why = "compile error";
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return b;
}

/* ---- создание шейдера ---- */
static const struct { const char *name; SIZE_T len; const char *res; const BYTE *code; SIZE_T clen; } g_known[] = {
    {"tile", 2020, "histogramMetaDataBed_Buffer", lum_tile, sizeof lum_tile},
    {"hist", 1716, "histogramBins_Buffer", lum_hist, sizeof lum_hist},
};

static int has(const void *c, SIZE_T n, const char *s)
{
    size_t k = strlen(s);
    for (SIZE_T i = 0; i + k <= n; i++)
        if (!memcmp((const char *)c + i, s, k)) return 1;
    return 0;
}

static HRESULT STDMETHODCALLTYPE hk_CreateComputeShader(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, void **o)
{
    PFN_CSH orig = VT_ORIG(d, &h_ccs, PFN_CSH);
    if (!c || !o || dxbc_find_binding(c, n, "scene_Buffer") < 0) return orig(d, c, n, l, o);   /* !o — игра только проверяет */
    char name[300] = "";
    const char *why = "not found in the game's shader cache";
    if (gd_cs_name(c, n, name, sizeof name)) {
        ID3DBlob *b = rebuild(name, (const BYTE *)c, n, &why);
        if (b) {
            const void *bc = ID3D10Blob_GetBufferPointer(b);
            SIZE_T bn = ID3D10Blob_GetBufferSize(b);
            int slot = dxbc_find_binding(bc, bn, "scene_Buffer");
            /* игра привязывает ресурсы по регистрам: пересборка не должна их сдвинуть */
            int same = dxbc_same_bindings(c, n, bc, bn);
            HRESULT hr = same ? orig(d, bc, bn, l, o) : E_FAIL;
            ID3D10Blob_Release(b);
            if (SUCCEEDED(hr) && *o && slot >= 0) {
                UINT s = (UINT)slot;
                ID3D11ComputeShader_SetPrivateData((ID3D11ComputeShader *)*o, &GUID_LUM, sizeof s, &s);
                g_cs[(InterlockedIncrement(&g_ncs) - 1) % LUM_PS] = *o;
                LOG("exposure: %s rebuilt from the game's source - reads the whole scene image", name);
                return hr;
            }
            if (SUCCEEDED(hr) && *o) { IUnknown_Release((IUnknown *)*o); *o = NULL; }
            /* ревизия кода 4, п. 6: причина по тому, что случилось, а не по коду ошибки (E_FAIL бывает и у самого D3D) */
            why = !same ? "resource registers differ after rebuilding" : "created shader rejected";
        }
    }
    /* запасной путь: копии двух шейдеров 16.11, если это они */
    for (int i = 0; i < (int)(sizeof g_known / sizeof *g_known); i++)
        if (n == g_known[i].len && has(c, n, "scene_Buffer") && has(c, n, g_known[i].res)) {
            HRESULT hr = orig(d, g_known[i].code, g_known[i].clen, l, o);
            LOG("exposure: %s (%s) - copy of game version 16.11 used -> 0x%08lx", name[0] ? name : g_known[i].name, why,
                (unsigned long)hr);
            if (SUCCEEDED(hr)) return hr;
            break;
        }
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 4)
        LOG("exposure: compute shader reading scene_Buffer %s left as is (%s) - on entries with world != screen brightness is "
            "measured on part of the image", name[0] ? name : "(unknown)", why);
    return orig(d, c, n, l, o);
}

void lum_hook_device(ID3D11Device *dev)
{
    Ratio one = {1, 1};
    if (!g_cfg.lum_fix || (ratio_eq(g_cfg.scene, one) && !presets_on())) return; /* картинка мира в логическом размере — чинить нечего */
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateComputeShader, hk_CreateComputeShader, &h_ccs);
}

/* ---- запуск: свой буфер с коэффициентом ---- */
typedef void(STDMETHODCALLTYPE *PFN_CSS)(ID3D11DeviceContext *, ID3D11ComputeShader *, ID3D11ClassInstance *const *, UINT);
static void STDMETHODCALLTYPE hk_CSSetShader(ID3D11DeviceContext *c, ID3D11ComputeShader *cs, ID3D11ClassInstance *const *ci, UINT n)
{
    CtxState *s = &CTXW(c)->st;
    s->cs = cs;
    s->cs_lum = 0;
    if (cs && g_ncs > LUM_PS) s->cs_lum = -1;   /* ревизия кода 4, п. 3: адресов больше, чем помнит отсев, — метку спросим всегда */
    else if (cs)
        for (int i = 0; i < LUM_PS; i++)
            if (g_cs[i] == cs) { s->cs_lum = -1; break; }
    ORIGC(c, CSSetShader, PFN_CSS)(RC(c), cs, ci, n);
}

static ID3D11Buffer *g_kcb;   /* свой буфер (одно устройство у игры) */

int lum_pre_dispatch(ID3D11DeviceContext *c, ID3D11Buffer **prev)
{
    CtxState *s = &CTXW(c)->st;
    *prev = NULL;
    if (!s->cs || !s->cs_lum) return 0;
    UINT slot = 0, sz = sizeof slot;
    if (FAILED(ID3D11ComputeShader_GetPrivateData(s->cs, &GUID_LUM, &sz, &slot)) || sz != sizeof slot) { s->cs_lum = 0; return 0; }
    float k[4] = {1, 1, 0, 0};
    ID3D11ShaderResourceView *v = NULL;
    ID3D11DeviceContext_CSGetShaderResources(RC(c), slot, 1, &v);
    ScaledInfo si;
    if (v && view_scaled((ID3D11View *)v, &si)) {
        UINT lw, lh, pw, ph;
        res_mip_size(&si, 0, &lw, &lh, &pw, &ph);
        if (lw && lh) { k[0] = (float)pw / (float)lw; k[1] = (float)ph / (float)lh; }
    }
    if (v) ID3D11ShaderResourceView_Release(v);
    if (!g_kcb) {
        ID3D11Device *d = NULL;
        ID3D11DeviceContext_GetDevice(RC(c), &d);
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3D11Buffer *b = NULL;
        if (d && SUCCEEDED(ID3D11Device_CreateBuffer(d, &bd, NULL, &b)) &&
            InterlockedCompareExchangePointer((void *volatile *)&g_kcb, b, NULL) != NULL)
            ID3D11Buffer_Release(b);   /* другой поток успел */
        if (d) ID3D11Device_Release(d);
        if (!g_kcb) return 0;
    }
    ID3D11DeviceContext_UpdateSubresource(RC(c), (ID3D11Resource *)g_kcb, 0, NULL, k, 0, 0);
    ID3D11DeviceContext_CSGetConstantBuffers(RC(c), LUM_CB, 1, prev);
    ID3D11DeviceContext_CSSetConstantBuffers(RC(c), LUM_CB, 1, &g_kcb);
    return 1;
}

void lum_post_dispatch(ID3D11DeviceContext *c, ID3D11Buffer *prev)
{
    ID3D11DeviceContext_CSSetConstantBuffers(RC(c), LUM_CB, 1, &prev);
    if (prev) ID3D11Buffer_Release(prev);
}

void lum_install(void)
{
    Ratio one = {1, 1};
    if (!g_cfg.lum_fix || (ratio_eq(g_cfg.scene, one) && !presets_on())) return;
    CTX_HOOK(CSSetShader, hk_CSSetShader);
}
