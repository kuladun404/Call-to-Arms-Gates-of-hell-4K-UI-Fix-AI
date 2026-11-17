/* Хуки рисования. Ставятся, только если нужны: F14 (рендер не 100% — CB с экранными константами под цель) или
 * диагностика (diag=1 — снимок кадра). Иначе рисование идёт мимо мода, как раньше. */
#include "common.h"

#define PASS(field, PFN, ...) ORIGC(c, field, PFN)(RC(c), __VA_ARGS__)
typedef void(STDMETHODCALLTYPE *PFN_D)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DI)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *PFN_DIN)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DIIN)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DIND)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DA)(ID3D11DeviceContext *);
typedef void(STDMETHODCALLTYPE *PFN_DSP)(ID3D11DeviceContext *, UINT, UINT, UINT);



static inline void pre(ID3D11DeviceContext *c, const char *what, UINT a, UINT b, int compute)
{
    if (!compute) world_pre_draw(c);   /* мир без HDR: первое рисование интерфейса — мир на экран */
    if (!compute && !ratio_eq(g_cfg.scene, g_cfg.scale)) cb_pre_draw(c);   /* F14: сцена и выход разного размера */
    if (!compute) dof_pre_draw(c);     /* Г4: радиус размытия глубины резкости — только на время шейдера размытия */
    if (g_cap) diag_draw(c, what, a, b, compute);
}

static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext *c, UINT n, UINT s)
{ pre(c, "Draw", n, s, 0); PASS(Draw, PFN_D, n, s); }
static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b)
{ pre(c, "DrawIndexed", n, s, 0); PASS(DrawIndexed, PFN_DI, n, s, b); }
static void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext *c, UINT n, UINT i, UINT s, UINT si)
{ pre(c, "DrawInstanced", n, i, 0); PASS(DrawInstanced, PFN_DIN, n, i, s, si); }
static void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext *c, UINT n, UINT i, UINT s, INT b, UINT si)
{ pre(c, "DrawIndexedInstanced", n, i, 0); PASS(DrawIndexedInstanced, PFN_DIIN, n, i, s, b, si); }
static void STDMETHODCALLTYPE hk_DrawIndexedInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *a, UINT o)
{ pre(c, "DrawIdxInstIndirect", 0, 0, 0); PASS(DrawIndexedInstancedIndirect, PFN_DIND, a, o); }
static void STDMETHODCALLTYPE hk_DrawInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *a, UINT o)
{ pre(c, "DrawInstIndirect", 0, 0, 0); PASS(DrawInstancedIndirect, PFN_DIND, a, o); }
static void STDMETHODCALLTYPE hk_DrawAuto(ID3D11DeviceContext *c)
{ pre(c, "DrawAuto", 0, 0, 0); ORIGC(c, DrawAuto, PFN_DA)(RC(c)); }
/* автоэкспозиция (lum.c): пересобранному шейдеру — свой буфер с коэффициентом картинки на время запуска */
static void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z)
{
    pre(c, "Dispatch", x, y, 1);
    ID3D11Buffer *pv;
    int l = lum_pre_dispatch(c, &pv);
    PASS(Dispatch, PFN_DSP, x, y, z);
    if (l) lum_post_dispatch(c, pv);
}
typedef void(STDMETHODCALLTYPE *PFN_DSPI)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
static void STDMETHODCALLTYPE hk_DispatchIndirect(ID3D11DeviceContext *c, ID3D11Buffer *a, UINT o)
{
    pre(c, "DispatchIndirect", 0, 0, 1);
    ID3D11Buffer *pv;
    int l = lum_pre_dispatch(c, &pv);
    PASS(DispatchIndirect, PFN_DSPI, a, o);
    if (l) lum_post_dispatch(c, pv);
}

void draw_install(void)
{
    if (ratio_eq(g_cfg.scene, g_cfg.scale) && !presets_on() && !g_cfg.diag && g_cfg.dof_radius <= 0 && !g_cfg.lum_fix) return;
    CTX_HOOK(Draw, hk_Draw);
    CTX_HOOK(DrawIndexed, hk_DrawIndexed);
    CTX_HOOK(DrawInstanced, hk_DrawInstanced);
    CTX_HOOK(DrawIndexedInstanced, hk_DrawIndexedInstanced);
    CTX_HOOK(DrawIndexedInstancedIndirect, hk_DrawIndexedInstancedIndirect);
    CTX_HOOK(DrawInstancedIndirect, hk_DrawInstancedIndirect);
    CTX_HOOK(DrawAuto, hk_DrawAuto);
    CTX_HOOK(Dispatch, hk_Dispatch);
    CTX_HOOK(DispatchIndirect, hk_DispatchIndirect);
}
