#pragma once
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>

/* ---- config (proxy.c) ---- */
extern int g_enabled;   /* 0 = чистый passthrough, хуки не ставятся */
extern int g_scale;     /* множитель физического разрешения (2) */
extern int g_log;       /* 0 нет, 1 основное, 2 подробно */
extern int g_scan_cb;   /* 1 = искать экранные константы в константных буферах (диагностика) */
extern int g_cbfix[16], g_ncbfix; /* cb_fix: размеры CB (в float) для пересчёта экранных констант */
extern int g_scale_rt;  /* 1 = масштабировать и прочие (не pow2) RT — кэши виджетов UI */

/* логический размер экрана (то, что видит игра); 0 пока swapchain не создан */
extern volatile LONG g_lw, g_lh;
/* поток, находящийся внутри D3D11CreateDeviceAndSwapChain (чтобы не масштабировать дважды) */
extern volatile DWORD g_cdsc_tid;

extern WCHAR g_dir[MAX_PATH]; /* папка DLL с завершающим \ */
void ui4k_log(const char *fmt, ...);
#define LOG(...)  do { if (g_log >= 1) ui4k_log(__VA_ARGS__); } while (0)
#define LOGV(...) do { if (g_log >= 2) ui4k_log(__VA_ARGS__); } while (0)

/* ---- vtable patching (proxy.c) ---- */
typedef struct { void *orig[8]; int n; size_t off; } VtHook;
void  vt_patch(void *obj, size_t slot_offset, void *hook, VtHook *h);
void *vt_orig(void *obj, VtHook *h);
#define VT_PATCH(obj, VtblType, field, hook, h) \
    vt_patch((obj), offsetof(VtblType, field), (void *)(hook), (h))
#define VT_ORIG(obj, h, FnType) ((FnType)vt_orig((obj), (h)))

/* ---- font_hd.c ---- */
extern int g_font_hd, g_font_dump, g_font_big, g_font_filter;
enum { FONT_BILINEAR = 0, FONT_NEAREST = 1, FONT_SHARP = 2 };
typedef HRESULT(STDMETHODCALLTYPE *PFN_CT2D_)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                              const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
typedef void(STDMETHODCALLTYPE *PFN_UPD_)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                          const void *, UINT, UINT);
int fonthd_create(ID3D11Device *dev, PFN_CT2D_ orig, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                  ID3D11Texture2D **out, HRESULT *phr);
int fonthd_update(ID3D11DeviceContext *real, PFN_UPD_ orig, ID3D11Resource *r, UINT sub, const D3D11_BOX *box,
                  const void *data, UINT rp);

/* ---- scale_d3d.c ---- */
void d3d_mark_scaled(ID3D11Texture2D *t); /* GetDesc будет показывать размер / scale */
void d3d_set_logical(UINT w, UINT h);
void d3d_hook_device(ID3D11Device *dev);
ID3D11DeviceContext *d3d_wrap_immediate(ID3D11DeviceContext *real); /* забирает ссылку */
void d3d_hook_swapchain(IDXGISwapChain *sc);
void d3d_hook_factory(IDXGIFactory *f);
void d3d_scale_swapchain_desc(DXGI_SWAP_CHAIN_DESC *d);

/* ---- scale_input.c ---- */
void input_install_iat(void);
void input_watch_window(HWND hwnd);
