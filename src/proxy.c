/* F02: прокси d3d11.dll. Грузит системную d3d11.dll, пробрасывает все экспорты (заглушки jmp из stubs.S,
 * генерирует build.py), перехватывает создание устройства. */
#include "common.h"
#include <stdio.h>
#include <string.h>

static const char *const g_names[] = {
#define X(n) #n,
#include "exports.inc"
#undef X
};
void *g_real[sizeof(g_names) / sizeof(g_names[0])];

static HMODULE g_sys;
static BYTE *g_sys_base;
static SIZE_T g_sys_size;

/* есть ли в файле строка s (файл читается кусками; совпадение на стыке кусков тоже находится) */
static int file_has(const WCHAR *path, const char *s)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static char buf[65536 + 64];
    size_t n = strlen(s), keep = 0;
    DWORD got;
    int found = 0;
    while (!found && ReadFile(f, buf + keep, 65536, &got, NULL) && got) {
        size_t len = keep + got;
        for (size_t i = 0; i + n <= len && !found; i++) found = !memcmp(buf + i, s, n);
        keep = len >= n - 1 ? n - 1 : len;   /* хвост — к началу следующего куска */
        memmove(buf, buf + len - keep, keep);
    }
    CloseHandle(f);
    return found;
}

int ui4k_from_d3d(void *ra)
{
    return (g_sys_base && (BYTE *)ra >= g_sys_base && (BYTE *)ra < g_sys_base + g_sys_size) || d3dbg_from_sdk(ra);
}

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                  UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

/* устройство и контекст после создания: хуки + обёртка; отдаёт игре только то, что она просила */
static void finish_device(HRESULT hr, ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Device **pdev,
                          ID3D11DeviceContext **pctx)
{
    if (SUCCEEDED(hr) && dev) d3dbg_device(dev);   /* d3d_debug=1: сообщения слоя отладки — в лог */
    if (SUCCEEDED(hr) && g_cfg.enabled) {
        if (dev) device_hook(dev);
        if (ctx) ctx = ctx_wrap(ctx);
    }
    if (pdev) *pdev = dev; else if (dev) ID3D11Device_Release(dev);
    if (pctx) *pctx = ctx; else if (ctx) ID3D11DeviceContext_Release(ctx);
}

HRESULT WINAPI my_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                                                const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
                                                const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **psc,
                                                ID3D11Device **pdev, D3D_FEATURE_LEVEL *pfl, ID3D11DeviceContext **pctx)
{
    PFN_CDSC real = (PFN_CDSC)GetProcAddress(g_sys, "D3D11CreateDeviceAndSwapChain");
    if (scd)
        LOG("D3D11CreateDeviceAndSwapChain: %ux%u fmt=%d samples=%u windowed=%d buffers=%u flags=0x%x hwnd=%p",
            scd->BufferDesc.Width, scd->BufferDesc.Height, scd->BufferDesc.Format, scd->SampleDesc.Count, scd->Windowed,
            scd->BufferCount, scd->Flags, scd->OutputWindow);
    DXGI_SWAP_CHAIN_DESC d;
    const DXGI_SWAP_CHAIN_DESC *use = scd;
    if (scd && g_cfg.enabled) {   /* F04: swapchain физический */
        config_poll_profile();
        msaa_swapchain(scd->SampleDesc.Count, 0);
        d = *scd;
        sc_scale_desc(&d);
        use = &d;
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    g_cdsc_tid = GetCurrentThreadId();
    vt_suspend();   /* оверлей Steam ставит свои перехваты внутри этого вызова: пусть видит таблицы без мода */
    UINT fx = d3dbg_flags(flags);
    HRESULT hr = real(ad, dt, sw, fx, fl, nfl, sdk, use, psc, &dev, pfl, &ctx);
    if (d3dbg_retry(hr, fx)) hr = real(ad, dt, sw, flags, fl, nfl, sdk, use, psc, &dev, pfl, &ctx);
    vt_resume();
    g_cdsc_tid = 0;
    LOG("  -> hr=0x%08lx dev=%p sc=%p", hr, dev, psc ? *psc : NULL);
    if (SUCCEEDED(hr) && g_cfg.enabled && psc && *psc) sc_hook(*psc);
    finish_device(hr, dev, ctx, pdev, pctx);
    return hr;
}

HRESULT WINAPI my_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                                    const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk, ID3D11Device **pdev,
                                    D3D_FEATURE_LEVEL *pfl, ID3D11DeviceContext **pctx)
{
    PFN_CD real = (PFN_CD)GetProcAddress(g_sys, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    vt_suspend();
    UINT fx = d3dbg_flags(flags);
    HRESULT hr = real(ad, dt, sw, fx, fl, nfl, sdk, &dev, pfl, &ctx);
    if (d3dbg_retry(hr, fx)) hr = real(ad, dt, sw, flags, fl, nfl, sdk, &dev, pfl, &ctx);
    vt_resume();
    LOG("D3D11CreateDevice -> hr=0x%08lx dev=%p (caller %p)", hr, dev, __builtin_return_address(0));
    finish_device(hr, dev, ctx, pdev, pctx);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    vt_init();
    config_load(inst);
    if (g_cfg.log) crash_install();
    /* Linux / Steam Deck: Wine (Proton) — там системная d3d11.dll уже DXVK; поведение окон и режимов отличается */
    typedef const char *(CDECL *PFN_WINEVER)(void);
    PFN_WINEVER wine_ver = (PFN_WINEVER)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    if (wine_ver) LOG("wine: running under Wine / Proton %s - Linux support is experimental, please send this log", wine_ver());
    if (wine_ver && g_next_auto) {   /* DXVK для Windows из дополнения поверх DXVK Proton — два разных DXVK вместе */
        LOG("wine: %ls next to the mod is ignored - Proton has its own DXVK; delete d3d11_dxvk.dll and dxgi.dll "
            "(or set next_d3d11= in ui4k.ini to use it anyway)", g_next_d3d11);
        g_next_d3d11[0] = 0;
    }
    WCHAR p[MAX_PATH];
    if (g_next_d3d11[0]) {   /* цепочка: d3d11.dll другой программы (DXVK и т. п.) вместо системной */
        /* LOAD_WITH_ALTERED_SEARCH_PATH: её зависимости (dxgi.dll DXVK) ищутся сначала в её папке */
        g_sys = LoadLibraryExW(g_next_d3d11, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        DWORD err = GetLastError();
        if (g_sys == (HMODULE)inst) {   /* указали саму себя: бесконечная цепочка */
            FreeLibrary(g_sys);
            g_sys = NULL;
            LOG("next_d3d11=%ls is the mod itself - ignored, the system d3d11.dll is used", g_next_d3d11);
        } else if (!g_sys) {
            LOG("next_d3d11: cannot load %ls (error %lu) - the system d3d11.dll is used", g_next_d3d11, err);
        } else {
            LOG("next_d3d11: %ls is used instead of the system d3d11.dll", g_next_d3d11);
        }
    }
    GetSystemDirectoryW(p, MAX_PATH);
    lstrcatW(p, L"\\d3d11.dll");
    if (!g_sys) {
        g_sys = LoadLibraryW(p);
        /* exe грузит dxgi.dll из своей папки раньше мода (Exe.md: импорт CreateDXGIFactory). dxgi.dll из DXVK
         * без d3d11 из DXVK — игра не запустится; под Wine такой dxgi.dll в папке не берётся (встроенный Proton) */
        WCHAR dx[MAX_PATH];
        if (!wine_ver && dir_file(dx, L"dxgi.dll") && file_has(dx, "DXVK_LOG_LEVEL"))
            LOG("WARNING: %ls is DXVK's dxgi.dll, but DXVK's d3d11 is not used (%s) - the game will not start properly. "
                "Delete dxgi.dll (and d3d11_dxvk.dll) to play without DXVK, or put DXVK's d3d11.dll next to the mod as "
                "d3d11_dxvk.dll and remove next_d3d11=system from ui4k.ini", dx,
                !lstrcmpiW(g_next_d3d11, L"") ? "no d3d11_dxvk.dll next to the mod or next_d3d11=system" : "it did not load");
    }
    if (!g_sys) {
        LOG("FATAL: cannot load %ls", p);
        return FALSE;
    }
    g_sys_base = (BYTE *)g_sys;
    g_sys_size = ((IMAGE_NT_HEADERS *)(g_sys_base + ((IMAGE_DOS_HEADER *)g_sys_base)->e_lfanew))->OptionalHeader.SizeOfImage;
    int missing = 0;
    char miss[600];
    int mp = 0;
    miss[0] = 0;
    for (size_t i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        g_real[i] = (void *)GetProcAddress(g_sys, g_names[i]);
        missing += g_real[i] == NULL;
        if (!g_real[i] && mp < (int)sizeof miss - 48) mp += snprintf(miss + mp, sizeof miss - mp, "%s%s", mp ? ", " : "", g_names[i]);
    }
    if (missing && g_next_d3d11[0])   /* Exe.md: игра берёт у d3d11.dll только D3D11CreateDeviceAndSwapChain */
        LOG("next_d3d11: %d exports are not in it (the game itself uses only D3D11CreateDeviceAndSwapChain): %s%s", missing,
            miss, mp >= (int)sizeof miss - 48 ? "..." : "");
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    LOG("ui4k %s loaded into %s; enabled=%d scale=%d/%d log=%d; exports %zu (missing in system dll: %d)", UI4K_VERSION,
        exe, g_cfg.enabled, g_cfg.scale.num, g_cfg.scale.den, g_cfg.log, sizeof(g_names) / sizeof(g_names[0]), missing);
    if (g_cfg.enabled) {
        dpi_init();   /* В1: до всего, что спрашивает у Windows размеры */
        if (g_cfg.fit_native && g_cfg.presets) presets_build();   /* L8: список разрешений = «мир + масштаб» */
        if (presets_on()) {
            LOG("presets: interface %d/%d, world %d/%d until the game picks its entry", g_cfg.scale.num, g_cfg.scale.den,
                g_cfg.scene.num, g_cfg.scene.den);
        } else {
            if (g_cfg.fit_native && !ratio_is_one(g_cfg.scale)) modes_fit();
            long nw = g_cfg.render_w ? modes_native_width() : 0;   /* F14: рендер = доля монитора */
            g_cfg.scene = nw > 0 ? ratio_mul(g_cfg.scale, g_cfg.render_w, (int)nw) : ratio_mul(g_cfg.scale, g_cfg.render ? g_cfg.render : 100, 100);
            LOG("render %s%d%s (monitor width %ld): scene textures %d/%d, interface %d/%d", g_cfg.render_w ? "width " : "",
                g_cfg.render_w ? g_cfg.render_w : g_cfg.render, g_cfg.render_w ? "" : "%", nw, g_cfg.scene.num, g_cfg.scene.den,
                g_cfg.scale.num, g_cfg.scale.den);
        }
        if (g_cfg.game_aa > 0)
            LOG("MSAA x%ld is on in the game settings%s", (long)g_cfg.game_aa,
                g_cfg.msaa_safe ? " - safe mode (msaa_safe=1): world rendered at interface size"
                                : " - the world keeps the size of the entry (own world target is multisampled)");
        scene_set_base(g_cfg.scene);
        if (msaa_safe()) g_cfg.scene = g_cfg.scale;
        vp_install();
        cb_install();
        copy_install();
        fontpages_install();
        sc_install_iat();
        input_install();
        cursor_install();
        draw_install();
        world_install();
        dof_install();
        lum_install();
        loc_install();
        if (g_cfg.diag) diag_install();
    }
    return TRUE;
}
