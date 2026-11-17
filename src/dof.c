/* Г4: глубина резкости. Шейдер размытия игры (postprocess/dof/dof_blur[blurPS], два варианта — по горизонтали и по
 * вертикали) берёт из буфера сцены (156 float, b0) радиус размытия в точках (maxCoCRadiusPixels, число 96) и размер
 * картинки размытия (textureDimensions, 97-98). Размер cb.c правит как экранную пару — картинка у мода больше, — а
 * радиус остаётся числом игры: шаг размытия стал мельче, размытие слабее, чем у игры в том же разрешении мира.
 * Править радиус при записи буфера нельзя: та же ячейка на других проходах значит другое (nearScale у dof_CoC,
 * deltaTime у dof_thirdPersonPC). Поэтому мод переписывает буфер только на время рисования шейдером размытия и
 * возвращает как было перед следующим рисованием другим шейдером.
 * Шейдер узнаётся по имени переменной в его отражении (RDEF) — не по контрольной сумме; оттуда же номера чисел и регистр.
 * Множитель радиуса — s^dof_radius, s — во сколько раз cb.c увеличил ширину картинки размытия. Игра сама берёт радиус
 * 2 на 1920x1080 и 8 на 3840x2160 (снимки кадра 17.11): радиус растёт как квадрат размера, поэтому по умолчанию 2 —
 * мир строки «3840x2160 ×2» размыт как у игры в 3840x2160. 1 — размыт как у игры в логическом разрешении. */
#include "common.h"
#include "dxbc.h"

typedef struct { UINT slot, rad, dim; } DofLay;   /* регистр буфера, номер числа радиуса, номер ширины картинки */
/* {6c2e9a41-0b7d-4f35-8e62-d1a4c3b5f907} */
static const GUID GUID_DOF = {0x6c2e9a41, 0x0b7d, 0x4f35, {0x8e, 0x62, 0xd1, 0xa4, 0xc3, 0xb5, 0xf9, 0x07}};

static DofLay g_lay;                 /* у обоих вариантов шейдера одна раскладка */
static volatile LONG g_have;         /* игра создала шейдер размытия */
#define DOF_PS 8
static void *volatile g_ps[DOF_PS];  /* адреса шейдеров размытия — быстрый отсев в PSSetShader; правду говорит метка */
static volatile LONG g_nps;

static int on(void) { return g_cfg.dof_radius > 0 && g_have; }

/* ---- шейдер ---- */
static VtHook h_cps;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CPS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device *d, const void *code, SIZE_T n, ID3D11ClassLinkage *l,
                                                      ID3D11PixelShader **o)
{
    HRESULT hr = VT_ORIG(d, &h_cps, PFN_CPS)(d, code, n, l, o);
    DxbcVar r, w;
    if (SUCCEEDED(hr) && o && *o && dxbc_find_var(code, n, "maxCoCRadiusPixels", &r) &&
        dxbc_find_var(code, n, "textureDimensions", &w) && r.slot == w.slot && r.size == 4 && w.size >= 4 &&
        !(r.offset & 3) && !(w.offset & 3)) {
        DofLay x = {r.slot, r.offset / 4, w.offset / 4};
        ID3D11PixelShader_SetPrivateData(*o, &GUID_DOF, sizeof x, &x);
        g_lay = x;
        g_ps[(InterlockedIncrement(&g_nps) - 1) % DOF_PS] = *o;
        InterlockedExchange(&g_have, 1);
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 4)
            LOG("dof: depth-of-field blur shader %p: radius float[%u], image width float[%u] of buffer b%u%s", (void *)*o, x.rad,
                x.dim, x.slot, g_cfg.dof_radius > 0 ? "" : " (dof_radius=0 - left as is)");
    }
    return hr;
}

void dof_hook_device(ID3D11Device *dev)
{
    if (g_cfg.dof_radius > 0) VT_PATCH(dev, ID3D11DeviceVtbl, CreatePixelShader, hk_CreatePixelShader, &h_cps);
}

/* ---- привязка шейдера ---- */
typedef void(STDMETHODCALLTYPE *PFN_PSS)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
static void STDMETHODCALLTYPE hk_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *ps, ID3D11ClassInstance *const *ci, UINT n)
{
    CtxState *s = &CTXW(c)->st;
    s->ps = ps;
    s->ps_dof = 0;
    if (ps && g_have) {
        if (g_nps > DOF_PS) s->ps_dof = -1;   /* ревизия кода 4, п. 3: адресов больше, чем помнит отсев, — метку спросим всегда */
        else
            for (int i = 0; i < DOF_PS; i++)
                if (g_ps[i] == ps) { s->ps_dof = -1; break; }   /* похоже — спросим метку при рисовании */
    }
    ORIGC(c, PSSetShader, PFN_PSS)(RC(c), ps, ci, n);
}

static int ps_is_dof(CtxState *s)
{
    if (s->ps_dof < 0) {
        DofLay x;
        UINT n = sizeof x;
        s->ps_dof = s->ps && SUCCEEDED(ID3D11PixelShader_GetPrivateData(s->ps, &GUID_DOF, &n, &x)) && n == sizeof x;
    }
    return s->ps_dof;
}

void dof_install(void)
{
    if (g_cfg.dof_radius > 0) CTX_HOOK(PSSetShader, hk_PSSetShader);
}

/* ---- копии буфера сцены ---- */
float dof_ldim(const float *f, UINT n)
{
    return on() && g_lay.rad < n && g_lay.dim < n ? f[g_lay.dim] : 0;
}

void dof_written(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n, float ldim)
{
    if (!on()) return;
    CtxState *s = &CTXW(c)->st;
    int i = 0;
    while (i < DOF_W && s->dofw[i].r != r) i++;
    if (ldim > 0 && n > DOF_F) {   /* ревизия кода 4, п. 2: обновление игры вырастило буфер — сказать, а не молчать */
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 2)
            LOG("dof: %u-float scene buffer is larger than %d - blur radius left as is (game update?)", n, DOF_F);
    }
    if (ldim <= 0 || n > DOF_F || g_lay.rad >= n || g_lay.dim >= n) {
        /* ревизия кода 3: эту запись игры не запоминаем — тогда и прежнюю копию буфера забыть: в нём уже свежие числа
         * игры, и вернуть «свою копию» после размытия значило бы их затереть */
        if (i < DOF_W) { ID3D11Resource_Release(s->dofw[i].r); s->dofw[i].r = NULL; s->dofw[i].adj = 0; }
        return;
    }
    if (i == DOF_W) {   /* новый буфер: место самой давней записи */
        i = s->dof_next;
        s->dof_next = (s->dof_next + 1) % DOF_W;
        if (s->dofw[i].r) ID3D11Resource_Release(s->dofw[i].r);
        ID3D11Resource_AddRef(r);
        s->dofw[i].r = r;
    }
    memcpy(s->dofw[i].f, f, n * sizeof(float));
    s->dofw[i].n = n;
    s->dofw[i].ldim = ldim;
    s->dofw[i].adj = 0;   /* в буфере — то, что записала игра (с поправкой экранных пар) */
}

static void put(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n)
{
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    if (d.ByteWidth < n * sizeof(float)) return;
    if (d.Usage == D3D11_USAGE_DYNAMIC) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(RC(c), r, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, f, n * sizeof(float));
            ID3D11DeviceContext_Unmap(RC(c), r, 0);
        }
    } else if (d.Usage == D3D11_USAGE_DEFAULT) {
        ID3D11DeviceContext_UpdateSubresource(RC(c), r, 0, NULL, f, 0, 0);
    }
}

/* что игра считает радиусом на каждой строке — по разу на значение (данные для формулы радиуса) */
static void say(float lrad, float rad, float ldim, float dim)
{
    static float seen[16];
    static volatile LONG ns;
    for (LONG i = 0; i < ns && i < 16; i++) if (seen[i] == lrad) return;
    LONG k = InterlockedIncrement(&ns);
    if (k > 16) return;
    seen[k - 1] = lrad;
    LOG("dof: blur radius %g -> %g (blur image %g -> %g wide, radius x %g)", lrad, rad, ldim, dim, rad / lrad);
}

void dof_pre_draw(ID3D11DeviceContext *c)
{
    if (!on()) return;
    CtxState *s = &CTXW(c)->st;
    if (!ps_is_dof(s)) {
        if (s->dof_adj) {   /* прошлое рисование было размытием: вернуть буфер игры */
            for (int i = 0; i < DOF_W; i++)
                if (s->dofw[i].r && s->dofw[i].adj) { put(c, s->dofw[i].r, s->dofw[i].f, s->dofw[i].n); s->dofw[i].adj = 0; }
            s->dof_adj = 0;
        }
        return;
    }
    ID3D11Buffer *b = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(RC(c), g_lay.slot, 1, &b);
    int i = 0;
    while (i < DOF_W && (!b || s->dofw[i].r != (ID3D11Resource *)b)) i++;
    if (b) ID3D11Buffer_Release(b);
    if (i == DOF_W) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 3) LOG("dof: blur draw, but its buffer was not written in this context - radius left as is");
        return;
    }
    if (s->dofw[i].adj) return;
    float *f = s->dofw[i].f, k = f[g_lay.dim] / s->dofw[i].ldim, m = 1;
    if (k > 0.999f && k < 1.001f) return;   /* картинка размытия не увеличена */
    for (int e = 0; e < g_cfg.dof_radius && e < 4; e++) m *= k;
    float tmp[DOF_F];
    UINT n = s->dofw[i].n;
    memcpy(tmp, f, n * sizeof(float));
    tmp[g_lay.rad] = f[g_lay.rad] * m;
    put(c, s->dofw[i].r, tmp, n);
    s->dofw[i].adj = 1;
    s->dof_adj = 1;
    say(f[g_lay.rad], tmp[g_lay.rad], s->dofw[i].ldim, f[g_lay.dim]);
    if (g_cap) diag_note(c, "  DOF radius [%u] %g -> %g (image width %g -> %g)", g_lay.rad, f[g_lay.rad], tmp[g_lay.rad],
                         s->dofw[i].ldim, f[g_lay.dim]);
}

void dof_reset(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    s->ps = NULL;
    s->ps_dof = 0;
}

void dof_ctx_forget(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < DOF_W; i++)
        if (s->dofw[i].r) { ID3D11Resource_Release(s->dofw[i].r); s->dofw[i].r = NULL; s->dofw[i].adj = 0; }
    s->dof_adj = 0;
}
