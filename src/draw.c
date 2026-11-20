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
    /* ревизия 20.11, Ф2: рисование оверлея внутри Present — правки мода для кадра игры (мир, шаг копии, радиус размытия,
     * замеры) его не касаются; состояние обёртки после Present вернётся к игре (ctx_present_restore) */
    if (in_present()) { if (g_cap) diag_draw(c, what, a, b, compute); return; }
    if (!compute) world_pre_draw(c);   /* мир без HDR: первое рисование интерфейса — мир на экран */
    if (!compute && !ratio_eq(g_cfg.scene, g_cfg.scale)) cb_pre_draw(c);   /* F14: сцена и выход разного размера */
    if (!compute) box_pre_draw(c);     /* Э8: шаг уменьшения копии — только на время шейдера копирования */
    if (!compute) dof_pre_draw(c);     /* Г4: радиус размытия глубины резкости — только на время шейдера размытия */
    if (g_cap) diag_draw(c, what, a, b, compute);
    if (g_cfg.perf && !compute) perf_draw(c);   /* Э1: метка начала интерфейса, перепись */
}

/* Э5 (реверс 18.11): рамка обзора камеры на миникарте — 4 рисования LineStrip в 1 пиксель. В цель xk линия остаётся
 * в 1 физический пиксель — в k раз тоньше логического. Дорисовать её со сдвигом viewport на 1..t-1 физических пикселей
 * по x и по y (t — k, округлённо): толщина как у игры в логических пикселях. Только непосредственный контекст, VS
 * интерфейса, цель увеличена; цена — 2 лишних рисования на линию при x2 (4 линии в кадре). */
typedef void(STDMETHODCALLTYPE *PFN_TOPO)(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
static void STDMETHODCALLTYPE hk_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
    CTXW(c)->st.topo = t;
    ORIGC(c, IASetPrimitiveTopology, PFN_TOPO)(RC(c), t);
}

static int thick_t(ID3D11DeviceContext *c)
{
    CtxState *st = &CTXW(c)->st;
    if (!g_cfg.thick_lines || (st->topo != D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP && st->topo != D3D11_PRIMITIVE_TOPOLOGY_LINELIST))
        return 0;
    if (CTXW(c)->type != D3D11_DEVICE_CONTEXT_IMMEDIATE || !st->rk.num || !st->nvp || !world_ui_vs(c)) return 0;
    if (in_present()) return 0;   /* оверлей внутри Present: состояние обёртки — игры, не его */
    int t = (int)((2 * st->rk.num + st->rk.den) / (2 * st->rk.den));   /* k округлённо */
    return t > 4 ? 4 : t;
}

#define THICK(call) do {                                                                     \
        int t_ = thick_t(c);                                                                 \
        if (t_ < 2) break;                                                                   \
        for (int i_ = 1; i_ < t_; i_++) {                                                    \
            vp_shift(c, (float)i_, 0); call;                                                 \
            vp_shift(c, 0, (float)i_); call;                                                 \
        }                                                                                    \
        vp_apply(c);                                                                         \
    } while (0)

void draw_key(void)
{
    static int was;
    int down = ui4k_hotkey(VK_F9, 0);
    if (down && !was) {
        LONG on = !InterlockedXor(&g_cfg.thick_lines, 1);
        LOG("lines: interface lines %s (Ctrl+F9)", on ? "drawn as thick as one logical pixel" : "left 1 physical pixel thin");
    }
    was = down;
}

static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext *c, UINT n, UINT s)
{
    pre(c, "Draw", n, s, 0);
    if (CTXW(c)->st.fp_live && fontpages_draw(c, n, s)) return;   /* Э6: буквы своей страницы — своими прямоугольниками */
    PASS(Draw, PFN_D, n, s);
    THICK(PASS(Draw, PFN_D, n, s));
}
static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b)
{ pre(c, "DrawIndexed", n, s, 0); PASS(DrawIndexed, PFN_DI, n, s, b); THICK(PASS(DrawIndexed, PFN_DI, n, s, b)); }
static void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext *c, UINT n, UINT i, UINT s, UINT si)
{ pre(c, "DrawInstanced", n, i, 0); PASS(DrawInstanced, PFN_DIN, n, i, s, si); THICK(PASS(DrawInstanced, PFN_DIN, n, i, s, si)); }
static void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext *c, UINT n, UINT i, UINT s, INT b, UINT si)
{ pre(c, "DrawIndexedInstanced", n, i, 0); PASS(DrawIndexedInstanced, PFN_DIIN, n, i, s, b, si); THICK(PASS(DrawIndexedInstanced, PFN_DIIN, n, i, s, b, si)); }
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
    /* ревизия v3, п. 1: чёткие буквы (Э6) рисуются из hk_Draw по топологии из hk_IASetPrimitiveTopology — без них fonts=1
     * при выключенных остальных ключах молча не работал */
    if (ratio_eq(g_cfg.scene, g_cfg.scale) && !presets_on() && !g_cfg.diag && g_cfg.dof_radius <= 0 && !box_on() && !g_cfg.lum_fix && !g_cfg.perf &&
        !g_cfg.thick_lines && !g_cfg.fonts)
        return;
    CTX_HOOK(Draw, hk_Draw);
    CTX_HOOK(IASetPrimitiveTopology, hk_IASetPrimitiveTopology);   /* Э5 */
    CTX_HOOK(DrawIndexed, hk_DrawIndexed);
    CTX_HOOK(DrawInstanced, hk_DrawInstanced);
    CTX_HOOK(DrawIndexedInstanced, hk_DrawIndexedInstanced);
    CTX_HOOK(DrawIndexedInstancedIndirect, hk_DrawIndexedInstancedIndirect);
    CTX_HOOK(DrawInstancedIndirect, hk_DrawInstancedIndirect);
    CTX_HOOK(DrawAuto, hk_DrawAuto);
    CTX_HOOK(Dispatch, hk_Dispatch);
    CTX_HOOK(DispatchIndirect, hk_DispatchIndirect);
}
