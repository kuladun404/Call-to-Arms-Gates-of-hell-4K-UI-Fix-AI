/* РџСЂРѕРєСЃРё d3d11.dll: РіСЂСѓР·РёС‚ СЃРёСЃС‚РµРјРЅСѓСЋ d3d11.dll, РїСЂРѕР±СЂР°СЃС‹РІР°РµС‚ РІСЃРµ СЌРєСЃРїРѕСЂС‚С‹
 * (Р·Р°РіР»СѓС€РєРё jmp РёР· stubs.S, РіРµРЅРµСЂРёСЂСѓРµС‚ build.py), РїРµСЂРµС…РІР°С‚С‹РІР°РµС‚ СЃРѕР·РґР°РЅРёРµ СѓСЃС‚СЂРѕР№СЃС‚РІР°. */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>

int g_enabled = 1, g_scale = 2, g_log = 1, g_scale_rt = 1, g_scan_cb = 0;
int g_cbfix[16], g_ncbfix;
int g_font_hd = 1, g_font_dump = 0, g_font_big = 1, g_font_filter = 2;
volatile LONG g_lw = 0, g_lh = 0;
volatile DWORD g_cdsc_tid = 0;

/* С‚Р°Р±Р»РёС†Р° СЂРµР°Р»СЊРЅС‹С… Р°РґСЂРµСЃРѕРІ РґР»СЏ Р·Р°РіР»СѓС€РµРє */
static const char *const g_names[] = {
#define X(n) #n,
#include "exports.inc"
#undef X
};
void *g_real[sizeof(g_names) / sizeof(g_names[0])];

static HMODULE g_sys;
static CRITICAL_SECTION g_logcs;
static FILE *g_logf;
WCHAR g_dir[MAX_PATH];

void ui4k_log(const char *fmt, ...)
{
    if (!g_logf) return;
    EnterCriticalSection(&g_logcs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_logf, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list a; va_start(a, fmt); vfprintf(g_logf, fmt, a); va_end(a);
    fputc('\n', g_logf);
    fflush(g_logf);
    LeaveCriticalSection(&g_logcs);
}

/* ---- vtable patching ---- */
static CRITICAL_SECTION g_vtcs;

/* Р’ СЌС‚РѕРј СЂР°РЅС‚Р°Р№РјРµ Сѓ РєР°Р¶РґРѕРіРѕ РѕР±СЉРµРєС‚Р° D3D11 СЃРІРѕСЏ (РІСЃС‚СЂРѕРµРЅРЅР°СЏ) РєРѕРїРёСЏ vtable, РїРѕСЌС‚РѕРјСѓ
 * РїР°С‚С‡РёРј СЃР»РѕС‚ РєР°Р¶РґРѕРіРѕ РѕР±СЉРµРєС‚Р°. РћСЂРёРіРёРЅР°Р» РѕР±С‹С‡РЅРѕ РѕРґРёРЅ РЅР° РјРµС‚РѕРґ; РµСЃР»Рё РІСЃС‚СЂРµС‚РёР»РёСЃСЊ СЂР°Р·РЅС‹Рµ вЂ”
 * С…СЂР°РЅРёРј РёС… РІ С…РµС€-С‚Р°Р±Р»РёС†Рµ РїРѕ Р°РґСЂРµСЃСѓ СЃР»РѕС‚Р°. */
#define SLOTMAP (1 << 16)
static struct { void **slot; void *orig; } g_slotmap[SLOTMAP];

static void slotmap_put(void **slot, void *orig)
{
    size_t i = ((size_t)slot >> 3) * 0x9E3779B97F4A7C15ull >> 48;
    for (int k = 0; k < 64; k++, i = (i + 1) & (SLOTMAP - 1))
        if (!g_slotmap[i].slot || g_slotmap[i].slot == slot) { g_slotmap[i].slot = slot; g_slotmap[i].orig = orig; return; }
}

static void *slotmap_get(void **slot)
{
    size_t i = ((size_t)slot >> 3) * 0x9E3779B97F4A7C15ull >> 48;
    for (int k = 0; k < 64; k++, i = (i + 1) & (SLOTMAP - 1)) {
        if (g_slotmap[i].slot == slot) return g_slotmap[i].orig;
        if (!g_slotmap[i].slot) break;
    }
    return NULL;
}

static void write_ptr(void **slot, void *v)
{
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(slot, &mi, sizeof mi) &&
        (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
        *slot = v;
        return;
    }
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = v;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

void vt_patch(void *obj, size_t off, void *hook, VtHook *h)
{
    void **slot = (void **)((char *)*(void ***)obj + off);
    void *cur = *slot;
    if (cur == hook) return;
    EnterCriticalSection(&g_vtcs);
    h->off = off;
    int found = 0;
    for (int i = 0; i < h->n; i++)
        if (h->orig[i] == cur) found = 1;
    if (!found) {
        if (h->n < 8) h->orig[h->n++] = cur;
        if (h->n > 1) LOG("vt_patch: second original %p for slot +%zu (first %p)", cur, off, h->orig[0]);
    }
    if (h->n > 1) slotmap_put(slot, cur);
    write_ptr(slot, hook);
    LeaveCriticalSection(&g_vtcs);
}

void *vt_orig(void *obj, VtHook *h)
{
    if (h->n == 1) return h->orig[0];
    void *o = slotmap_get((void **)((char *)*(void ***)obj + h->off));
    return o ? o : h->orig[0];
}

/* ---- СЌРєСЃРїРѕСЂС‚РёСЂСѓРµРјС‹Рµ РїРµСЂРµС…РІР°С‚С‹ ---- */
typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                  UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

HRESULT WINAPI my_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
    UINT sdk, const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **psc, ID3D11Device **pdev,
    D3D_FEATURE_LEVEL *pfl, ID3D11DeviceContext **pctx)
{
    PFN_CDSC real = (PFN_CDSC)GetProcAddress(g_sys, "D3D11CreateDeviceAndSwapChain");
    DXGI_SWAP_CHAIN_DESC d;
    const DXGI_SWAP_CHAIN_DESC *use = scd;
    if (scd) {
        LOG("D3D11CreateDeviceAndSwapChain: %ux%u fmt=%d windowed=%d buffers=%u flags=0x%x hwnd=%p",
            scd->BufferDesc.Width, scd->BufferDesc.Height, scd->BufferDesc.Format, scd->Windowed,
            scd->BufferCount, scd->Flags, scd->OutputWindow);
        if (g_enabled) {
            d = *scd;
            d3d_scale_swapchain_desc(&d);
            use = &d;
            input_watch_window(scd->OutputWindow);
        }
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    g_cdsc_tid = GetCurrentThreadId();
    HRESULT hr = real(ad, dt, sw, flags, fl, nfl, sdk, use, psc, &dev, pfl, &ctx);
    g_cdsc_tid = 0;
    LOG("  -> hr=0x%08lx dev=%p sc=%p", hr, dev, psc ? *psc : NULL);
    if (SUCCEEDED(hr) && g_enabled) {
        if (dev) d3d_hook_device(dev);
        if (ctx) ctx = d3d_wrap_immediate(ctx);
        if (psc && *psc) d3d_hook_swapchain(*psc);
    }
    if (pdev) *pdev = dev; else if (dev) ID3D11Device_Release(dev);
    if (pctx) *pctx = ctx; else if (ctx) ID3D11DeviceContext_Release(ctx);
    return hr;
}

HRESULT WINAPI my_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw,
                                                          UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                                                          UINT sdk, ID3D11Device **pdev, D3D_FEATURE_LEVEL *pfl,
                                                          ID3D11DeviceContext **pctx)
{
    PFN_CD real = (PFN_CD)GetProcAddress(g_sys, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = real(ad, dt, sw, flags, fl, nfl, sdk, &dev, pfl, &ctx);
    LOG("D3D11CreateDevice -> hr=0x%08lx dev=%p (caller %p)", hr, dev, __builtin_return_address(0));
    if (SUCCEEDED(hr) && g_enabled && dev) {
        d3d_hook_device(dev);
        if (ctx) ctx = d3d_wrap_immediate(ctx);
    }
    if (pdev) *pdev = dev; else if (dev) ID3D11Device_Release(dev);
    if (pctx) *pctx = ctx; else if (ctx) ID3D11DeviceContext_Release(ctx);
    return hr;
}

static void load_config(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    WCHAR *s = wcsrchr(g_dir, L'\\');
    if (s) s[1] = 0;
    WCHAR ini[MAX_PATH], log[MAX_PATH];
    lstrcpyW(ini, g_dir); lstrcatW(ini, L"ui4k.ini");
    lstrcpyW(log, g_dir); lstrcatW(log, L"ui4k.log");
    g_enabled = GetPrivateProfileIntW(L"ui4k", L"enabled", 1, ini);
    g_scale = GetPrivateProfileIntW(L"ui4k", L"scale", 2, ini);
    g_log = GetPrivateProfileIntW(L"ui4k", L"log", 1, ini);
    g_scale_rt = GetPrivateProfileIntW(L"ui4k", L"scale_ui_rt", 1, ini);
    g_scan_cb = GetPrivateProfileIntW(L"ui4k", L"scan_cb", 0, ini);
    g_font_hd = GetPrivateProfileIntW(L"ui4k", L"font_hd", 1, ini);
    g_font_dump = GetPrivateProfileIntW(L"ui4k", L"font_dump", 0, ini);
    g_font_big = GetPrivateProfileIntW(L"ui4k", L"font_big", 1, ini);
    {
        WCHAR f[32];
        GetPrivateProfileStringW(L"ui4k", L"font_filter", L"sharp", f, 32, ini);
        g_font_filter = !lstrcmpiW(f, L"bilinear") ? FONT_BILINEAR : !lstrcmpiW(f, L"nearest") ? FONT_NEAREST : FONT_SHARP;
    }
    {
        WCHAR buf[256], *p, *e;
        GetPrivateProfileStringW(L"ui4k", L"cb_fix", L"156,84", buf, 256, ini);
        for (p = buf; *p && g_ncbfix < 16; p = e) {
            long v = wcstol(p, &e, 10);
            if (e == p) { e = p + 1; continue; }
            if (v > 0) g_cbfix[g_ncbfix++] = (int)v;
        }
    }
    if (g_scale < 1 || g_scale > 4) g_scale = 2;
    if (g_log) g_logf = _wfopen(log, L"w");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logcs);
        InitializeCriticalSection(&g_vtcs);
        load_config(inst);
        WCHAR p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        lstrcatW(p, L"\\d3d11.dll");
        g_sys = LoadLibraryW(p);
        if (!g_sys) { LOG("FATAL: cannot load %ls", p); return FALSE; }
        for (size_t i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++)
            g_real[i] = (void *)GetProcAddress(g_sys, g_names[i]);
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        LOG("ui4k loaded into %s; enabled=%d scale=%d scale_ui_rt=%d log=%d cb_fix=%d sizes", exe, g_enabled, g_scale, g_scale_rt, g_log, g_ncbfix);
        if (g_enabled) input_install_iat();
    }
    return TRUE;
}
