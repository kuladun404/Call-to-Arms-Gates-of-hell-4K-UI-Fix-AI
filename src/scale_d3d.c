/* РњР°СЃС€С‚Р°Р±РёСЂРѕРІР°РЅРёРµ СЂРµРЅРґРµСЂР°: swapchain Рё РІСЃРµ В«СЌРєСЂР°РЅРЅС‹РµВ» СЂРµРЅРґРµСЂ-С‚Р°СЂРіРµС‚С‹ С„РёР·РёС‡РµСЃРєРё Г—S,
 * Р° РёРіСЂРµ С‡РµСЂРµР· GetDesc/RSGetViewports РїРѕРєР°Р·С‹РІР°РµС‚СЃСЏ Р»РѕРіРёС‡РµСЃРєРёР№ СЂР°Р·РјРµСЂ.
 * РњР°СЃС€С‚Р°Р±РёСЂРѕРІР°РЅРЅС‹Рµ СЂРµСЃСѓСЂСЃС‹ РїРѕРјРµС‡Р°СЋС‚СЃСЏ private data (MARK). */
#include "common.h"

static const GUID MARK = {0x5a1e4b2c, 0x7d31, 0x4c8e, {0x9b, 0x10, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99}};

static int is_marked(ID3D11Resource *r)
{
    if (!r) return 0;
    UINT sz = sizeof(DWORD);
    DWORD v = 0;
    return SUCCEEDED(ID3D11Resource_GetPrivateData(r, &MARK, &sz, &v)) && v == 1;
}

static void mark(ID3D11Resource *r)
{
    DWORD v = 1;
    ID3D11Resource_SetPrivateData(r, &MARK, sizeof v, &v);
}

static int view_marked(ID3D11View *v)
{
    if (!v) return 0;
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    int m = is_marked(r);
    if (r) ID3D11Resource_Release(r);
    return m;
}

void d3d_set_logical(UINT w, UINT h)
{
    if (w && h && ((UINT)g_lw != w || (UINT)g_lh != h)) {
        InterlockedExchange(&g_lw, (LONG)w);
        InterlockedExchange(&g_lh, (LONG)h);
        LOG("logical screen = %ux%u, physical = %ux%u", w, h, w * g_scale, h * g_scale);
    }
}

static int screen_match(UINT w, UINT h)
{
    UINT lw = (UINT)g_lw, lh = (UINT)g_lh;
    if (!lw || !lh) return 0;
    for (UINT k = 1; k <= 16; k *= 2) {
        UINT wf = lw / k, wc = (lw + k - 1) / k, hf = lh / k, hc = (lh + k - 1) / k;
        if ((w == wf || w == wc) && (h == hf || h == hc)) return 1;
    }
    return 0;
}

static int pow2(UINT v) { return v && !(v & (v - 1)); }

/* РїСЂРѕС‡РёРµ СЂРµРЅРґРµСЂ-С‚Р°СЂРіРµС‚С‹ (РєСЌС€Рё РІРёРґР¶РµС‚РѕРІ UI РїСЂРѕРёР·РІРѕР»СЊРЅРѕРіРѕ СЂР°Р·РјРµСЂР°): РЅРµ РєРІР°РґСЂР°С‚ Рё РЅРµ СЃС‚РµРїРµРЅСЊ РґРІРѕР№РєРё */
static int ui_rt(const D3D11_TEXTURE2D_DESC *d)
{
    return g_scale_rt && (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) && d->Usage == D3D11_USAGE_DEFAULT &&
           d->ArraySize == 1 && d->SampleDesc.Count == 1 && d->Width != d->Height &&
           !(pow2(d->Width) && pow2(d->Height)) && d->Width <= 4096 && d->Height <= 4096;
}

static UINT full_mips(UINT w, UINT h)
{
    UINT m = 1, s = w > h ? w : h;
    while (s > 1) { s >>= 1; m++; }
    return m;
}

/* =================== оверлей Steam ===================
 * GameOverlayRenderer64.dll рисует поверх игры через то же устройство и контекст. Ему нужны настоящие размеры
 * (он сам масштабируется по DPI); если отдать ему логические 1920×1080 и увеличить вывод ×2, он станет огромным.
 * Вызовы из его кода узнаём по адресу возврата и пропускаем без пересчёта. */
static int from_overlay(void *ra)
{
    static BYTE *base;
    static SIZE_T size;
    static DWORD last;
    if (!base) {
        DWORD now = GetTickCount();
        if (last && now - last < 2000) return 0;
        last = now ? now : 1;
        HMODULE m = GetModuleHandleW(L"GameOverlayRenderer64.dll");
        if (!m) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
        base = (BYTE *)m;
        LOG("Steam overlay at %p (+0x%zx): its D3D calls pass unscaled", base, size);
    }
    return (BYTE *)ra >= base && (BYTE *)ra < base + size;
}
#define OVERLAY_CALL() from_overlay(__builtin_return_address(0))

/* =================== ID3D11Texture2D =================== */
static VtHook h_tex_getdesc;
typedef void(STDMETHODCALLTYPE *PFN_TexGetDesc)(ID3D11Texture2D *, D3D11_TEXTURE2D_DESC *);

static void STDMETHODCALLTYPE hk_TexGetDesc(ID3D11Texture2D *t, D3D11_TEXTURE2D_DESC *d)
{
    VT_ORIG(t, &h_tex_getdesc, PFN_TexGetDesc)(t, d);
    if (d && is_marked((ID3D11Resource *)t) && !OVERLAY_CALL()) {
        d->Width /= g_scale;
        d->Height /= g_scale;
    }
}

static void hook_texture(ID3D11Texture2D *t)
{
    VT_PATCH(t, ID3D11Texture2DVtbl, GetDesc, hk_TexGetDesc, &h_tex_getdesc);
}

void d3d_mark_scaled(ID3D11Texture2D *t)
{
    mark((ID3D11Resource *)t);
    hook_texture(t);
}

/* =================== ID3D11Device =================== */
static VtHook h_ct2d;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CT2D)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                             const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d,
                                                    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    PFN_CT2D orig = VT_ORIG(dev, &h_ct2d, PFN_CT2D);
    HRESULT fhr;
    if (g_scale > 1 && fonthd_create(dev, (PFN_CT2D_)orig, d, init, out, &fhr)) return fhr;
    const UINT gpu = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
    if (d && g_scale > 1 && !init &&
        ((screen_match(d->Width, d->Height) && ((d->BindFlags & gpu) || d->Usage == D3D11_USAGE_STAGING)) ||
         ui_rt(d))) {
        D3D11_TEXTURE2D_DESC n = *d;
        n.Width *= g_scale;
        n.Height *= g_scale;
        if (n.MipLevels == 0) n.MipLevels = full_mips(d->Width, d->Height);
        HRESULT hr = orig(dev, &n, NULL, out);
        LOG("tex2D x%d: %ux%u fmt=%d bind=0x%x usage=%d cpu=0x%x mips=%u arr=%u ms=%u -> hr=0x%08lx %p",
            g_scale, d->Width, d->Height, d->Format, d->BindFlags, d->Usage, d->CPUAccessFlags,
            d->MipLevels, d->ArraySize, d->SampleDesc.Count, hr, out ? *out : NULL);
        if (SUCCEEDED(hr) && out && *out) {
            mark((ID3D11Resource *)*out);
            hook_texture(*out);
        }
        return hr;
    }
    HRESULT hr = orig(dev, d, init, out);
    if (d && (d->BindFlags & gpu))
        LOGV("tex2D keep: %ux%u fmt=%d bind=0x%x usage=%d init=%d -> %p", d->Width, d->Height, d->Format,
             d->BindFlags, d->Usage, init != NULL, out ? *out : NULL);
    return hr;
}

/* =================== ID3D11DeviceContext =================== */
typedef struct { void **vtbl; ID3D11DeviceContext *real; volatile LONG ref; } CtxWrap;
#define RC(c) (((CtxWrap *)(c))->real)
#define ORIGC(c, field, PFN) ((PFN)(RC(c)->lpVtbl->field))
typedef struct {
    ID3D11DeviceContext *ctx;
    int scaled;
    UINT nvp, nsc;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    struct { ID3D11Resource *r; float *p; UINT n; } cbm[8]; /* РѕС‚РєСЂС‹С‚С‹Рµ Map РєРѕРЅСЃС‚Р°РЅС‚РЅС‹С… Р±СѓС„РµСЂРѕРІ */
} CtxState;

#define NCTX 128
static CtxState g_ctx[NCTX];
static SRWLOCK g_ctxlock = SRWLOCK_INIT;

static CtxState *st_of(ID3D11DeviceContext *c)
{
    for (int i = 0; i < NCTX; i++)
        if (g_ctx[i].ctx == c) return &g_ctx[i];
    AcquireSRWLockExclusive(&g_ctxlock);
    CtxState *r = NULL;
    int idx = -1;
    for (int i = 0; i < NCTX && !r; i++)
        if (g_ctx[i].ctx == c) r = &g_ctx[i];
    for (int i = 0; i < NCTX && !r; i++)
        if (!g_ctx[i].ctx) { r = &g_ctx[i]; ZeroMemory(r, sizeof *r); r->ctx = c; idx = i; }
    ReleaseSRWLockExclusive(&g_ctxlock);
    if (idx >= 0) LOG("new context %p (type %d) slot %d", c, ID3D11DeviceContext_GetType(c), idx);
    if (!r) { static int once; if (!once++) LOG("context table full!"); r = &g_ctx[NCTX - 1]; }
    return r;
}

static void st_free(ID3D11DeviceContext *c)
{
    AcquireSRWLockExclusive(&g_ctxlock);
    for (int i = 0; i < NCTX; i++)
        if (g_ctx[i].ctx == c) g_ctx[i].ctx = NULL;
    ReleaseSRWLockExclusive(&g_ctxlock);
}

typedef void(STDMETHODCALLTYPE *PFN_VP)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *PFN_SC)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
typedef void(STDMETHODCALLTYPE *PFN_GVP)(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *PFN_GSC)(ID3D11DeviceContext *, UINT *, D3D11_RECT *);
typedef void(STDMETHODCALLTYPE *PFN_OM)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                       ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *PFN_OMUAV)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                          ID3D11DepthStencilView *, UINT, UINT,
                                          ID3D11UnorderedAccessView *const *, const UINT *);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR)(ID3D11DeviceContext *);
typedef void(STDMETHODCALLTYPE *PFN_CSR)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                        ID3D11Resource *, UINT, const D3D11_BOX *);
typedef void(STDMETHODCALLTYPE *PFN_CR)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_MAP)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *PFN_UPD)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                        const void *, UINT, UINT);

/* ---- РґР°РјРї РѕРґРЅРѕРіРѕ РєР°РґСЂР°: СЃРѕР·РґР°Р№С‚Рµ С„Р°Р№Р» ui4k.dump СЂСЏРґРѕРј СЃ DLL ---- */
static volatile LONG g_dump;
#define DUMP(...) do { if (g_dump) ui4k_log(__VA_ARGS__); } while (0)

static void res_info(ID3D11View *v, char *buf, size_t n)
{
    buf[0] = 0;
    if (!v) { lstrcpynA(buf, "-", (int)n); return; }
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        wsprintfA(buf, "%p %ux%u f%d%s", r, d.Width, d.Height, d.Format, is_marked(r) ? " X2" : "");
    } else
        wsprintfA(buf, "%p dim%d", r, dim);
    ID3D11Resource_Release(r);
}

static void diag_vp(ID3D11DeviceContext *c, CtxState *s)
{
    if (g_log < 2 || !s->nvp) return;
    static ULONG64 seen[512];
    static LONG nseen;
    ULONG64 key = ((ULONG64)(ULONG_PTR)c << 20) ^ ((ULONG64)s->scaled << 63) ^
                  ((ULONG64)(UINT)s->vp[0].Width << 32) ^ (ULONG64)(UINT)s->vp[0].Height ^
                  ((ULONG64)(UINT)s->vp[0].TopLeftX << 48);
    LONG n = nseen;
    for (LONG i = 0; i < n && i < 512; i++)
        if (seen[i] == key) return;
    if (n >= 512) return;
    seen[InterlockedIncrement(&nseen) - 1] = key;
    LOG("vp ctx=%p scaled=%d n=%u [%g,%g %gx%g]", c, s->scaled, s->nvp, s->vp[0].TopLeftX, s->vp[0].TopLeftY,
        s->vp[0].Width, s->vp[0].Height);
}

static void apply_vp(ID3D11DeviceContext *c, CtxState *s)
{
    diag_vp(c, s);
    D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    float k = s->scaled ? (float)g_scale : 1.0f;
    for (UINT i = 0; i < s->nvp; i++) {
        v[i] = s->vp[i];
        v[i].TopLeftX *= k; v[i].TopLeftY *= k; v[i].Width *= k; v[i].Height *= k;
    }
    ORIGC(c, RSSetViewports, PFN_VP)(RC(c), s->nvp, s->nvp ? v : NULL);
}

static void apply_sc(ID3D11DeviceContext *c, CtxState *s)
{
    D3D11_RECT r[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    LONG k = s->scaled ? g_scale : 1;
    for (UINT i = 0; i < s->nsc; i++) {
        r[i].left = s->sc[i].left * k; r[i].top = s->sc[i].top * k;
        r[i].right = s->sc[i].right * k; r[i].bottom = s->sc[i].bottom * k;
    }
    ORIGC(c, RSSetScissorRects, PFN_SC)(RC(c), s->nsc, s->nsc ? r : NULL);
}

static void set_scaled(ID3D11DeviceContext *c, int scaled)
{
    CtxState *s = st_of(c);
    if (s->scaled == scaled) return;
    s->scaled = scaled;
    if (s->nvp) apply_vp(c, s);
    if (s->nsc) apply_sc(c, s);
}

static int targets_scaled(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
    for (UINT i = 0; i < n && rtv; i++)
        if (rtv[i]) return view_marked((ID3D11View *)rtv[i]);
    if (dsv) return view_marked((ID3D11View *)dsv);
    return -1; /* С†РµР»РµР№ РЅРµС‚ */
}

static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v)
{
    if (OVERLAY_CALL()) { ORIGC(c, RSSetViewports, PFN_VP)(RC(c), n, v); return; }
    CtxState *s = st_of(c);
    if (g_dump) DUMP("  %p RSSetViewports n=%u [%d,%d %dx%d] scaled=%d", c, n, v && n ? (int)v[0].TopLeftX : -1,
                     v && n ? (int)v[0].TopLeftY : -1, v && n ? (int)v[0].Width : -1, v && n ? (int)v[0].Height : -1,
                     s->scaled);
    if (n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    s->nvp = v ? n : 0;
    if (v) memcpy(s->vp, v, n * sizeof *v);
    apply_vp(c, s);
}

static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{
    if (OVERLAY_CALL()) { ORIGC(c, RSSetScissorRects, PFN_SC)(RC(c), n, r); return; }
    CtxState *s = st_of(c);
    if (n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    s->nsc = r ? n : 0;
    if (r) memcpy(s->sc, r, n * sizeof *r);
    apply_sc(c, s);
}

static void STDMETHODCALLTYPE hk_RSGetViewports(ID3D11DeviceContext *c, UINT *pn, D3D11_VIEWPORT *v)
{
    if (OVERLAY_CALL()) { ORIGC(c, RSGetViewports, PFN_GVP)(RC(c), pn, v); return; }
    CtxState *s = st_of(c);
    if (!pn) return;
    if (!v) { *pn = s->nvp; return; }
    UINT n = *pn;
    for (UINT i = 0; i < n; i++) {
        if (i < s->nvp) v[i] = s->vp[i];
        else ZeroMemory(&v[i], sizeof v[i]);
    }
}

static void STDMETHODCALLTYPE hk_RSGetScissorRects(ID3D11DeviceContext *c, UINT *pn, D3D11_RECT *r)
{
    if (OVERLAY_CALL()) { ORIGC(c, RSGetScissorRects, PFN_GSC)(RC(c), pn, r); return; }
    CtxState *s = st_of(c);
    if (!pn) return;
    if (!r) { *pn = s->nsc; return; }
    UINT n = *pn;
    for (UINT i = 0; i < n; i++) {
        if (i < s->nsc) r[i] = s->sc[i];
        else ZeroMemory(&r[i], sizeof r[i]);
    }
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n,
                                                    ID3D11RenderTargetView *const *rtv,
                                                    ID3D11DepthStencilView *dsv)
{
    ORIGC(c, OMSetRenderTargets, PFN_OM)(RC(c), n, rtv, dsv);
    int t = targets_scaled(n, rtv, dsv);
    if (g_dump) {
        char a[96], b[96];
        res_info(n && rtv ? (ID3D11View *)rtv[0] : NULL, a, sizeof a);
        res_info((ID3D11View *)dsv, b, sizeof b);
        DUMP("  %p OMSetRenderTargets n=%u rt0=[%s] ds=[%s] -> scaled=%d", c, n, a, b, t);
    }
    if (t >= 0) set_scaled(c, t);
}

static void STDMETHODCALLTYPE hk_OMSetRTUAV(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                            ID3D11DepthStencilView *dsv, UINT us, UINT un,
                                            ID3D11UnorderedAccessView *const *uav, const UINT *ic)
{
    ORIGC(c, OMSetRenderTargetsAndUnorderedAccessViews, PFN_OMUAV)(RC(c), n, rtv, dsv, us, un, uav, ic);
    int t = -1;
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) t = targets_scaled(n, rtv, dsv);
    if (t < 0 && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS && uav)
        for (UINT i = 0; i < un; i++)
            if (uav[i]) { t = view_marked((ID3D11View *)uav[i]); break; }
    if (t >= 0) set_scaled(c, t);
}

static void STDMETHODCALLTYPE hk_ClearState(ID3D11DeviceContext *c)
{
    ORIGC(c, ClearState, PFN_CLEAR)(RC(c));
    CtxState *s = st_of(c);
    s->scaled = 0; s->nvp = 0; s->nsc = 0;
}

static void STDMETHODCALLTYPE hk_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub,
                                                       UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub,
                                                       const D3D11_BOX *box)
{
    int ms = is_marked(src), md = is_marked(dst);
    D3D11_BOX b;
    if (box && ms) {
        b = *box;
        b.left *= g_scale; b.right *= g_scale; b.top *= g_scale; b.bottom *= g_scale;
        box = &b;
    }
    if (md) { x *= g_scale; y *= g_scale; }
    if (ms != md) {
        static int once;
        if (once++ < 20) LOG("CopySubresourceRegion mixed: src marked=%d dst marked=%d box=%d", ms, md, box != NULL);
    }
    ORIGC(c, CopySubresourceRegion, PFN_CSR)(RC(c), dst, dsub, x, y, z, src, ssub, box);
}

static void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    int ms = is_marked(src), md = is_marked(dst);
    if (ms != md) {
        static int once;
        if (once++ < 20) LOG("CopyResource MISMATCH: src marked=%d (%p) dst marked=%d (%p)", ms, src, md, dst);
    }
    ORIGC(c, CopyResource, PFN_CR)(RC(c), dst, src);
}

/* ---- РїРѕРёСЃРє СЌРєСЂР°РЅРЅС‹С… РєРѕРЅСЃС‚Р°РЅС‚ РІ РєРѕРЅСЃС‚Р°РЅС‚РЅС‹С… Р±СѓС„РµСЂР°С… ---- */
static int feq(float a, float b) { float d = a - b; if (d < 0) d = -d; float m = b < 0 ? -b : b; return d <= m * 1e-4f + 1e-9f; }

static const struct { float kw, kh; int inv; const char *name; } P[] = {
    {1, 1, 0, "W,H"}, {1, 1, 1, "1/W,1/H"}, {0.5f, 0.5f, 0, "W/2,H/2"}, {0.5f, 0.5f, 1, "2/W,2/H"},
    {2, 2, 1, "0.5/W,0.5/H"}, {0.25f, 0.25f, 0, "W/4,H/4"}, {0.25f, 0.25f, 1, "4/W,4/H"},
};

/* СЂР°Р·РјРµСЂС‹ (РІ float) РєРѕРЅСЃС‚Р°РЅС‚РЅС‹С… Р±СѓС„РµСЂРѕРІ СЃС†РµРЅС‹, РІ РєРѕС‚РѕСЂС‹С… СЌРєСЂР°РЅРЅС‹Рµ РІРµР»РёС‡РёРЅС‹ РЅР°РґРѕ СЃРґРµР»Р°С‚СЊ С„РёР·РёС‡РµСЃРєРёРјРё */
static int cb_fix_size(UINT n)
{
    for (int i = 0; i < g_ncbfix; i++)
        if (g_cbfix[i] == (int)n) return 1;
    return 0;
}

static void cb_fix(float *f, UINT n)
{
    float W = (float)g_lw, H = (float)g_lh;
    if (!W) return;
    for (UINT i = 0; i + 1 < n; i++)
        for (size_t k = 0; k < sizeof P / sizeof P[0]; k++) {
            float a = W * P[k].kw, b = H * P[k].kh;
            if (P[k].inv) { a = 1.0f / a; b = 1.0f / b; }
            if (feq(f[i], a) && feq(f[i + 1], b)) {
                float m = P[k].inv ? 1.0f / g_scale : (float)g_scale;
                f[i] *= m;
                f[i + 1] *= m;
                i++;
                break;
            }
        }
}

static void cb_scan(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n, const char *how)
{
    float W = (float)g_lw, H = (float)g_lh;
    if (!W || n < 2) return;
    for (UINT i = 0; i + 1 < n; i++) {
        for (size_t k = 0; k < sizeof P / sizeof P[0]; k++) {
            float a = W * P[k].kw, b = H * P[k].kh;
            if (P[k].inv) { a = 1.0f / a; b = 1.0f / b; }
            if (!(feq(f[i], a) && feq(f[i + 1], b))) continue;
            static ULONG64 seen[1024];
            static LONG ns;
            ULONG64 key = ((ULONG64)(ULONG_PTR)r << 16) ^ ((ULONG64)i << 4) ^ k;
            LONG cnt = ns;
            int dup = 0;
            for (LONG j = 0; j < cnt && j < 1024; j++) if (seen[j] == key) { dup = 1; break; }
            if (!dup && cnt < 1024) {
                seen[InterlockedIncrement(&ns) - 1] = key;
                CtxState *st = st_of(c);
                LOG("CB %s %p float[%u]=%s (%g,%g) n=%u ctx=%p scaledRT=%d", how, r, i, P[k].name, f[i], f[i + 1], n, c,
                    st->scaled);
            }
        }
    }
}

static int is_cb(ID3D11Resource *r, UINT *bytes)
{
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    *bytes = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

static HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT f,
                                        D3D11_MAPPED_SUBRESOURCE *m)
{
    if (is_marked(r)) {
        static int once;
        if (once++ < 20) LOG("Map of scaled resource %p type=%d", r, t);
    }
    HRESULT hr = ORIGC(c, Map, PFN_MAP)(RC(c), r, sub, t, f, m);
    UINT bytes;
    if ((g_scan_cb || g_ncbfix) && SUCCEEDED(hr) && m && m->pData && (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE ||
        t == D3D11_MAP_WRITE_NO_OVERWRITE) && is_cb(r, &bytes)) {
        CtxState *s = st_of(c);
        for (int i = 0; i < 8; i++)
            if (!s->cbm[i].r) { s->cbm[i].r = r; s->cbm[i].p = (float *)m->pData; s->cbm[i].n = bytes / 4; break; }
    }
    return hr;
}

typedef void(STDMETHODCALLTYPE *PFN_UNMAP)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub)
{
    if (g_scan_cb || g_ncbfix) {
        CtxState *s = st_of(c);
        for (int i = 0; i < 8; i++)
            if (s->cbm[i].r == r) {
                if (g_scan_cb) cb_scan(c, r, s->cbm[i].p, s->cbm[i].n, "map");
                if (cb_fix_size(s->cbm[i].n)) cb_fix(s->cbm[i].p, s->cbm[i].n);
                s->cbm[i].r = NULL;
                break;
            }
    }
    ORIGC(c, Unmap, PFN_UNMAP)(RC(c), r, sub);
}

static void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                                   const D3D11_BOX *box, const void *data, UINT rp, UINT dp)
{
    if (is_marked(r)) {
        static int once;
        if (once++ < 20) LOG("UpdateSubresource of scaled resource %p box=%d", r, box != NULL);
    }
    UINT bytes;
    float tmp[4096];
    if ((g_scan_cb || g_ncbfix) && data && !box && is_cb(r, &bytes)) {
        if (g_scan_cb) cb_scan(c, r, (const float *)data, bytes / 4, "upd");
        if (bytes <= sizeof tmp && cb_fix_size(bytes / 4)) {
            memcpy(tmp, data, bytes);
            cb_fix(tmp, bytes / 4);
            data = tmp;
        }
    }
    if (g_scale > 1 && fonthd_update(RC(c), (PFN_UPD_)ORIGC(c, UpdateSubresource, PFN_UPD), r, sub, box, data, rp))
        return;
    ORIGC(c, UpdateSubresource, PFN_UPD)(RC(c), r, sub, box, data, rp, dp);
}

typedef void(STDMETHODCALLTYPE *PFN_DRAW)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DRAWI)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *PFN_DRAWII)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DRAWIN)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DISP)(ID3D11DeviceContext *, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_CLRTV)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
static void dump_draw(ID3D11DeviceContext *c, const char *what, UINT n)
{
    CtxState *s = st_of(c);
    DUMP("  %p %s %u  scaled=%d vp=%dx%d", c, what, n, s->scaled, s->nvp ? (int)s->vp[0].Width : -1,
         s->nvp ? (int)s->vp[0].Height : -1);
}
static void STDMETHODCALLTYPE hk_Draw(ID3D11DeviceContext *c, UINT a, UINT b)
{ if (g_dump) dump_draw(c, "Draw", a); ORIGC(c, Draw, PFN_DRAW)(RC(c), a, b); }
static void STDMETHODCALLTYPE hk_DrawIndexed(ID3D11DeviceContext *c, UINT a, UINT b, INT d)
{ if (g_dump) dump_draw(c, "DrawIndexed", a); ORIGC(c, DrawIndexed, PFN_DRAWI)(RC(c), a, b, d); }
static void STDMETHODCALLTYPE hk_DrawIndexedInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, INT e, UINT f)
{ if (g_dump) dump_draw(c, "DrawIndexedInstanced", a); ORIGC(c, DrawIndexedInstanced, PFN_DRAWII)(RC(c), a, b, d, e, f); }
static void STDMETHODCALLTYPE hk_DrawInstanced(ID3D11DeviceContext *c, UINT a, UINT b, UINT d, UINT e)
{ if (g_dump) dump_draw(c, "DrawInstanced", a); ORIGC(c, DrawInstanced, PFN_DRAWIN)(RC(c), a, b, d, e); }
static void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z)
{ DUMP("  %p Dispatch %u %u %u", c, x, y, z); ORIGC(c, Dispatch, PFN_DISP)(RC(c), x, y, z); }
static void STDMETHODCALLTYPE hk_ClearRTV(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT col[4])
{
    if (g_dump) { char a[96]; res_info((ID3D11View *)v, a, sizeof a); DUMP("  %p ClearRTV [%s] (%.2f %.2f %.2f %.2f)", c, a, col[0], col[1], col[2], col[3]); }
    ORIGC(c, ClearRenderTargetView, PFN_CLRTV)(RC(c), v, col);
}

typedef void(STDMETHODCALLTYPE *PFN_ECL)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FCL)(ID3D11DeviceContext *, BOOL, ID3D11CommandList **);
static void STDMETHODCALLTYPE hk_ExecuteCommandList(ID3D11DeviceContext *c, ID3D11CommandList *l, BOOL restore)
{
    static int n;
    if (n++ < 5) LOG("ExecuteCommandList ctx=%p restore=%d", c, restore);
    DUMP("  %p ExecuteCommandList %p restore=%d", c, l, restore);
    ORIGC(c, ExecuteCommandList, PFN_ECL)(RC(c), l, restore);
    if (!restore) { CtxState *s = st_of(c); s->scaled = 0; s->nvp = 0; s->nsc = 0; }
}
static HRESULT STDMETHODCALLTYPE hk_FinishCommandList(ID3D11DeviceContext *c, BOOL restore, ID3D11CommandList **l)
{
    static int n;
    if (n++ < 5) LOG("FinishCommandList ctx=%p restore=%d", c, restore);
    DUMP("  %p FinishCommandList restore=%d", c, restore);
    HRESULT hr = ORIGC(c, FinishCommandList, PFN_FCL)(RC(c), restore, l);
    if (!restore) { CtxState *s = st_of(c); s->scaled = 0; s->nvp = 0; s->nsc = 0; }
    return hr;
}

/* ---- РѕР±С‘СЂС‚РєР° РєРѕРЅС‚РµРєСЃС‚Р°: СЂР°РЅС‚Р°Р№Рј D3D11 РїРµСЂРµРїРёСЃС‹РІР°РµС‚ СЃРІРѕРё РІСЃС‚СЂРѕРµРЅРЅС‹Рµ vtable, РїРѕСЌС‚РѕРјСѓ
 * РїР°С‚С‡РёС‚СЊ СЃР»РѕС‚С‹ РЅРµР»СЊР·СЏ; РёРіСЂР° РїРѕР»СѓС‡Р°РµС‚ РЅР°С€ РѕР±СЉРµРєС‚, РІСЃРµ РјРµС‚РѕРґС‹ РєРѕС‚РѕСЂРѕРіРѕ РїРµСЂРµС…РѕРґРЅРёРєР°РјРё
 * (ctx_thunk_N РІ stubs.S) СѓС…РѕРґСЏС‚ РІ РЅР°СЃС‚РѕСЏС‰РёР№ РєРѕРЅС‚РµРєСЃС‚, Р° РЅСѓР¶РЅС‹Рµ вЂ” РІ РЅР°С€Рё С„СѓРЅРєС†РёРё. ---- */
extern void *g_ctx_thunks[];
#define CTX_SLOTS (sizeof(ID3D11DeviceContext4Vtbl) / sizeof(void *))
_Static_assert(CTX_SLOTS <= 192, "СѓРІРµР»РёС‡СЊС‚Рµ CTX_THUNKS РІ build.py");
static void *g_ctx_vt[CTX_SLOTS];
static CtxWrap *volatile g_imm;
static SRWLOCK g_immlock = SRWLOCK_INIT;
#define SETV(field, fn) g_ctx_vt[offsetof(ID3D11DeviceContext4Vtbl, field) / sizeof(void *)] = (void *)(fn)

static int ctx_iid(REFIID r)
{
    return IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_ID3D11DeviceChild) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext) || IsEqualIID(r, &IID_ID3D11DeviceContext1) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext2) || IsEqualIID(r, &IID_ID3D11DeviceContext3) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext4);
}

static ULONG STDMETHODCALLTYPE w_AddRef(ID3D11DeviceContext *c)
{
    return (ULONG)InterlockedIncrement(&((CtxWrap *)c)->ref);
}

static ULONG STDMETHODCALLTYPE w_Release(ID3D11DeviceContext *c)
{
    CtxWrap *w = (CtxWrap *)c;
    LONG r = InterlockedDecrement(&w->ref);
    if (r == 0) {
        AcquireSRWLockExclusive(&g_immlock);
        if (g_imm == w) g_imm = NULL;
        ReleaseSRWLockExclusive(&g_immlock);
        st_free(c);
        ID3D11DeviceContext_Release(w->real);
        HeapFree(GetProcessHeap(), 0, w);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE w_QI(ID3D11DeviceContext *c, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (ctx_iid(riid)) {
        void *t = NULL;
        HRESULT hr = ID3D11DeviceContext_QueryInterface(RC(c), riid, &t);
        if (FAILED(hr)) { *out = NULL; return hr; }
        IUnknown_Release((IUnknown *)t);
        w_AddRef(c);
        *out = c;
        return S_OK;
    }
    return ID3D11DeviceContext_QueryInterface(RC(c), riid, out);
}

static void build_ctx_vtbl(void)
{
    if (g_ctx_vt[0]) return;
    for (size_t i = 0; i < CTX_SLOTS; i++) g_ctx_vt[i] = g_ctx_thunks[i];
    SETV(AddRef, w_AddRef);
    SETV(Release, w_Release);
    SETV(RSSetViewports, hk_RSSetViewports);
    SETV(RSSetScissorRects, hk_RSSetScissorRects);
    SETV(RSGetViewports, hk_RSGetViewports);
    SETV(RSGetScissorRects, hk_RSGetScissorRects);
    SETV(OMSetRenderTargets, hk_OMSetRenderTargets);
    SETV(OMSetRenderTargetsAndUnorderedAccessViews, hk_OMSetRTUAV);
    SETV(ClearState, hk_ClearState);
    SETV(CopySubresourceRegion, hk_CopySubresourceRegion);
    SETV(CopyResource, hk_CopyResource);
    SETV(Map, hk_Map);
    SETV(Unmap, hk_Unmap);
    SETV(UpdateSubresource, hk_UpdateSubresource);
    SETV(Draw, hk_Draw);
    SETV(DrawIndexed, hk_DrawIndexed);
    SETV(DrawIndexedInstanced, hk_DrawIndexedInstanced);
    SETV(DrawInstanced, hk_DrawInstanced);
    SETV(Dispatch, hk_Dispatch);
    SETV(ClearRenderTargetView, hk_ClearRTV);
    SETV(ExecuteCommandList, hk_ExecuteCommandList);
    SETV(FinishCommandList, hk_FinishCommandList);
    SETV(QueryInterface, w_QI); /* РїРѕСЃР»РµРґРЅРёРј: g_ctx_vt[0] вЂ” С„Р»Р°Рі РіРѕС‚РѕРІРЅРѕСЃС‚Рё */
}

/* Р·Р°Р±РёСЂР°РµС‚ СЃСЃС‹Р»РєСѓ РЅР° real; РІРѕР·РІСЂР°С‰Р°РµС‚ РѕР±С‘СЂС‚РєСѓ СЃ ref=1 */
static ID3D11DeviceContext *ctx_wrap(ID3D11DeviceContext *real)
{
    static SRWLOCK once = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&once);
    build_ctx_vtbl();
    ReleaseSRWLockExclusive(&once);
    CtxWrap *w = (CtxWrap *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    w->vtbl = g_ctx_vt;
    w->real = real;
    w->ref = 1;
    LOG("context %p wrapped as %p (type %d)", real, w, ID3D11DeviceContext_GetType(real));
    return (ID3D11DeviceContext *)w;
}

/* РЅРµРїРѕСЃСЂРµРґСЃС‚РІРµРЅРЅС‹Р№ РєРѕРЅС‚РµРєСЃС‚ вЂ” РѕРґРЅР° РѕР±С‘СЂС‚РєР°; real РїСЂРёС…РѕРґРёС‚ СЃРѕ СЃСЃС‹Р»РєРѕР№, РєРѕС‚РѕСЂСѓСЋ Р·Р°Р±РёСЂР°РµРј */
ID3D11DeviceContext *d3d_wrap_immediate(ID3D11DeviceContext *real)
{
    if (!real) return NULL;
    AcquireSRWLockExclusive(&g_immlock);
    CtxWrap *w = g_imm;
    if (w && w->real == real) {
        InterlockedIncrement(&w->ref);
        ID3D11DeviceContext_Release(real);
    } else {
        w = (CtxWrap *)ctx_wrap(real);
        g_imm = w;
    }
    ReleaseSRWLockExclusive(&g_immlock);
    return (ID3D11DeviceContext *)w;
}

static VtHook h_cdc, h_gic, h_gic1, h_gic2, h_gic3, h_cdc1, h_cdc2, h_cdc3;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CDC)(void *, UINT, void **);
typedef void(STDMETHODCALLTYPE *PFN_GIC)(void *, void **);

#define DEF_CDC(name, hk)                                                                \
    static HRESULT STDMETHODCALLTYPE name(void *dev, UINT f, void **out)                 \
    {                                                                                    \
        HRESULT hr = VT_ORIG(dev, &hk, PFN_CDC)(dev, f, out);                            \
        if (SUCCEEDED(hr) && out && *out) *out = ctx_wrap((ID3D11DeviceContext *)*out);  \
        return hr;                                                                       \
    }
DEF_CDC(hk_CreateDeferredContext, h_cdc)
DEF_CDC(hk_CreateDeferredContext1, h_cdc1)
DEF_CDC(hk_CreateDeferredContext2, h_cdc2)
DEF_CDC(hk_CreateDeferredContext3, h_cdc3)

#define DEF_GIC(name, hk)                                                                \
    static void STDMETHODCALLTYPE name(void *dev, void **out)                            \
    {                                                                                    \
        VT_ORIG(dev, &hk, PFN_GIC)(dev, out);                                            \
        if (out && *out) *out = d3d_wrap_immediate((ID3D11DeviceContext *)*out);         \
    }
DEF_GIC(hk_GetImmediateContext, h_gic)
DEF_GIC(hk_GetImmediateContext1, h_gic1)
DEF_GIC(hk_GetImmediateContext2, h_gic2)
DEF_GIC(hk_GetImmediateContext3, h_gic3)

void d3d_hook_device(ID3D11Device *dev)
{
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateTexture2D, hk_CreateTexture2D, &h_ct2d);
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateDeferredContext, hk_CreateDeferredContext, &h_cdc);
    VT_PATCH(dev, ID3D11DeviceVtbl, GetImmediateContext, hk_GetImmediateContext, &h_gic);
    void *d = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, &d)) && d) {
        VT_PATCH(d, ID3D11Device1Vtbl, GetImmediateContext1, hk_GetImmediateContext1, &h_gic1);
        VT_PATCH(d, ID3D11Device1Vtbl, CreateDeferredContext1, hk_CreateDeferredContext1, &h_cdc1);
        IUnknown_Release((IUnknown *)d);
    }
    d = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device2, &d)) && d) {
        VT_PATCH(d, ID3D11Device2Vtbl, GetImmediateContext2, hk_GetImmediateContext2, &h_gic2);
        VT_PATCH(d, ID3D11Device2Vtbl, CreateDeferredContext2, hk_CreateDeferredContext2, &h_cdc2);
        IUnknown_Release((IUnknown *)d);
    }
    d = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, &d)) && d) {
        VT_PATCH(d, ID3D11Device3Vtbl, GetImmediateContext3, hk_GetImmediateContext3, &h_gic3);
        VT_PATCH(d, ID3D11Device3Vtbl, CreateDeferredContext3, hk_CreateDeferredContext3, &h_cdc3);
        IUnknown_Release((IUnknown *)d);
    }
    LOG("device %p hooked (context vtable %u slots)", dev, (unsigned)CTX_SLOTS);
}

/* =================== IDXGISwapChain =================== */
void d3d_scale_swapchain_desc(DXGI_SWAP_CHAIN_DESC *d)
{
    UINT w = d->BufferDesc.Width, h = d->BufferDesc.Height;
    if (!w || !h) {
        RECT r;
        if (d->OutputWindow && GetClientRect(d->OutputWindow, &r)) { /* СЂРµР°Р»СЊРЅС‹Р№ (С„РёР·РёС‡РµСЃРєРёР№) СЂР°Р·РјРµСЂ */
            w = (UINT)(r.right - r.left) / g_scale;
            h = (UINT)(r.bottom - r.top) / g_scale;
        }
        d3d_set_logical(w, h);
        return; /* 0 = DXGI СЃР°РјР° РІРѕР·СЊРјС‘С‚ С„РёР·РёС‡РµСЃРєРёР№ СЂР°Р·РјРµСЂ РѕРєРЅР° */
    }
    d3d_set_logical(w, h);
    d->BufferDesc.Width = w * g_scale;
    d->BufferDesc.Height = h * g_scale;
}

static VtHook h_present;
typedef HRESULT(STDMETHODCALLTYPE *PFN_PRESENT)(IDXGISwapChain *, UINT, UINT);
static WCHAR g_dumpfile[MAX_PATH];
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT si, UINT f)
{
    HRESULT hr = VT_ORIG(sc, &h_present, PFN_PRESENT)(sc, si, f);
    if (g_dump) { InterlockedExchange(&g_dump, 0); LOG("==== dump end ===="); }
    static UINT frame;
    if ((++frame & 31) == 0) {
        if (!g_dumpfile[0]) { lstrcpyW(g_dumpfile, g_dir); lstrcatW(g_dumpfile, L"ui4k.dump"); }
        if (GetFileAttributesW(g_dumpfile) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(g_dumpfile);
            LOG("==== dump begin (frame %u) ====", frame);
            InterlockedExchange(&g_dump, 1);
        }
    }
    return hr;
}

static VtHook h_sc_getbuf, h_sc_getdesc, h_sc_rb, h_sc_rt, h_sc_fs, h_f_csc;
typedef HRESULT(STDMETHODCALLTYPE *PFN_GETBUF)(IDXGISwapChain *, UINT, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SCDESC)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RB)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RT)(IDXGISwapChain *, const DXGI_MODE_DESC *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);

static HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out)
{
    HRESULT hr = VT_ORIG(sc, &h_sc_getbuf, PFN_GETBUF)(sc, i, riid, out);
    if (SUCCEEDED(hr) && out && *out && g_scale > 1) {
        ID3D11Texture2D *t = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_ID3D11Texture2D, (void **)&t)) && t) {
            mark((ID3D11Resource *)t);
            hook_texture(t);
            ID3D11Texture2D_Release(t);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SCGetDesc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d)
{
    HRESULT hr = VT_ORIG(sc, &h_sc_getdesc, PFN_SCDESC)(sc, d);
    if (SUCCEEDED(hr) && d && !OVERLAY_CALL()) {
        d->BufferDesc.Width /= g_scale;
        d->BufferDesc.Height /= g_scale;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f,
                                                  UINT fl)
{
    HRESULT hr;
    if (w && h) {
        d3d_set_logical(w, h);
        hr = VT_ORIG(sc, &h_sc_rb, PFN_RB)(sc, n, w * g_scale, h * g_scale, f, fl);
    } else {
        hr = VT_ORIG(sc, &h_sc_rb, PFN_RB)(sc, n, w, h, f, fl);
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(VT_ORIG(sc, &h_sc_getdesc, PFN_SCDESC)(sc, &d)))
            d3d_set_logical(d.BufferDesc.Width / g_scale, d.BufferDesc.Height / g_scale);
    }
    LOG("ResizeBuffers(%u, %ux%u) -> 0x%08lx", n, w, h, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeTarget(IDXGISwapChain *sc, const DXGI_MODE_DESC *m)
{
    DXGI_MODE_DESC d;
    if (m) {
        d = *m;
        d.Width *= g_scale;
        d.Height *= g_scale;
        LOG("ResizeTarget(%ux%u @%u/%u)", m->Width, m->Height, m->RefreshRate.Numerator, m->RefreshRate.Denominator);
        m = &d;
    }
    return VT_ORIG(sc, &h_sc_rt, PFN_RT)(sc, m);
}

static HRESULT STDMETHODCALLTYPE hk_SetFullscreenState(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *o)
{
    HRESULT hr = VT_ORIG(sc, &h_sc_fs, PFN_FS)(sc, fs, o);
    LOG("SetFullscreenState(%d) -> 0x%08lx", fs, hr);
    return hr;
}

void d3d_hook_swapchain(IDXGISwapChain *sc)
{
    VT_PATCH(sc, IDXGISwapChainVtbl, Present, hk_Present, &h_present);
    VT_PATCH(sc, IDXGISwapChainVtbl, GetBuffer, hk_GetBuffer, &h_sc_getbuf);
    VT_PATCH(sc, IDXGISwapChainVtbl, GetDesc, hk_SCGetDesc, &h_sc_getdesc);
    VT_PATCH(sc, IDXGISwapChainVtbl, ResizeBuffers, hk_ResizeBuffers, &h_sc_rb);
    VT_PATCH(sc, IDXGISwapChainVtbl, ResizeTarget, hk_ResizeTarget, &h_sc_rt);
    VT_PATCH(sc, IDXGISwapChainVtbl, SetFullscreenState, hk_SetFullscreenState, &h_sc_fs);
    LOG("swapchain %p hooked", sc);
}

static HRESULT STDMETHODCALLTYPE hk_FactoryCreateSwapChain(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d,
                                                           IDXGISwapChain **out)
{
    DXGI_SWAP_CHAIN_DESC n;
    if (g_cdsc_tid == GetCurrentThreadId()) /* РІС‹Р·РѕРІ РёР·РЅСѓС‚СЂРё D3D11CreateDeviceAndSwapChain: desc СѓР¶Рµ РјР°СЃС€С‚Р°Р±РёСЂРѕРІР°РЅ */
        return VT_ORIG(f, &h_f_csc, PFN_CSC)(f, dev, d, out);
    if (d) {
        LOG("IDXGIFactory::CreateSwapChain %ux%u windowed=%d hwnd=%p", d->BufferDesc.Width, d->BufferDesc.Height,
            d->Windowed, d->OutputWindow);
        n = *d;
        d3d_scale_swapchain_desc(&n);
        input_watch_window(d->OutputWindow);
        d = &n;
    }
    HRESULT hr = VT_ORIG(f, &h_f_csc, PFN_CSC)(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) d3d_hook_swapchain(*out);
    return hr;
}

void d3d_hook_factory(IDXGIFactory *f)
{
    VT_PATCH(f, IDXGIFactoryVtbl, CreateSwapChain, hk_FactoryCreateSwapChain, &h_f_csc);
}
