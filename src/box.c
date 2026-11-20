/* Э8 (19.11, снимок кадра при открытии инвентаря, diag_on=105x120): шаг уменьшения в шейдере копирования игры.
 * Иконку предмета игра рисует 3D-моделью вдвое крупнее ячейки в вид GUI2::eViewport (1024x1024) и уменьшает в ячейку
 * шейдером postprocess/copy.hlsl [BoxRescalePS]: 4 выборки uv, uv + one_pixel, +(one_pixel.x, 0), +(0, one_pixel.y);
 * вариант UV_CORRECTION ещё сдвигает uv на -one_pixel/2. one_pixel (c1.zw) и src_size (c1.xy, варианты MSAA) игра
 * берёт из размера текстуры-источника — логического, его отдаёт GetDesc обёртки: 1/1024 у вида 1024, а у мода вид
 * 2048. Шаг выходит в 2 физических текселя: уменьшение 2:1 усредняет 4x4 вместо 2x2 — иконка мягче, чем у игры в 4K.
 * Правка — на время рисования таким шейдером, если в буфере числа размера его источника (baseTexture):
 *  - src_size = логический размер источника -> настоящий (Load по номерам текселей увеличенной картинки);
 *  - one_pixel = 1/логический -> 1/(логический x k цели). Шаг у игры — тексель источника, то есть половина пикселя
 *    цели при её уменьшении 2:1; в пикселях цели так и остаётся. Источник и цель одного масштаба (обычно): 1/2048 у
 *    вида 2048. Разного — вид 3D игра делает один раз при запуске, а после смены масштаба в настройках ячейки уже
 *    нового (19.11: вид x2, ячейки x1.5): шаг 1/1536 — половина пикселя ячейки, а не 1/2048 и не 1/1024.
 * Перед следующим рисованием другим шейдером — снова числа игры (как радиус размытия, dof.c). Шейдер узнаётся по
 * именам в отражении (RDEF): one_pixel / src_size, которые шейдер читает, и baseTexture. Числа, которые уже поправил
 * cb.c (пары размера экрана — тоже по коэффициенту цели), размеру источника не равны и не трогаются; цель и источник не
 * увеличены — ничего не меняется. Размер — у mip 0 (у видов и кэшей игры он один); вид с другим первым уровнем — числа
 * игры. */
#include <math.h>
#include "common.h"
#include "dxbc.h"

typedef struct { UINT slot, src, one, tex; int has_src, has_one; } BoxLay;   /* bN, номера чисел, tN */
/* {3b8f1d27-5c64-4e0a-9a7b-2f6e1c4d8a53} */
static const GUID GUID_BOX = {0x3b8f1d27, 0x5c64, 0x4e0a, {0x9a, 0x7b, 0x2f, 0x6e, 0x1c, 0x4d, 0x8a, 0x53}};

/* {a4c71e09-3d52-4b8f-96e0-58b2d7c14f3a}: страж живого шейдера (pstag.c) */
static const GUID GUID_BOX_LIVE = {0xa4c71e09, 0x3d52, 0x4b8f, {0x96, 0xe0, 0x58, 0xb2, 0xd7, 0xc1, 0x4f, 0x3a}};

static PsSet g_set;                    /* живые такие шейдеры — быстрый отсев в PSSetShader; правду говорит метка */
static volatile LONG g_have, g_first;
static BoxLay g_lay;                   /* раскладка первого узнанного — для чисел при записи буфера */

int box_on(void) { return g_cfg.box_step > 0; }

/* ---- шейдер ---- */
static VtHook h_cps;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CPS)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device *d, const void *code, SIZE_T n, ID3D11ClassLinkage *l,
                                                      ID3D11PixelShader **o)
{
    HRESULT hr = VT_ORIG(d, &h_cps, PFN_CPS)(d, code, n, l, o);
    if (FAILED(hr) || !o || !*o) return hr;
    DxbcVar s = {0}, p = {0};
    int hs = dxbc_find_var(code, n, "src_size", &s) && s.used && s.size == 8 && !(s.offset & 3);
    int ho = dxbc_find_var(code, n, "one_pixel", &p) && p.used && p.size == 8 && !(p.offset & 3);
    if (hs && ho && s.slot != p.slot) hs = 0;   /* в разных буферах — у игры так не бывает; держимся шага */
    int t = (hs || ho) ? dxbc_find_binding(code, n, "baseTexture") : -1;
    if (t < 0 || t >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return hr;
    BoxLay x = {ho ? p.slot : s.slot, hs ? s.offset / 4 : 0, ho ? p.offset / 4 : 0, (UINT)t, hs, ho};
    ID3D11PixelShader_SetPrivateData(*o, &GUID_BOX, sizeof x, &x);
    if (!InterlockedExchange(&g_first, 1)) { g_lay = x; MemoryBarrier(); InterlockedExchange(&g_have, 1); }   /* раскладка — до флага */
    psset_add(&g_set, *o, &GUID_BOX_LIVE, "box: copy");
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 4)
        LOG("box: copy shader %p reads%s%s of buffer b%u, source t%u - while it draws, size and step follow the real pixels "
            "of the source and the target", (void *)*o, ho ? " one_pixel" : "", hs ? " src_size" : "", x.slot, x.tex);
    return hr;
}

void box_hook_device(ID3D11Device *dev)
{
    if (box_on()) VT_PATCH(dev, ID3D11DeviceVtbl, CreatePixelShader, hk_CreatePixelShader, &h_cps);
}

/* из PSSetShader (dof.c): -1 — похож, метку спросим при рисовании; 0 — нет */
int box_ps_hint(ID3D11PixelShader *ps)
{
    if (!ps || !g_have) return 0;
    return psset_maybe(&g_set, ps) ? -1 : 0;   /* вердикт 19.11, п. 1: набор живых, не счётчик созданных */
}

static int ps_box(CtxState *s, BoxLay *x)
{
    if (!s->ps_box) return 0;
    UINT n = sizeof *x;
    if (s->ps && SUCCEEDED(ID3D11PixelShader_GetPrivateData(s->ps, &GUID_BOX, &n, x)) && n == sizeof *x) return s->ps_box = 1;
    return s->ps_box = 0;
}

/* ---- копии буфера ---- */
void box_written(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n)
{
    if (!g_have || !box_on()) return;
    CtxState *s = &CTXW(c)->st;
    int i = 0;
    while (i < BOX_W && s->boxw[i].r != r) i++;
    /* у буфера без чисел шага копия не нужна; прежнюю — забыть: в буфере уже свежие числа игры */
    UINT need = (g_lay.has_one ? g_lay.one : g_lay.src) + 2;
    if (n > BOX_F || n < need) {
        if (i < BOX_W) { ID3D11Resource_Release(s->boxw[i].r); s->boxw[i].r = NULL; s->boxw[i].adj = 0; }
        return;
    }
    if (i == BOX_W) {   /* новый буфер: место самой давней записи */
        i = s->box_next;
        s->box_next = (s->box_next + 1) % BOX_W;
        if (s->boxw[i].r) ID3D11Resource_Release(s->boxw[i].r);
        ID3D11Resource_AddRef(r);
        s->boxw[i].r = r;
    }
    memcpy(s->boxw[i].f, f, n * sizeof(float));
    s->boxw[i].n = n;
    s->boxw[i].adj = 0;   /* в буфере — то, что записала игра (с поправкой экранных пар) */
}

/* dof.c переписал буфер (своё или число игры): нашей правки в нём больше нет */
void box_overwritten(ID3D11DeviceContext *c, ID3D11Resource *r)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < BOX_W; i++)
        if (s->boxw[i].r == r) s->boxw[i].adj = 0;
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
    } else return;
    dof_overwritten(c, r);   /* правка радиуса размытия в этом буфере тоже затёрта */
}

static void restore(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < BOX_W; i++)
        if (s->boxw[i].r && s->boxw[i].adj) { s->boxw[i].adj = 0; put(c, s->boxw[i].r, s->boxw[i].f, s->boxw[i].n); }
    s->box_adj = 0;
}

static int near_(float a, float b) { return fabsf(a - b) <= 1e-4f * fabsf(b); }

/* по разу на размер источника и коэффициент цели (что видно в логе игрока) */
static void say(UINT lw, UINT lh, UINT pw, UINT ph, Ratio kd, float was, float now)
{
    static ULONGLONG seen[16];
    static volatile LONG ns;
    ULONGLONG key = (ULONGLONG)lw << 48 | (ULONGLONG)lh << 36 | (ULONGLONG)pw << 24 | (ULONGLONG)(kd.num & 0xfff) << 12 | (kd.den & 0xfff);
    for (LONG i = 0; i < ns && i < 16; i++) if (seen[i] == key) return;
    LONG k = InterlockedIncrement(&ns);
    if (k > 16) return;
    seen[k - 1] = key;
    LOG("box: copy from a %ux%u picture (real %ux%u) into a target x%d/%d - step 1/%.0f -> 1/%.0f", lw, lh, pw, ph, kd.num, kd.den,
        1.0 / was, 1.0 / now);
}

void box_pre_draw(ID3D11DeviceContext *c)
{
    if (!g_have || !box_on()) return;
    CtxState *s = &CTXW(c)->st;
    BoxLay x;
    if (!ps_box(s, &x)) {
        if (s->box_adj) restore(c);   /* прошлое рисование было копией с правкой: вернуть числа игры */
        return;
    }
    ID3D11Buffer *b = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(RC(c), x.slot, 1, &b);
    int i = 0;
    while (i < BOX_W && (!b || s->boxw[i].r != (ID3D11Resource *)b)) i++;
    if (b) ID3D11Buffer_Release(b);
    if (i == BOX_W) {
        if (s->box_adj) restore(c);
        return;   /* буфер записан не в этом контексте (или без чисел шага) — числа игры */
    }
    /* источник: логический и настоящий размер (mip 0); увеличен ли модом */
    UINT lw = 0, lh = 0, pw = 0, ph = 0;
    ID3D11ShaderResourceView *v = NULL;
    ID3D11DeviceContext_PSGetShaderResources(RC(c), x.tex, 1, &v);
    if (v) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11ShaderResourceView_GetDesc(v, &vd);
        int mip0 = (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && vd.Texture2D.MostDetailedMip == 0) ||
                   vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS;
        ID3D11Resource *tr = NULL;
        ID3D11ShaderResourceView_GetResource(v, &tr);
        ScaledInfo si;
        ID3D11Texture2D *t2 = NULL;
        if (tr && mip0 && res_scaled(tr, &si)) {
            lw = si.lw; lh = si.lh;
            pw = (UINT)to_phys(si.r, si.lw); ph = (UINT)to_phys(si.r, si.lh);   /* как tex_phys_desc */
        } else if (tr && mip0 && SUCCEEDED(ID3D11Resource_QueryInterface(tr, &IID_ID3D11Texture2D, (void **)&t2)) && t2) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(t2, &td);   /* не увеличен: логический = настоящий */
            lw = pw = td.Width; lh = ph = td.Height;
            ID3D11Texture2D_Release(t2);
        }
        if (tr) ID3D11Resource_Release(tr);
        ID3D11ShaderResourceView_Release(v);
    }
    Ratio kd = s->rk.num ? s->rk : (Ratio){1, 1};   /* коэффициент цели; цель не увеличена — 1 */
    const float *f = s->boxw[i].f;
    UINT n = s->boxw[i].n;
    float tmp[BOX_F];
    int ch = 0;
    if (lw && lh) {
        memcpy(tmp, f, n * sizeof(float));
        if (x.has_src && x.src + 1 < n && (pw != lw || ph != lh) && near_(f[x.src], (float)lw) && near_(f[x.src + 1], (float)lh)) {
            tmp[x.src] = (float)pw; tmp[x.src + 1] = (float)ph; ch = 1;
        }
        if (x.has_one && x.one + 1 < n && kd.num != kd.den && near_(f[x.one], 1.0f / lw) && near_(f[x.one + 1], 1.0f / lh)) {
            tmp[x.one] = (float)kd.den / ((float)lw * kd.num);
            tmp[x.one + 1] = (float)kd.den / ((float)lh * kd.num);
            ch = 1;
        }
    }
    if (!ch) {   /* правка не нужна: если в буфере осталась прошлая — вернуть числа игры */
        if (s->box_adj) restore(c);
        return;
    }
    float pv[4] = {x.has_one ? tmp[x.one] : 0, x.has_one ? tmp[x.one + 1] : 0, x.has_src ? tmp[x.src] : 0, x.has_src ? tmp[x.src + 1] : 0};
    if (s->boxw[i].adj && !memcmp(s->boxw[i].pv, pv, sizeof pv)) return;   /* уже та же правка */
    if (s->box_adj) restore(c);
    put(c, s->boxw[i].r, tmp, n);
    s->boxw[i].adj = 1;
    memcpy(s->boxw[i].pv, pv, sizeof pv);
    s->box_adj = 1;
    if (x.has_one && tmp[x.one] != f[x.one]) say(lw, lh, pw, ph, kd, f[x.one], tmp[x.one]);
    if (g_cfg.ndiag_stk) {   /* разведка (diag_stack): кто рисует уменьшение */
        static volatile LONG ns;
        if (InterlockedIncrement(&ns) <= 12) diag_stack("box draw");
    }
    if (g_cap) {
        if (x.has_one) diag_note(c, "  BOX one_pixel [%u] %g -> %g (source %ux%u, real %ux%u, target x%d/%d)", x.one, f[x.one], tmp[x.one],
                                 lw, lh, pw, ph, kd.num, kd.den);
        if (x.has_src) diag_note(c, "  BOX src_size [%u] %g -> %g", x.src, f[x.src], tmp[x.src]);
    }
}

/* вердикт 19.11, п. 2: перед концом списка команд (FinishCommandList) и перед исполнением чужого — в буфере числа игры.
 * Иначе список уходил с шагом мода, а копии контекста тут же забываются (box_ctx_forget) — вернуть было бы нечем. */
void box_restore(ID3D11DeviceContext *c)
{
    if (CTXW(c)->st.box_adj) restore(c);
}

void box_ctx_forget(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < BOX_W; i++)
        if (s->boxw[i].r) { ID3D11Resource_Release(s->boxw[i].r); s->boxw[i].r = NULL; s->boxw[i].adj = 0; }
    s->box_adj = 0;
}
