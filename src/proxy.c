/* F02: прокси d3d11.dll. Грузит системную d3d11.dll, пробрасывает все экспорты (заглушки jmp из stubs.S,
 * генерирует build.py), перехватывает создание устройства. */
#include "common.h"

static const char *const g_names[] = {
#define X(n) #n,
#include "exports.inc"
#undef X
};
void *g_real[sizeof(g_names) / sizeof(g_names[0])];

static HMODULE g_sys;
static BYTE *g_sys_base;
static SIZE_T g_sys_size;

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
    WCHAR p[MAX_PATH];
    GetSystemDirectoryW(p, MAX_PATH);
    lstrcatW(p, L"\\d3d11.dll");
    g_sys = LoadLibraryW(p);
    if (!g_sys) {
        LOG("FATAL: cannot load %ls", p);
        return FALSE;
    }
    g_sys_base = (BYTE *)g_sys;
    g_sys_size = ((IMAGE_NT_HEADERS *)(g_sys_base + ((IMAGE_DOS_HEADER *)g_sys_base)->e_lfanew))->OptionalHeader.SizeOfImage;
    int missing = 0;
    for (size_t i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        g_real[i] = (void *)GetProcAddress(g_sys, g_names[i]);
        missing += g_real[i] == NULL;
    }
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
