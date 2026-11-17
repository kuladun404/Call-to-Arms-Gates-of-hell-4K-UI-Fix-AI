/* F14/L8: мир в своём разрешении без HDR.
 * С HDR игра рисует мир в отдельную HDR-цель и переносит её на экран тонмаппингом — там сцена и экран разделены
 * форматом (resources.c). Без HDR игра рисует мир прямо в back buffer, туда же — эффекты и интерфейс.
 * Мод делает за игру то же, что HDR-путь: своя цель мира W (размер мира, формат back buffer):
 *  - от Present до первого рисования интерфейса привязка back buffer подменяется W (viewport, глубина и CB — по W);
 *  - чтения back buffer (Copy/Resolve) в это время идут из W, очистка чистит и W;
 *  - первое рисование интерфейса (VS интерфейса по контрольной сумме DXBC, тест глубины выключен) — W растягивается
 *    на back buffer, привязка возвращается к back buffer, дальше всё как есть;
 *  - кадр без интерфейса — W растягивается перед Present.
 * MSAA (П7): экран игры многовыборочный, глубина мира — тоже. W получает столько же выборок, сколько экран: глубина
 * игры подходит к ней как есть, чтения экрана сводят выборки W обычным ResolveSubresource. Перед растяжением на
 * экран выборки W сводятся в обычную текстуру того же размера (WF) — её и растягиваем. Размер мира от MSAA не
 * зависит, поэтому и включение MSAA на ходу его не меняет (Г1).
 * Включается сам: игра нарисовала в back buffer с тестом глубины (значит, HDR выключен) и мир не в размере экрана. */
#include <string.h>
#include "common.h"

typedef void(STDMETHODCALLTYPE *PFN_VSS)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DSS)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
typedef void(STDMETHODCALLTYPE *PFN_CRTV)(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT *);
typedef void(STDMETHODCALLTYPE *PFN_RS)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CVS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);

static ID3D11Texture2D *g_bb;                 /* back buffer — только сравнение указателей */
static ID3D11Texture2D *g_w;                  /* цель мира */
static ID3D11RenderTargetView *g_wv;
static ID3D11Texture2D *g_wf;                 /* MSAA: W со сведёнными выборками (обычная текстура того же размера) */
static UINT g_wlw, g_wlh;                     /* под что сделана W: экран игры, коэффициент мира, ... */
static Ratio g_wr;
static DXGI_FORMAT g_wfmt;                    /* ... формат и выборки back buffer */
static DXGI_SAMPLE_DESC g_wsd;
static DXGI_FORMAT g_bbfmt;                   /* формат и выборки текущего back buffer */
static DXGI_SAMPLE_DESC g_bbsd = {1, 0};
static volatile LONG g_hdroff;                /* видели мир в back buffer */
static int g_world = 1;                       /* 1 — идёт мир (подмена), 0 — интерфейс */
static ID3D11DeviceContext *g_imm;            /* обёртка immediate-контекста */
static ID3D11Resource *g_last_dst;            /* куда в прошлом кадре было последнее чтение экрана (только сравнение) */
static ID3D11Resource *g_cur_dst;             /* то же в этом кадре */
static int g_seen_last;                       /* в этом кадре уже прочитали экран в g_last_dst */
static int g_3d;                              /* в этом кадре уже был 3D-мир (с глубиной) в цели мира */

/* VS интерфейса: контрольные суммы DXBC (байты 4..11), найдено снимками без HDR 16.11 (меню, бой, пауза) */
static const BYTE UI_VS[][8] = {
    {0xb7, 0xe2, 0x4d, 0x53, 0x5a, 0x3a, 0x5d, 0x52},
    {0xce, 0x3d, 0x9b, 0xfe, 0xa0, 0xd2, 0xc0, 0x76},
};
/* С2: признак «VS интерфейса» — метка на самом объекте шейдера (private data), как у увеличенных текстур. Раньше была
 * таблица на 64 сырых адреса без очистки: после нескольких боёв новые объекты того же шейдера в неё не попадали
 * (интерфейс уходил в разрешение мира), а адрес удалённого шейдера мог достаться чужому.
 * {3d8f6a17-52c4-4e0b-a9d3-7e1f20b46c85} */
static const GUID GUID_UIVS = {0x3d8f6a17, 0x52c4, 0x4e0b, {0xa9, 0xd3, 0x7e, 0x1f, 0x20, 0xb4, 0x6c, 0x85}};

/* текущий VS контекста — интерфейса? Метку спрашиваем один раз на привязку и только когда это нужно */
static int is_ui_vs(CtxState *s)
{
    if (!s->vs) return 0;
    if (s->vs_ui < 0) {
        BYTE b = 0;
        UINT n = sizeof b;
        s->vs_ui = SUCCEEDED(ID3D11VertexShader_GetPrivateData(s->vs, &GUID_UIVS, &n, &b)) && n == sizeof b && b;
    }
    return s->vs_ui;
}

static int split(void) { return !ratio_eq(g_cfg.scene, g_cfg.scale); }
static int active(ID3D11DeviceContext *c) { return g_hdroff && split() && g_bb && CTXW(c)->type == D3D11_DEVICE_CONTEXT_IMMEDIATE; }

static int is_bb_view(ID3D11View *v)
{
    if (!v || !g_bb) return 0;
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    int m = r == (ID3D11Resource *)g_bb;
    if (r) ID3D11Resource_Release(r);
    return m;
}

void world_set_bb(ID3D11Texture2D *bb)
{
    g_bb = bb;
    if (!bb) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(bb, &d);
    g_bbfmt = d.Format;
    g_bbsd = d.SampleDesc;
}

/* HDR в настройках игры сменился на ходу (config_poll_profile) */
void world_hdr_changed(void)
{
    InterlockedExchange(&g_hdroff, g_cfg.game_hdr == 0);
    LOG("world: HDR is %s in the game settings now - %s", g_cfg.game_hdr ? "on" : "off",
        g_cfg.game_hdr ? "the game keeps the world in its own HDR target" : "own world target, screen textures at world size");
}

/* без HDR: экранные картинки игры — в размере мира (resources.c) */
int world_hdr_off(void) { return g_hdroff || g_cfg.game_hdr == 0; }

static void drop_w(void)
{
    if (g_wv) { ID3D11RenderTargetView_Release(g_wv); g_wv = NULL; }
    if (g_wf) { ID3D11Texture2D_Release(g_wf); g_wf = NULL; }
    if (g_w) { ID3D11Texture2D_Release(g_w); g_w = NULL; }
}

/* цель мира под текущий экран (размер, формат, выборки) и коэффициент мира */
static int ensure_w(void)
{
    Screen s = screen_get();
    if (!s.lw || !g_bb) return 0;
    /* та же цель. Не получилось создать — заново не пробуем, пока не сменится экран: мир рисуется в экран */
    if (g_wlw == (UINT)s.lw && g_wlh == (UINT)s.lh && ratio_eq(g_wr, g_cfg.scene) && g_wfmt == g_bbfmt &&
        g_wsd.Count == g_bbsd.Count && g_wsd.Quality == g_bbsd.Quality)
        return g_w != NULL;
    drop_w();
    g_wlw = (UINT)s.lw; g_wlh = (UINT)s.lh; g_wr = g_cfg.scene; g_wfmt = g_bbfmt; g_wsd = g_bbsd;
    /* П7: выборок столько же, сколько у экрана, — иначе глубина игры (она многовыборочная вместе с экраном) не подошла бы */
    D3D11_TEXTURE2D_DESC d = {(UINT)to_phys(g_cfg.scene, s.lw), (UINT)to_phys(g_cfg.scene, s.lh), 1, 1, g_bbfmt, g_bbsd,
                              D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Device *dev = NULL;
    ID3D11Texture2D_GetDevice(g_bb, &dev);
    HRESULT hr = dev ? res_create_raw(dev, &d, &g_w) : E_FAIL;
    if (SUCCEEDED(hr)) {
        res_mark(g_w, (UINT)s.lw, (UINT)s.lh, g_cfg.scene);
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)g_w, NULL, &g_wv);
    }
    if (SUCCEEDED(hr) && g_bbsd.Count > 1) {   /* куда сводить выборки перед растяжением на экран */
        D3D11_TEXTURE2D_DESC f = d;
        f.SampleDesc.Count = 1; f.SampleDesc.Quality = 0;
        hr = res_create_raw(dev, &f, &g_wf);
        if (SUCCEEDED(hr)) res_mark(g_wf, (UINT)s.lw, (UINT)s.lh, g_cfg.scene);
    }
    if (dev) ID3D11Device_Release(dev);
    LOG("world: own target %ux%u, samples %u (screen %ldx%ld, world %d/%d) -> 0x%08lx", d.Width, d.Height, g_bbsd.Count, s.lw,
        s.lh, g_cfg.scene.num, g_cfg.scene.den, (unsigned long)hr);
    if (FAILED(hr)) {
        drop_w();
        LOG("world: no own world target - the world is drawn straight into the screen (its depth does not fit there)");
        return 0;
    }
    return 1;
}

static void forget_orig(CtxState *s)
{
    for (UINT i = 0; i < s->norig; i++)
        if (s->orig[i]) { ID3D11RenderTargetView_Release(s->orig[i]); s->orig[i] = NULL; }
    if (s->odsv) { ID3D11DepthStencilView_Release(s->odsv); s->odsv = NULL; }
    s->norig = 0;
}

void world_reset(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    forget_orig(s);
    s->bbbound = s->bbsub = 0;
    s->vs = NULL;
    s->vs_ui = 0;
    s->zon = 1;   /* состояние глубины по умолчанию: тест включён */
    s->hasdsv = 0;
}

ID3D11RenderTargetView *const *world_rtv(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                         ID3D11DepthStencilView *dsv, ID3D11RenderTargetView **tmp, void *ret)
{
    CtxState *s = &CTXW(c)->st;
    if (CTXW(c)->type != D3D11_DEVICE_CONTEXT_IMMEDIATE || overlay_call(ret)) return rtv;
    g_imm = c;
    forget_orig(s);
    s->bbbound = s->bbsub = 0;
    s->hasdsv = dsv != NULL;
    if (n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) n = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    for (UINT i = 0; i < n && rtv; i++)
        if (is_bb_view((ID3D11View *)rtv[i])) s->bbbound = 1;
    if (!s->bbbound) return rtv;
    /* запомнить цели игры: при переходе к интерфейсу вернуть их */
    s->norig = n;
    for (UINT i = 0; i < n; i++) { s->orig[i] = rtv[i]; if (rtv[i]) ID3D11RenderTargetView_AddRef(rtv[i]); }
    s->odsv = dsv;
    if (dsv) ID3D11DepthStencilView_AddRef(dsv);
    if (!active(c) || !g_world || !ensure_w()) return rtv;
    for (UINT i = 0; i < n; i++) tmp[i] = is_bb_view((ID3D11View *)rtv[i]) ? g_wv : rtv[i];
    s->bbsub = 1;
    return tmp;
}

/* мир -> back buffer, привязка игры возвращается (через обёртку: viewport и CB — по back buffer) */
static void to_screen(ID3D11DeviceContext *c, const char *why)
{
    g_world = 0;
    if (!g_w || !g_bb) return;
    ID3D11Resource *from = (ID3D11Resource *)g_w;
    if (g_wf) {   /* MSAA: свести выборки мира, растянуть сведённое (в многовыборочный экран — во все выборки) */
        ORIGC(c, ResolveSubresource, PFN_RS)(RC(c), (ID3D11Resource *)g_wf, 0, (ID3D11Resource *)g_w, 0, g_wfmt);
        from = (ID3D11Resource *)g_wf;
    }
    if (!copy_stretch(RC(c), (ID3D11Resource *)g_bb, g_cfg.scale, 0, 0, from, NULL)) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) LOG("world: own target could NOT be drawn to the back buffer");
    }
    if (g_cap) diag_note(c, "world: own target -> back buffer (%s)", why);
    static const char *said[8];   /* каждая причина — один раз в обычный лог */
    for (int i = 0; i < 8; i++) {
        if (said[i] == why) break;
        if (!said[i]) { said[i] = why; LOG("world: own target -> back buffer (%s)", why); break; }
    }
    CtxState *s = &CTXW(c)->st;
    if (s->bbsub) {
        ID3D11RenderTargetView *o[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        UINT n = s->norig;
        ID3D11DepthStencilView *d = s->odsv;
        for (UINT i = 0; i < n; i++) { o[i] = s->orig[i]; if (o[i]) ID3D11RenderTargetView_AddRef(o[i]); }
        if (d) ID3D11DepthStencilView_AddRef(d);
        ID3D11DeviceContext_OMSetRenderTargets(c, n, o, d);
        for (UINT i = 0; i < n; i++) if (o[i]) ID3D11RenderTargetView_Release(o[i]);
        if (d) ID3D11DepthStencilView_Release(d);
    }
}

void world_pre_draw(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    if (CTXW(c)->type != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    if (!g_hdroff && s->bbbound && !s->bbsub && s->zon && s->hasdsv) {   /* мир прямо в back buffer: HDR выключен */
        InterlockedExchange(&g_hdroff, 1);
        LOG("world: game draws the 3D world straight into the back buffer (HDR off) - own world target from next frame");
    }
    /* интерфейс начинается после последнего чтения экрана в кадре (копия под размытие стеклянных панелей);
     * метки на земле тем же VS раньше — остаются в мире */
    if (s->bbsub && g_world && s->zon && s->hasdsv) g_3d = 1;
    /* меню, плашки, выход из боя: мира в кадре нет — ждать выученную копию экрана нечего */
    if (s->bbsub && g_world && !s->zon && is_ui_vs(s) && (!g_3d || !g_last_dst || g_seen_last))
        to_screen(c, !g_3d ? "interface, no 3D world in this frame"
                           : g_last_dst ? "interface after the last screen read" : "first interface draw");
}

/* чтение back buffer (Copy/Resolve): в мире — из цели мира; запоминаем, куда было последнее чтение в кадре */
ID3D11Resource *world_read(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    if (!src || src != (ID3D11Resource *)g_bb || !active(c)) return src;
    g_cur_dst = dst;
    if (dst == g_last_dst) g_seen_last = 1;
    return g_world && g_w ? (ID3D11Resource *)g_w : src;
}

/* перед ResizeBuffers: DXGI не пересоздаст back buffer, пока на его виды есть ссылки (наши копии целей игры) */
void world_release_bb(void)
{
    if (g_imm) { CtxState *s = &CTXW(g_imm)->st; forget_orig(s); s->bbbound = s->bbsub = 0; }
    g_bb = NULL;
}

void world_present(void)
{
    ID3D11DeviceContext *c = g_imm;
    if (c && g_world && g_w && active(c)) to_screen(c, "frame without interface");
    g_world = 1;
    g_last_dst = g_cur_dst; g_cur_dst = NULL; g_seen_last = 0; g_3d = 0;
    /* back buffer остался привязан с интерфейса — следующий кадр рисует мир сразу: снова подменить */
    if (c && active(c)) {
        CtxState *s = &CTXW(c)->st;
        if (s->bbbound && !s->bbsub && s->norig) {
            ID3D11RenderTargetView *o[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            UINT n = s->norig;
            ID3D11DepthStencilView *d = s->odsv;
            for (UINT i = 0; i < n; i++) { o[i] = s->orig[i]; if (o[i]) ID3D11RenderTargetView_AddRef(o[i]); }
            if (d) ID3D11DepthStencilView_AddRef(d);
            ID3D11DeviceContext_OMSetRenderTargets(c, n, o, d);
            for (UINT i = 0; i < n; i++) if (o[i]) ID3D11RenderTargetView_Release(o[i]);
            if (d) ID3D11DepthStencilView_Release(d);
        }
    }
}

/* ---- хуки контекста ---- */
static void STDMETHODCALLTYPE hk_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n)
{
    CTXW(c)->st.vs = vs;
    CTXW(c)->st.vs_ui = -1;
    ORIGC(c, VSSetShader, PFN_VSS)(RC(c), vs, ci, n);
}

static void STDMETHODCALLTYPE hk_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *d, UINT ref)
{
    int on = 1;
    if (d) { D3D11_DEPTH_STENCIL_DESC dd; ID3D11DepthStencilState_GetDesc(d, &dd); on = dd.DepthEnable != 0; }
    CTXW(c)->st.zon = on;
    ORIGC(c, OMSetDepthStencilState, PFN_DSS)(RC(c), d, ref);
}

static void STDMETHODCALLTYPE hk_ClearRenderTargetView(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT *col)
{
    if (g_cap) diag_view_note(c, "ClearRTV", (ID3D11View *)v);
    ORIGC(c, ClearRenderTargetView, PFN_CRTV)(RC(c), v, col);
    if (g_world && g_wv && active(c) && is_bb_view((ID3D11View *)v))
        ORIGC(c, ClearRenderTargetView, PFN_CRTV)(RC(c), g_wv, col);
}

static void STDMETHODCALLTYPE hk_ResolveSubresource(ID3D11DeviceContext *c, ID3D11Resource *d, UINT ds, ID3D11Resource *s,
                                                    UINT ss, DXGI_FORMAT f)
{
    ID3D11Resource *w = world_read(c, d, s);
    if (g_cap) diag_copy(c, "ResolveSubresource", d, w);
    /* настоящие размеры разные (цель мира и экран, многовыборочная и её пара, увеличенная и обычная) — мод
     * пересчитывает сам: сведение требует один размер, а D3D без отладочного слоя этого не проверяет */
    if (copy_resolve(c, d, ds, w, ss, f, w != s)) return;
    ORIGC(c, ResolveSubresource, PFN_RS)(RC(c), d, ds, w, ss, f);
}

/* ---- VS интерфейса ---- */
static VtHook h_cvs;
static HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(ID3D11Device *d, const void *code, SIZE_T n, ID3D11ClassLinkage *l,
                                                       ID3D11VertexShader **o)
{
    HRESULT hr = VT_ORIG(d, &h_cvs, PFN_CVS)(d, code, n, l, o);
    if (SUCCEEDED(hr) && o && *o && code && n > 12) {
        /* устойчивость к обновлениям: сначала — по именам из кэша шейдеров игры (anchors.c); отпечатки 16.11 — только
         * если имён в кэше нет; ui_vs из ui4k.ini — всегда */
        int ui = anchors_ui_vs(code, n);
        for (int i = 0; !ui && !anchors_ui_learned() && i < (int)(sizeof UI_VS / sizeof *UI_VS); i++)
            ui = !memcmp((const BYTE *)code + 4, UI_VS[i], 8);
        if (!ui && g_cfg.has_ui_vs) ui = !memcmp((const BYTE *)code + 4, g_cfg.ui_vs, 8);
        if (ui) {
            const BYTE mark = 1;
            ID3D11VertexShader_SetPrivateData(*o, &GUID_UIVS, sizeof mark, &mark);
            LOGV("world: interface vertex shader %p", *o);
        }
    }
    return hr;
}

void world_hook_device(ID3D11Device *dev)
{
    if (!presets_on() && ratio_eq(g_cfg.scene, g_cfg.scale)) return;
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateVertexShader, hk_CreateVertexShader, &h_cvs);
}

void world_install(void)
{
    if (g_cfg.game_hdr == 0) { g_hdroff = 1; LOG("world: HDR is off in the game settings - own world target, screen textures at world size"); }
    CTX_HOOK(ClearRenderTargetView, hk_ClearRenderTargetView);   /* и для снимка кадра (diag) */
    CTX_HOOK(ResolveSubresource, hk_ResolveSubresource);
    if (!presets_on() && ratio_eq(g_cfg.scene, g_cfg.scale)) return;
    CTX_HOOK(VSSetShader, hk_VSSetShader);
    CTX_HOOK(OMSetDepthStencilState, hk_OMSetDepthStencilState);
}
