/* F06: COM-обёртки immediate и deferred контекстов.
 * Одна обёртка на настоящий контекст: указатель на неё хранится в private data настоящего контекста
 * (в v1 был один глобальный g_imm — второе устройство ломало идентичность). */
#include "common.h"

extern void *g_ctx_thunks[];   /* stubs.S: mov 8(%rcx),%rcx; mov (%rcx),%rax; jmp *8i(%rax) */
#define CTX_SLOTS (sizeof(ID3D11DeviceContext4Vtbl) / sizeof(void *))
_Static_assert(CTX_SLOTS <= 192, "увеличьте CTX_THUNKS в build.py");
_Static_assert(offsetof(CtxWrap, real) == 8, "переходники читают real по смещению 8");

/* {6f1e0b52-3c1d-4b8e-9a51-7d2f0c4a2b10} */
static const GUID GUID_CTXWRAP = {0x6f1e0b52, 0x3c1d, 0x4b8e, {0x9a, 0x51, 0x7d, 0x2f, 0x0c, 0x4a, 0x2b, 0x10}};

static void *g_vt[CTX_SLOTS];
static void *g_hooks[CTX_SLOTS];
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_built;
static SRWLOCK g_lock = SRWLOCK_INIT;   /* поиск обёртки и её последний Release */
static CtxWrap *g_immw;                 /* обёртка непосредственного контекста (под g_lock; без своей ссылки) */

int ctx_hook(size_t off, void *fn, const char *name)
{
    size_t i = off / sizeof(void *);
    if (g_built || i >= CTX_SLOTS || i < 3) {
        LOG("ctx_hook(%s): rejected (%s)", name, g_built ? "vtable already built" : "bad slot");
        return 0;
    }
    g_hooks[i] = fn;
    LOGV("ctx_hook: %s -> slot %zu", name, i);
    return 1;
}

static int is_ctx_iid(REFIID r)
{
    return IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_ID3D11DeviceChild) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext) || IsEqualIID(r, &IID_ID3D11DeviceContext1) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext2) || IsEqualIID(r, &IID_ID3D11DeviceContext3) ||
           IsEqualIID(r, &IID_ID3D11DeviceContext4);
}

static ULONG STDMETHODCALLTYPE w_AddRef(ID3D11DeviceContext *c)
{
    return (ULONG)InterlockedIncrement(&CTXW(c)->ref);
}

static ULONG STDMETHODCALLTYPE w_Release(ID3D11DeviceContext *c)
{
    CtxWrap *w = CTXW(c);
    AcquireSRWLockExclusive(&g_lock);
    LONG r = InterlockedDecrement(&w->ref);
    if (r == 0) {
        ID3D11DeviceContext_SetPrivateData(w->real, &GUID_CTXWRAP, 0, NULL);
        if (g_immw == w) g_immw = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (r == 0) {
        LOGV("context wrapper %p freed (real %p)", w, w->real);
        cb_ctx_forget((ID3D11DeviceContext *)w);
        if (g_cfg.d3d_debug) {   /* сообщения слоя отладки — в лог (тесты без Present) */
            ID3D11Device *d = NULL;
            ID3D11DeviceContext_GetDevice(w->real, &d);
            if (d) { d3dbg_drain(d); ID3D11Device_Release(d); }
        }
        ID3D11DeviceContext_Release(w->real);
        HeapFree(GetProcessHeap(), 0, w);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE w_QI(ID3D11DeviceContext *c, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (is_ctx_iid(riid)) {
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

static BOOL CALLBACK build_vt(PINIT_ONCE o, PVOID p, PVOID *x)
{
    (void)o; (void)p; (void)x;
    int n = 0;
    for (size_t i = 0; i < CTX_SLOTS; i++) {
        g_vt[i] = g_hooks[i] ? g_hooks[i] : g_ctx_thunks[i];
        n += g_hooks[i] != NULL;
    }
    g_vt[0] = (void *)w_QI;
    g_vt[1] = (void *)w_AddRef;
    g_vt[2] = (void *)w_Release;
    InterlockedExchange(&g_built, 1);
    LOG("context vtable: %zu slots, %d hooked", CTX_SLOTS, n);
    return TRUE;
}

/* Ф2 (ревизия 20.11): простые поля CtxState непосредственного контекста — до настоящего Present и обратно после */
int ctx_present_save(CtxPlain *p)
{
    AcquireSRWLockExclusive(&g_lock);
    CtxWrap *w = g_immw;
    if (w) InterlockedIncrement(&w->ref);   /* обёртка не уйдёт до ctx_present_restore */
    ReleaseSRWLockExclusive(&g_lock);
    p->w = w;
    if (!w) return 0;
    const CtxState *s = &w->st;
    p->rk = s->rk; p->nvp = s->nvp; p->nsc = s->nsc;
    memcpy(p->vp, s->vp, sizeof p->vp);
    memcpy(p->sc, s->sc, sizeof p->sc);
    p->vs = s->vs; p->ps = s->ps; p->cs = s->cs;
    p->vs_ui = s->vs_ui; p->zon = s->zon; p->hasdsv = s->hasdsv; p->ps_dof = s->ps_dof; p->ps_box = s->ps_box; p->cs_lum = s->cs_lum;
    p->topo = s->topo;
    p->fp_live = s->fp_live;
    return 1;
}

void ctx_present_restore(const CtxPlain *p)
{
    CtxWrap *w = p->w;
    if (!w) return;
    CtxState *s = &w->st;
    s->rk = p->rk; s->nvp = p->nvp; s->nsc = p->nsc;
    memcpy(s->vp, p->vp, sizeof s->vp);
    memcpy(s->sc, p->sc, sizeof s->sc);
    s->vs = p->vs; s->ps = p->ps; s->cs = p->cs;
    s->vs_ui = p->vs_ui; s->zon = p->zon; s->hasdsv = p->hasdsv; s->ps_dof = p->ps_dof; s->ps_box = p->ps_box; s->cs_lum = p->cs_lum;
    s->topo = p->topo;
    s->fp_live = p->fp_live;
    w_Release((ID3D11DeviceContext *)w);
}

int ctx_is_wrap(const void *c)
{
    return c && *(void *const *)c == (void *)g_vt;
}

ID3D11DeviceContext *ctx_real(ID3D11DeviceContext *c)
{
    return ctx_is_wrap(c) ? RC(c) : c;
}

ID3D11DeviceContext *ctx_wrap(ID3D11DeviceContext *real)
{
    if (!real) return NULL;
    if (ctx_is_wrap(real)) return real;
    InitOnceExecuteOnce(&g_once, build_vt, NULL, NULL);

    AcquireSRWLockExclusive(&g_lock);
    CtxWrap *w = NULL;
    UINT sz = sizeof w;
    if (SUCCEEDED(ID3D11DeviceContext_GetPrivateData(real, &GUID_CTXWRAP, &sz, &w)) && sz == sizeof w && w) {
        InterlockedIncrement(&w->ref);
        ReleaseSRWLockExclusive(&g_lock);
        ID3D11DeviceContext_Release(real);   /* ссылку держит обёртка */
        return (ID3D11DeviceContext *)w;
    }
    w = (CtxWrap *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    if (!w) {
        ReleaseSRWLockExclusive(&g_lock);
        LOG("ctx_wrap: out of memory - context %p left unwrapped", real);
        return real;
    }
    w->vtbl = g_vt;
    w->real = real;
    w->ref = 1;
    w->type = ID3D11DeviceContext_GetType(real);
    w->st.zon = 1;   /* состояние глубины по умолчанию: тест включён */
    ID3D11DeviceContext_SetPrivateData(real, &GUID_CTXWRAP, sizeof w, &w);
    if (w->type == D3D11_DEVICE_CONTEXT_IMMEDIATE) g_immw = w;
    ReleaseSRWLockExclusive(&g_lock);
    LOG("context %p wrapped as %p (%s)", real, w, w->type == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "deferred");
    return (ID3D11DeviceContext *)w;
}
