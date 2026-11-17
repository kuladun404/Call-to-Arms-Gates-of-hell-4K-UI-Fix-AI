/* F05: масштабированные ресурсы. Метка (private data) хранит логический размер mip0 — GetDesc отдаёт его игре
 * как есть, без деления (при ×1.5 деление физического неоднозначно). Сама D3D (вызов изнутри системной d3d11.dll,
 * например проверка при создании view) получает настоящее описание. */
#include "common.h"
#include "classify.h"

/* {5a1e4b2c-7d31-4c8e-9b10-445566778899} — тот же GUID, что в v1 */
static const GUID GUID_SCALED = {0x5a1e4b2c, 0x7d31, 0x4c8e, {0x9b, 0x10, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99}};

int res_scaled(ID3D11Resource *r, ScaledInfo *out)
{
    if (!r) return 0;
    ScaledInfo si;
    UINT sz = sizeof si;
    if (FAILED(ID3D11Resource_GetPrivateData(r, &GUID_SCALED, &sz, &si)) || sz != sizeof si || !si.lw) return 0;
    if (out) *out = si;
    return 1;
}

int view_scaled(ID3D11View *v, ScaledInfo *out)
{
    if (!v) return 0;
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    int m = res_scaled(r, out);
    if (r) ID3D11Resource_Release(r);
    return m;
}

static VtHook h_tex_getdesc;
typedef void(STDMETHODCALLTYPE *PFN_TexGetDesc)(ID3D11Texture2D *, D3D11_TEXTURE2D_DESC *);

static void STDMETHODCALLTYPE hk_TexGetDesc(ID3D11Texture2D *t, D3D11_TEXTURE2D_DESC *d)
{
    VT_ORIG(t, &h_tex_getdesc, PFN_TexGetDesc)(t, d);
    void *ra = __builtin_return_address(0);
    ScaledInfo si;
    if (d && !ui4k_from_d3d(ra) && !overlay_call(ra) && res_scaled((ID3D11Resource *)t, &si)) {
        d->Width = si.lw;
        d->Height = si.lh;
    }
}

Ratio res_ratio(ID3D11Resource *r)
{
    ScaledInfo si;
    if (res_scaled(r, &si)) return si.r;
    Ratio z = {0, 1};
    return z;
}

static UINT mip_dim(UINT v, UINT mip)
{
    v >>= mip;
    return v ? v : 1;
}

void res_mip_size(const ScaledInfo *si, UINT mip, UINT *lw, UINT *lh, UINT *pw, UINT *ph)
{
    UINT w = (UINT)to_phys(si->r, si->lw), h = (UINT)to_phys(si->r, si->lh);   /* как tex_phys_desc */
    if (mip > 31) mip = 31;
    *lw = mip_dim(si->lw, mip); *lh = mip_dim(si->lh, mip);
    *pw = mip_dim(w ? w : 1, mip); *ph = mip_dim(h ? h : 1, mip);
}

UINT res_edge(const ScaledInfo *si, UINT mip, int vertical, UINT x)
{
    UINT lw, lh, pw, ph;
    res_mip_size(si, mip, &lw, &lh, &pw, &ph);
    UINT l = vertical ? lh : lw, p = vertical ? ph : pw;
    if (x >= l) return p;
    /* уровень 0 — тем же to_phys, что viewport и размеры; мельче — пропорционально настоящему размеру уровня */
    UINT e = mip == 0 ? (UINT)to_phys(si->r, x) : (UINT)(((unsigned long long)x * p + l / 2) / l);
    return e > p ? p : e;
}

void res_mark(ID3D11Texture2D *t, UINT lw, UINT lh, Ratio r)
{
    ScaledInfo si = {lw, lh, r};
    ID3D11Texture2D_SetPrivateData(t, &GUID_SCALED, sizeof si, &si);
    VT_PATCH_OBJ(t, ID3D11Texture2DVtbl, GetDesc, hk_TexGetDesc, &h_tex_getdesc);
}

static VtHook h_ct2d;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CT2D)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                             const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d,
                                                    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    PFN_CT2D orig = VT_ORIG(dev, &h_ct2d, PFN_CT2D);
    if (!d || overlay_call(__builtin_return_address(0))) return orig(dev, d, init, out);
    Screen s = screen_get();
    TexClass c = tex_classify(d, init != NULL, (UINT)s.lw, (UINT)s.lh, g_cfg.scale);
    if (c == TEX_KEEP) return orig(dev, d, init, out);
    if (c == TEX_SCREEN) config_poll_profile();   /* игра пересоздаёт экранные цели, когда меняет HDR или MSAA на ходу */
    /* F14: экранные текстуры не в формате back buffer — сцена (её коэффициент); интерфейс и выход — масштаб интерфейса */
    /* без HDR мир рисуется прямо в экран (world.c), его копии для воды, свечения и размытия — тоже в размере мира */
    int scene = c == TEX_SCREEN && (!screen_is_output_format(d->Format) || (world_hdr_off() && !ratio_eq(g_cfg.scene, g_cfg.scale)));
    Ratio r = scene ? g_cfg.scene : g_cfg.scale;
    D3D11_TEXTURE2D_DESC n = tex_phys_desc(d, r);
    HRESULT hr = orig(dev, &n, NULL, out);
    /* С6: кэшей интерфейса (подсказки, панели) игра создаёт десятки тысяч за сессию. Строка лога на каждую, со сбросом
     * на диск, в потоке отрисовки — это подёргивания и лог без предела. На log=1 пишем первые строки каждого вида и
     * вехи (каждое удвоение счёта); всё подряд — только на log=2. Экранных целей мало, их предел большой. */
    static volatile LONG n_ui, n_scr;
    LONG k = InterlockedIncrement(c == TEX_UI_RT ? &n_ui : &n_scr);
    if (g_cfg.log >= 2 || k <= (c == TEX_UI_RT ? 200 : 2000) || FAILED(hr))
        LOG("tex2D %s: %ux%u -> %ux%u fmt=%d ms=%u bind=0x%x usage=%d mips=%u -> hr=0x%08lx %p",
            c == TEX_UI_RT ? "ui_rt" : scene ? "scene" : "screen", d->Width, d->Height, n.Width, n.Height, d->Format,
            d->SampleDesc.Count, d->BindFlags, d->Usage, d->MipLevels, hr, out ? *out : NULL);
    else if (!(k & (k - 1)))
        LOG("tex2D: %ld enlarged %s textures so far (each one is logged at log=2 only)", (long)k,
            c == TEX_UI_RT ? "interface cache" : "screen");
    if (c == TEX_SCREEN && d->SampleDesc.Count > 1) msaa_seen(d->SampleDesc.Count);   /* В0: MSAA включили во время игры */
    if (SUCCEEDED(hr) && out && *out) res_mark(*out, d->Width, d->Height, r);
    return hr;
}

/* текстура без классификации и увеличения (своя цель мода) */
HRESULT res_create_raw(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, ID3D11Texture2D **out)
{
    return VT_ORIG(dev, &h_ct2d, PFN_CT2D)(dev, d, NULL, out);
}

void res_hook_device(ID3D11Device *dev)
{
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateTexture2D, hk_CreateTexture2D, &h_ct2d);
}
