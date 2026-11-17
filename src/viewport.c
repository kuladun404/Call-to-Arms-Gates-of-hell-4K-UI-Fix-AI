/* F07: viewport и scissor. Игра задаёт логические значения; обёртка хранит их в CtxState и отдаёт настоящему
 * контексту в физике, пока привязана масштабированная цель (RT/DS/UAV с меткой F05). RSGet* возвращают логические.
 * Перевод по краям, тем же to_phys(), что и размер ресурса: край экрана логического попадает ровно в край
 * физического, соседние прямоугольники стыкуются без щелей (при ×1.5 умножение ширины дало бы щели в 1 px). */
#include "common.h"

typedef void(STDMETHODCALLTYPE *PFN_VP)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *PFN_SC)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
typedef void(STDMETHODCALLTYPE *PFN_GVP)(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *PFN_GSC)(ID3D11DeviceContext *, UINT *, D3D11_RECT *);
typedef void(STDMETHODCALLTYPE *PFN_OM)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                       ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *PFN_OMUAV)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                          ID3D11DepthStencilView *, UINT, UINT, ID3D11UnorderedAccessView *const *,
                                          const UINT *);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR)(ID3D11DeviceContext *);
typedef void(STDMETHODCALLTYPE *PFN_ECL)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FCL)(ID3D11DeviceContext *, BOOL, ID3D11CommandList **);

#define ST(c) (&CTXW(c)->st)

static int is_int(float f) { return f == (float)(long long)f; }

/* край x -> физика: целые — to_phys (как размеры ресурсов), дробные — умножение */
static float edge(Ratio k, float x) { return is_int(x) ? (float)to_phys(k, (long long)x) : to_phys_f(k, x); }

void vp_to_phys(Ratio k, D3D11_VIEWPORT *v)
{
    float l = edge(k, v->TopLeftX), t = edge(k, v->TopLeftY);
    float r = edge(k, v->TopLeftX + v->Width), b = edge(k, v->TopLeftY + v->Height);
    v->TopLeftX = l; v->TopLeftY = t; v->Width = r - l; v->Height = b - t;
}

static void apply_vp(ID3D11DeviceContext *c)
{
    CtxState *s = ST(c);
    D3D11_VIEWPORT v[MAX_VP];
    for (UINT i = 0; i < s->nvp; i++) {
        v[i] = s->vp[i];
        if (s->rk.num) vp_to_phys(s->rk, &v[i]);
    }
    ORIGC(c, RSSetViewports, PFN_VP)(RC(c), s->nvp, s->nvp ? v : NULL);
}

static void apply_sc(ID3D11DeviceContext *c)
{
    CtxState *s = ST(c);
    D3D11_RECT r[MAX_VP];
    for (UINT i = 0; i < s->nsc; i++) {
        r[i] = s->sc[i];
        if (s->rk.num) rect_to_phys(s->rk, &r[i].left, &r[i].top, &r[i].right, &r[i].bottom);
    }
    ORIGC(c, RSSetScissorRects, PFN_SC)(RC(c), s->nsc, s->nsc ? r : NULL);
}

static void set_ratio(ID3D11DeviceContext *c, Ratio r)
{
    CtxState *s = ST(c);
    if (s->rk.num == r.num && s->rk.den == r.den) return;
    s->rk = r;
    if (s->nvp) apply_vp(c);
    if (s->nsc) apply_sc(c);
}

static void reset(ID3D11DeviceContext *c)
{
    CtxState *s = ST(c);
    s->rk.num = 0; s->rk.den = 1; s->nvp = 0; s->nsc = 0;
    world_reset(c);
    dof_reset(c);
    ST(c)->cs = NULL; ST(c)->cs_lum = 0;
}

static Ratio view_ratio(ID3D11View *v)
{
    ScaledInfo si;
    if (view_scaled(v, &si)) return si.r;
    Ratio z = {0, 1};
    return z;
}

/* 1 — есть цель, *r — коэффициент первой (num 0 — не увеличена); 0 — целей нет (состояние не меняем) */
static int targets_ratio(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv, Ratio *r)
{
    for (UINT i = 0; i < n && rtv; i++)
        if (rtv[i]) { *r = view_ratio((ID3D11View *)rtv[i]); return 1; }
    if (dsv) { *r = view_ratio((ID3D11View *)dsv); return 1; }
    return 0;
}

/* настоящий размер и число выборок уровня mip текстуры, на которую смотрит вид */
typedef struct { UINT w, h, samples; int ok; } Real;
static Real view_real(ID3D11View *v, UINT mip)
{
    Real r = {0, 0, 0, 0};
    ID3D11Resource *res = NULL;
    ID3D11View_GetResource(v, &res);
    if (!res) return r;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ScaledInfo si;
        UINT lw, lh;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
        if (!res_scaled(res, &si)) { si.lw = d.Width; si.lh = d.Height; si.r.num = 1; si.r.den = 1; }
        res_mip_size(&si, mip, &lw, &lh, &r.w, &r.h);
        r.samples = d.SampleDesc.Count;
        r.ok = 1;
    }
    ID3D11Resource_Release(res);
    return r;
}

/* Цель и глубина должны совпадать по настоящему размеру и числу выборок: такую пару D3D не принимает, а без
 * отладочного слоя и не проверяет (В0). Игра считает их одинаковыми; расходятся они, когда мод увеличил одну и не
 * увеличил другую, или увеличил по-разному (F14: глубина сцены вместе с back buffer при тонмаппинге и интерфейсе —
 * там тест глубины выключен). Глубину отвязываем. */
static ID3D11DepthStencilView *fit_dsv(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
    if (!dsv || !rtv) return dsv;
    for (UINT i = 0; i < n; i++)
        if (rtv[i]) {
            Ratio a = view_ratio((ID3D11View *)rtv[i]), b = view_ratio((ID3D11View *)dsv);
            if (!a.num && !b.num) return dsv;                       /* мод не трогал ни ту, ни другую */
            if (a.num && b.num && ratio_eq(a, b)) return dsv;       /* увеличены одинаково */
            D3D11_RENDER_TARGET_VIEW_DESC rd;
            D3D11_DEPTH_STENCIL_VIEW_DESC dd;
            ID3D11RenderTargetView_GetDesc(rtv[i], &rd);
            ID3D11DepthStencilView_GetDesc(dsv, &dd);
            UINT rm = rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? rd.Texture2D.MipSlice
                    : rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ? rd.Texture2DArray.MipSlice : 0;
            UINT dm = dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D ? dd.Texture2D.MipSlice
                    : dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY ? dd.Texture2DArray.MipSlice : 0;
            Real ra = view_real((ID3D11View *)rtv[i], rm), rb = view_real((ID3D11View *)dsv, dm);
            if (ra.ok && rb.ok && (ra.w != rb.w || ra.h != rb.h || ra.samples != rb.samples)) {
                static volatile LONG said;
                if (InterlockedIncrement(&said) <= 8)
                    LOG("targets: depth detached - real size or sample count differs from the render target "
                        "(target %ux%u x%u, ratio %d/%d; depth %ux%u x%u, ratio %d/%d)", ra.w, ra.h, ra.samples, a.num, a.den,
                        rb.w, rb.h, rb.samples, b.num, b.den);
                return NULL;
            }
            break;
        }
    return dsv;
}

static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v)
{
    if (overlay_call(__builtin_return_address(0))) { ORIGC(c, RSSetViewports, PFN_VP)(RC(c), n, v); return; }
    CtxState *s = ST(c);
    if (n > MAX_VP) n = MAX_VP;
    s->nvp = v ? n : 0;
    if (v) memcpy(s->vp, v, n * sizeof *v);
    apply_vp(c);
}

static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{
    if (overlay_call(__builtin_return_address(0))) { ORIGC(c, RSSetScissorRects, PFN_SC)(RC(c), n, r); return; }
    CtxState *s = ST(c);
    if (n > MAX_VP) n = MAX_VP;
    s->nsc = r ? n : 0;
    if (r) memcpy(s->sc, r, n * sizeof *r);
    apply_sc(c);
}

static void STDMETHODCALLTYPE hk_RSGetViewports(ID3D11DeviceContext *c, UINT *pn, D3D11_VIEWPORT *v)
{
    if (overlay_call(__builtin_return_address(0))) { ORIGC(c, RSGetViewports, PFN_GVP)(RC(c), pn, v); return; }
    CtxState *s = ST(c);
    if (!pn) return;
    if (!v) { *pn = s->nvp; return; }
    for (UINT i = 0; i < *pn; i++)
        if (i < s->nvp) v[i] = s->vp[i]; else ZeroMemory(&v[i], sizeof v[i]);
}

static void STDMETHODCALLTYPE hk_RSGetScissorRects(ID3D11DeviceContext *c, UINT *pn, D3D11_RECT *r)
{
    if (overlay_call(__builtin_return_address(0))) { ORIGC(c, RSGetScissorRects, PFN_GSC)(RC(c), pn, r); return; }
    CtxState *s = ST(c);
    if (!pn) return;
    if (!r) { *pn = s->nsc; return; }
    for (UINT i = 0; i < *pn; i++)
        if (i < s->nsc) r[i] = s->sc[i]; else ZeroMemory(&r[i], sizeof r[i]);
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                                    ID3D11DepthStencilView *dsv)
{
    Ratio r;
    ID3D11RenderTargetView *tmp[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    rtv = world_rtv(c, n, rtv, dsv, tmp, __builtin_return_address(0));   /* мир без HDR: back buffer -> цель мира */
    int t = targets_ratio(n, rtv, dsv, &r);
    ORIGC(c, OMSetRenderTargets, PFN_OM)(RC(c), n, rtv, fit_dsv(n, rtv, dsv));
    if (t) set_ratio(c, r);
}

static void STDMETHODCALLTYPE hk_OMSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                            ID3D11DepthStencilView *dsv, UINT us, UINT un,
                                            ID3D11UnorderedAccessView *const *uav, const UINT *ic)
{
    Ratio r;
    int t = 0;
    ID3D11RenderTargetView *tmp[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        rtv = world_rtv(c, n, rtv, dsv, tmp, __builtin_return_address(0));
        t = targets_ratio(n, rtv, dsv, &r);
        dsv = fit_dsv(n, rtv, dsv);
    }
    ORIGC(c, OMSetRenderTargetsAndUnorderedAccessViews, PFN_OMUAV)(RC(c), n, rtv, dsv, us, un, uav, ic);
    if (!t && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && uav)
        for (UINT i = 0; i < un; i++)
            if (uav[i]) { r = view_ratio((ID3D11View *)uav[i]); t = 1; break; }
    if (t) set_ratio(c, r);
}

static void STDMETHODCALLTYPE hk_ClearState(ID3D11DeviceContext *c)
{
    ORIGC(c, ClearState, PFN_CLEAR)(RC(c));
    reset(c);
}

/* restore=FALSE: после исполнения/записи списка состояние контекста сброшено в умолчания */
static void STDMETHODCALLTYPE hk_ExecuteCommandList(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL restore)
{
    if (g_cap) diag_note(c, "ExecuteCommandList %06x", (unsigned)((ULONG_PTR)l & 0xffffff));
    ORIGC(c, ExecuteCommandList, PFN_ECL)(RC(c), l, restore);
    cb_ctx_forget(c);   /* список мог переписать буферы */
    if (!restore) reset(c);
}

static HRESULT STDMETHODCALLTYPE hk_FinishCommandList(ID3D11DeviceContext *c, BOOL restore, ID3D11CommandList **l)
{
    HRESULT hr = ORIGC(c, FinishCommandList, PFN_FCL)(RC(c), restore, l);
    if (g_cap && l) diag_note(c, "FinishCommandList %06x", (unsigned)((ULONG_PTR)*l & 0xffffff));
    cb_ctx_forget(c);   /* новый список начинается без содержимого буферов */
    if (!restore) reset(c);
    return hr;
}

void vp_install(void)
{
    CTX_HOOK(RSSetViewports, hk_RSSetViewports);
    CTX_HOOK(RSSetScissorRects, hk_RSSetScissorRects);
    CTX_HOOK(RSGetViewports, hk_RSGetViewports);
    CTX_HOOK(RSGetScissorRects, hk_RSGetScissorRects);
    CTX_HOOK(OMSetRenderTargets, hk_OMSetRenderTargets);
    CTX_HOOK(OMSetRenderTargetsAndUnorderedAccessViews, hk_OMSetRTUAV);
    CTX_HOOK(ClearState, hk_ClearState);
    CTX_HOOK(ExecuteCommandList, hk_ExecuteCommandList);
    CTX_HOOK(FinishCommandList, hk_FinishCommandList);
}
