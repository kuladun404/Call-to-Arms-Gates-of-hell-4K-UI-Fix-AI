/* F09: копии между масштабированными (F05) и обычными ресурсами. Игра задаёт области в своих (логических) размерах.
 * Правило ревизии (В0): в D3D уходит только вызов, верный в настоящих размерах. Без отладочного слоя D3D копию
 * с несходящимися размерами не проверяет — от «ничего» до падения драйвера и «устройство удалено».
 *  - обе стороны увеличены одинаково — координаты и box в настоящие по краям (res_edge), лишнее обрезается;
 *  - настоящие размеры областей разные (обычная <-> увеличенная, мир <-> экран) — мод пересчитывает сам: область
 *    источника рисуется шейдером в область приёмника. Что нельзя читать шейдером или во что нельзя рисовать
 *    («читаемые» staging-текстуры, массивы) — через временную текстуру размера области;
 *  - пересчитать нельзя (целочисленный формат, глубина) — копия 1:1 той части, что помещается, и запись в лог;
 *    MSAA разного размера — вызов не передаётся.
 * Примеры: 3D-иконки инвентаря (обычная 512x512 -> увеличенная промежуточная: без растяжения предмет вдвое меньше
 * клетки, v1 15.11); скриншот и картинка сохранения (увеличенный кадр -> «читаемая» текстура логического размера).
 * Рисование идёт на настоящем контексте, состояние игры сохраняется и восстанавливается (CtxState не трогаем:
 * возвращаем ровно то, что стояло в настоящем контексте). Урок v1: CB нужен и VS, и PS. */
#include "common.h"
#include "fmt.h"

typedef void(STDMETHODCALLTYPE *PFN_CSR)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                        ID3D11Resource *, UINT, const D3D11_BOX *);
typedef void(STDMETHODCALLTYPE *PFN_CSR1)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                         ID3D11Resource *, UINT, const D3D11_BOX *, UINT);
typedef void(STDMETHODCALLTYPE *PFN_CR)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *,
                                     const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);

static const char SRC[] =
    "cbuffer r : register(b0) { float4 rect; float4 lim; };\n"
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O vs(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = rect.xy + t * rect.zw; return o; }\n"
    "Texture2D s : register(t0); SamplerState ss : register(s0);\n"
    "float4 ps(O i) : SV_Target { return s.Sample(ss, clamp(i.uv, lim.xy, lim.zw)); }\n";

static struct {
    ID3D11Device *dev;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11SamplerState *smp;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bl;
    ID3D11DepthStencilState *dss;
    ID3D11Buffer *cb;
    int failed;
} B;
static SRWLOCK g_blk = SRWLOCK_INIT;

static int blit_init(ID3D11Device *dev)
{
    if (B.dev == dev) return 1;
    if (B.failed) return 0;
    AcquireSRWLockExclusive(&g_blk);
    if (B.dev != dev && !B.failed) {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        PFN_COMPILE comp = m ? (PFN_COMPILE)GetProcAddress(m, "D3DCompile") : NULL;
        ID3DBlob *v = NULL, *p = NULL;
        int ok = comp && SUCCEEDED(comp(SRC, sizeof SRC - 1, "blit", NULL, NULL, "vs", "vs_4_0", 0, 0, &v, NULL)) &&
                 SUCCEEDED(comp(SRC, sizeof SRC - 1, "blit", NULL, NULL, "ps", "ps_4_0", 0, 0, &p, NULL)) &&
                 SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &B.vs)) &&
                 SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(p), ID3D10Blob_GetBufferSize(p), NULL, &B.ps));
        if (v) ID3D10Blob_Release(v);
        if (p) ID3D10Blob_Release(p);
        D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, 0};
        D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
        D3D11_BLEND_DESC bd = {0};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC dd = {0};
        D3D11_BUFFER_DESC cd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ok = ok && SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &sd, &B.smp)) &&
             SUCCEEDED(ID3D11Device_CreateRasterizerState(dev, &rd, &B.rs)) &&
             SUCCEEDED(ID3D11Device_CreateBlendState(dev, &bd, &B.bl)) &&
             SUCCEEDED(ID3D11Device_CreateDepthStencilState(dev, &dd, &B.dss)) &&
             SUCCEEDED(ID3D11Device_CreateBuffer(dev, &cd, NULL, &B.cb));
        if (ok) { MemoryBarrier(); B.dev = dev; }
        else B.failed = 1;
        LOG("copy: stretch blit %s", ok ? "ready" : "UNAVAILABLE (plain copies only)");
    }
    ReleaseSRWLockExclusive(&g_blk);
    return B.dev == dev;
}

typedef struct { UINT x0, y0, x1, y1; } Rc;   /* область в настоящих пикселях уровня */

/* Ревизия кода 17.11, п. 1: привязка приёмника целью рисования заставляет D3D отвязать его от всех слотов чтения во всех
 * шейдерах; OMSetRenderTargets снимает и UAV вывода; привязка источника на чтение снимает его с записи (UAV вычислений).
 * Игра об этом не знает и может рисовать дальше без перепривязки — эффект читал бы пустоту. Такие привязки запоминаем
 * и возвращаем после рисования. Лишней памяти не нужно. */
#define SRV_N D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define UAV_N D3D11_PS_CS_UAV_REGISTER_COUNT
typedef struct {
    struct { int stage; UINT slot; ID3D11ShaderResourceView *v; } srv[32];
    int nsrv;
    ID3D11UnorderedAccessView *om[UAV_N];   /* все UAV вывода (со ссылками) */
    struct { UINT slot; ID3D11UnorderedAccessView *v; } cs[UAV_N];
    int ncs;
} Binds;

static int view_is(ID3D11View *v, ID3D11Resource *a, ID3D11Resource *b)
{
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    if (r) ID3D11Resource_Release(r);
    return r && (r == a || r == b);
}

static void srv_get(ID3D11DeviceContext *c, int st, ID3D11ShaderResourceView **v)
{
    switch (st) {
    case 0: ID3D11DeviceContext_VSGetShaderResources(c, 0, SRV_N, v); break;
    case 1: ID3D11DeviceContext_HSGetShaderResources(c, 0, SRV_N, v); break;
    case 2: ID3D11DeviceContext_DSGetShaderResources(c, 0, SRV_N, v); break;
    case 3: ID3D11DeviceContext_GSGetShaderResources(c, 0, SRV_N, v); break;
    case 4: ID3D11DeviceContext_PSGetShaderResources(c, 0, SRV_N, v); break;
    default: ID3D11DeviceContext_CSGetShaderResources(c, 0, SRV_N, v); break;
    }
}

static void srv_set(ID3D11DeviceContext *c, int st, UINT slot, ID3D11ShaderResourceView *v)
{
    switch (st) {
    case 0: ID3D11DeviceContext_VSSetShaderResources(c, slot, 1, &v); break;
    case 1: ID3D11DeviceContext_HSSetShaderResources(c, slot, 1, &v); break;
    case 2: ID3D11DeviceContext_DSSetShaderResources(c, slot, 1, &v); break;
    case 3: ID3D11DeviceContext_GSSetShaderResources(c, slot, 1, &v); break;
    case 4: ID3D11DeviceContext_PSSetShaderResources(c, slot, 1, &v); break;
    default: ID3D11DeviceContext_CSSetShaderResources(c, slot, 1, &v); break;
    }
}

/* приёмник в слотах чтения всех шейдеров, UAV вывода, источник и приёмник в UAV вычислений */
static void binds_save(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src, Binds *b)
{
    ZeroMemory(b, sizeof *b);
    ID3D11ShaderResourceView *v[SRV_N];
    for (int st = 0; st < 6; st++) {
        srv_get(c, st, v);
        for (UINT i = 0; i < SRV_N; i++) {
            if (!v[i]) continue;
            if (b->nsrv < 32 && view_is((ID3D11View *)v[i], dst, NULL)) {
                b->srv[b->nsrv].stage = st; b->srv[b->nsrv].slot = i; b->srv[b->nsrv].v = v[i]; b->nsrv++;
            } else ID3D11ShaderResourceView_Release(v[i]);
        }
    }
    ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews(c, 0, NULL, NULL, 0, UAV_N, b->om);
    ID3D11UnorderedAccessView *u[UAV_N];
    ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, UAV_N, u);
    for (UINT i = 0; i < UAV_N; i++) {
        if (!u[i]) continue;
        if (view_is((ID3D11View *)u[i], dst, src)) { b->cs[b->ncs].slot = i; b->cs[b->ncs].v = u[i]; b->ncs++; }
        else ID3D11UnorderedAccessView_Release(u[i]);
    }
}

/* цели игры вместе с UAV вывода; потом слоты чтения и UAV вычислений. Счётчики UAV не трогаем (-1) */
static void binds_restore(ID3D11DeviceContext *c, ID3D11RenderTargetView *const *ort, ID3D11DepthStencilView *odsv, Binds *b)
{
    static const UINT keep[UAV_N] = {(UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1};
    UINT nrt = 0, u0 = UAV_N;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) if (ort[i]) nrt = i + 1;
    for (UINT i = 0; i < UAV_N; i++) if (b->om[i]) { u0 = i; break; }
    if (u0 < UAV_N && u0 >= nrt)
        ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(c, nrt, ort, odsv, u0, UAV_N - u0, b->om + u0, keep);
    else
        ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, ort, odsv);
    for (int i = 0; i < b->nsrv; i++) {
        srv_set(c, b->srv[i].stage, b->srv[i].slot, b->srv[i].v);
        ID3D11ShaderResourceView_Release(b->srv[i].v);
    }
    for (int i = 0; i < b->ncs; i++) {
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, b->cs[i].slot, 1, &b->cs[i].v, keep);
        ID3D11UnorderedAccessView_Release(b->cs[i].v);
    }
    for (UINT i = 0; i < UAV_N; i++) if (b->om[i]) ID3D11UnorderedAccessView_Release(b->om[i]);
}

/* нарисовать область sr уровня smip текстуры src (настоящий размер уровня sw x sh) в область dr уровня dmip текстуры
 * dst; vf — формат обоих видов (fmt_view). 1 — нарисовано; 0 — нельзя (вызывающий решает, что делать). */
static int blit(ID3D11DeviceContext *real, ID3D11Resource *dst, UINT dmip, const Rc *dr, ID3D11Resource *src, UINT smip,
                const Rc *sr, UINT sw, UINT sh, DXGI_FORMAT vf)
{
    if (!vf || !sw || !sh || dr->x1 <= dr->x0 || dr->y1 <= dr->y0 || sr->x1 <= sr->x0 || sr->y1 <= sr->y0) return 0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceChild_GetDevice((ID3D11DeviceChild *)dst, &dev);
    int ok = dev && blit_init(dev);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    D3D11_RENDER_TARGET_VIEW_DESC rvd;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd;
    ZeroMemory(&rvd, sizeof rvd);
    ZeroMemory(&svd, sizeof svd);
    rvd.Format = vf; rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rvd.Texture2D.MipSlice = dmip;
    D3D11_TEXTURE2D_DESC dd;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)dst, &dd);
    /* П7: многовыборочный приёмник (экран игры с MSAA) — рисуем во все выборки; уровень у такой текстуры один */
    if (dd.SampleDesc.Count > 1) rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    svd.Format = vf; svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MostDetailedMip = smip;
    svd.Texture2D.MipLevels = 1;
    ok = ok && SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, dst, &rvd, &rtv)) &&
         SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, src, &svd, &srv));
    if (dev) ID3D11Device_Release(dev);
    if (!ok) {
        if (rtv) ID3D11RenderTargetView_Release(rtv);
        if (srv) ID3D11ShaderResourceView_Release(srv);
        return 0;
    }
    /* ---- состояние игры ---- */
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11InputLayout *il;
    ID3D11VertexShader *vs; ID3D11GeometryShader *gs; ID3D11HullShader *hs; ID3D11DomainShader *ds;
    ID3D11PixelShader *ps;
    ID3D11Buffer *vcb, *pcb;
    ID3D11ShaderResourceView *psrv;
    ID3D11SamplerState *pss;
    ID3D11RasterizerState *rs;
    D3D11_VIEWPORT vp[MAX_VP];
    UINT nvp = MAX_VP;
    ID3D11RenderTargetView *ort[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *odsv;
    ID3D11BlendState *obl; FLOAT bf[4]; UINT bm;
    ID3D11DepthStencilState *odss; UINT sref;
    ID3D11DeviceContext_IAGetPrimitiveTopology(real, &topo);
    ID3D11DeviceContext_IAGetInputLayout(real, &il);
    ID3D11DeviceContext_VSGetShader(real, &vs, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(real, &gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(real, &hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(real, &ds, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(real, &ps, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(real, 0, 1, &vcb);
    ID3D11DeviceContext_PSGetConstantBuffers(real, 0, 1, &pcb);
    ID3D11DeviceContext_PSGetShaderResources(real, 0, 1, &psrv);
    ID3D11DeviceContext_PSGetSamplers(real, 0, 1, &pss);
    ID3D11DeviceContext_RSGetState(real, &rs);
    ID3D11DeviceContext_RSGetViewports(real, &nvp, vp);
    ID3D11DeviceContext_OMGetRenderTargets(real, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, ort, &odsv);
    ID3D11DeviceContext_OMGetBlendState(real, &obl, bf, &bm);
    ID3D11DeviceContext_OMGetDepthStencilState(real, &odss, &sref);
    Binds bd;
    binds_save(real, dst, src, &bd);
    /* Слой отладки Direct3D (17.11): источник или приёмник ещё привязан игрой как UAV вычислений — D3D обнулил бы наше
     * чтение источника («still bound on output! Forcing to NULL»), и копия вышла бы чёрной на любой видеокарте. Снять их
     * на время рисования; binds_restore вернёт */
    for (int i = 0; i < bd.ncs; i++) {
        ID3D11UnorderedAccessView *nu = NULL;
        UINT k = (UINT)-1;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(real, bd.cs[i].slot, 1, &nu, &k);
    }
    /* приёмник — из слотов чтения самим (D3D снял бы его сам при привязке цели, но с предупреждением в слое отладки на
     * каждую копию — за ними не видно настоящих); binds_restore вернёт */
    for (int i = 0; i < bd.nsrv; i++) srv_set(real, bd.srv[i].stage, bd.srv[i].slot, NULL);
    /* ---- растянуть: область источника в UV, lim — центры крайних текселей (соседей снаружи не подмешивать) ---- */
    float fw = (float)sw, fh = (float)sh;
    float rect[8] = {sr->x0 / fw, sr->y0 / fh, (sr->x1 - sr->x0) / fw, (sr->y1 - sr->y0) / fh,
                     (sr->x0 + 0.5f) / fw, (sr->y0 + 0.5f) / fh, (sr->x1 - 0.5f) / fw, (sr->y1 - 0.5f) / fh};
    ID3D11DeviceContext_UpdateSubresource(real, (ID3D11Resource *)B.cb, 0, NULL, rect, 0, 0);
    D3D11_VIEWPORT v = {(float)dr->x0, (float)dr->y0, (float)(dr->x1 - dr->x0), (float)(dr->y1 - dr->y0), 0, 1};
    ID3D11DeviceContext_IASetPrimitiveTopology(real, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(real, NULL);
    ID3D11DeviceContext_VSSetShader(real, B.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(real, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(real, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(real, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(real, B.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(real, 0, 1, &B.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(real, 0, 1, &B.cb);
    ID3D11DeviceContext_OMSetRenderTargets(real, 1, &rtv, NULL);   /* сначала цель: src мог быть привязан как цель */
    ID3D11DeviceContext_PSSetShaderResources(real, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(real, 0, 1, &B.smp);
    ID3D11DeviceContext_RSSetState(real, B.rs);
    ID3D11DeviceContext_RSSetViewports(real, 1, &v);
    FLOAT zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_OMSetBlendState(real, B.bl, zero, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(real, B.dss, 0);
    ID3D11DeviceContext_Draw(real, 3, 0);
    /* ---- вернуть ---- */
    ID3D11ShaderResourceView *nul = NULL;
    ID3D11DeviceContext_PSSetShaderResources(real, 0, 1, &nul);
    ID3D11DeviceContext_IASetPrimitiveTopology(real, topo);
    ID3D11DeviceContext_IASetInputLayout(real, il);
    ID3D11DeviceContext_VSSetShader(real, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(real, gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(real, hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(real, ds, NULL, 0);
    ID3D11DeviceContext_PSSetShader(real, ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(real, 0, 1, &vcb);
    ID3D11DeviceContext_PSSetConstantBuffers(real, 0, 1, &pcb);
    binds_restore(real, ort, odsv, &bd);   /* цели игры и UAV вывода; приёмник — обратно в слоты чтения */
    ID3D11DeviceContext_PSSetShaderResources(real, 0, 1, &psrv);
    ID3D11DeviceContext_PSSetSamplers(real, 0, 1, &pss);
    ID3D11DeviceContext_RSSetState(real, rs);
    ID3D11DeviceContext_RSSetViewports(real, nvp, vp);
    ID3D11DeviceContext_OMSetBlendState(real, obl, bf, bm);
    ID3D11DeviceContext_OMSetDepthStencilState(real, odss, sref);
    if (il) ID3D11InputLayout_Release(il);
    if (vs) ID3D11VertexShader_Release(vs);
    if (gs) ID3D11GeometryShader_Release(gs);
    if (hs) ID3D11HullShader_Release(hs);
    if (ds) ID3D11DomainShader_Release(ds);
    if (ps) ID3D11PixelShader_Release(ps);
    if (vcb) ID3D11Buffer_Release(vcb);
    if (pcb) ID3D11Buffer_Release(pcb);
    if (psrv) ID3D11ShaderResourceView_Release(psrv);
    if (pss) ID3D11SamplerState_Release(pss);
    if (rs) ID3D11RasterizerState_Release(rs);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
        if (ort[i]) ID3D11RenderTargetView_Release(ort[i]);
    if (odsv) ID3D11DepthStencilView_Release(odsv);
    if (obl) ID3D11BlendState_Release(obl);
    if (odss) ID3D11DepthStencilState_Release(odss);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11ShaderResourceView_Release(srv);
    LOGV("copy: drawn src [%u,%u %ux%u] of %ux%u -> dst [%u,%u %ux%u]", sr->x0, sr->y0, sr->x1 - sr->x0, sr->y1 - sr->y0, sw, sh,
         dr->x0, dr->y0, dr->x1 - dr->x0, dr->y1 - dr->y0);
    return 1;
}

/* ---- текстура глазами игры и настоящая ---- */
typedef struct {
    D3D11_TEXTURE2D_DESC d;   /* Width/Height — как видит игра; остальное настоящее */
    ScaledInfo si;            /* не увеличена: коэффициент 1/1, размер как есть */
    int scaled;
} Tex;

static int tex_get(ID3D11Resource *r, Tex *t)
{
    D3D11_RESOURCE_DIMENSION dim;
    if (!r) return 0;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return 0;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &t->d);
    t->scaled = res_scaled(r, &t->si);
    if (!t->scaled) { t->si.lw = t->d.Width; t->si.lh = t->d.Height; t->si.r.num = 1; t->si.r.den = 1; }
    if (!t->d.MipLevels) t->d.MipLevels = 1;
    return 1;
}

/* Ревизия кода 17.11, п. 5: временные текстуры пересчёта — из небольшого запаса. Создавать и удалять текстуру (до
 * размера кадра, десятки МБ) на каждую копию — подёргивания, если такая копия идёт каждый кадр. Запас держит одну ссылку;
 * занятая текстура другому вызову не достаётся. Каждая последовательность «записать во временную -> прочитать» целиком
 * в одном контексте, поэтому общая текстура у разных списков команд безопасна. Не нужная 2 с — освобождается (copy_tick
 * из Present): постоянной лишней видеопамяти нет. */
#define POOL_N 6
static struct { ID3D11Device *dev; ID3D11Texture2D *t; UINT w, h, bind; DXGI_FORMAT f; ULONGLONG used; int busy; } g_pool[POOL_N];
static SRWLOCK g_poolk = SRWLOCK_INIT;

static ID3D11Texture2D *scratch(ID3D11Resource *like, UINT w, UINT h, DXGI_FORMAT f, UINT bind)
{
    ID3D11Device *dev = NULL;
    ID3D11Texture2D *t = NULL;
    ID3D11DeviceChild_GetDevice((ID3D11DeviceChild *)like, &dev);
    if (!dev) return NULL;
    AcquireSRWLockExclusive(&g_poolk);
    for (int i = 0; i < POOL_N && !t; i++)
        if (g_pool[i].t && !g_pool[i].busy && g_pool[i].dev == dev && g_pool[i].w == w && g_pool[i].h == h && g_pool[i].f == f &&
            g_pool[i].bind == bind) {
            g_pool[i].busy = 1;
            t = g_pool[i].t;
            ID3D11Texture2D_AddRef(t);
        }
    ReleaseSRWLockExclusive(&g_poolk);
    if (t) { ID3D11Device_Release(dev); return t; }
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    res_create_raw(dev, &d, &t);
    if (t) {
        LOGV("copy: temporary texture %ux%u fmt=%d bind=0x%x created", w, h, f, bind);
        AcquireSRWLockExclusive(&g_poolk);
        int k = -1;
        for (int i = 0; i < POOL_N && k < 0; i++) if (!g_pool[i].t) k = i;
        for (int i = 0; i < POOL_N && k < 0; i++)   /* полон: место самой давней свободной */
            if (!g_pool[i].busy && (k < 0 || g_pool[i].used < g_pool[k].used)) k = i;
        ID3D11Texture2D *old = NULL;
        if (k >= 0) {
            old = g_pool[k].t;
            g_pool[k].t = t; g_pool[k].dev = dev; g_pool[k].w = w; g_pool[k].h = h; g_pool[k].f = f; g_pool[k].bind = bind;
            g_pool[k].busy = 1;
            ID3D11Texture2D_AddRef(t);   /* ссылка запаса */
        }
        ReleaseSRWLockExclusive(&g_poolk);
        if (old) ID3D11Texture2D_Release(old);
    }
    ID3D11Device_Release(dev);
    return t;
}

/* вызов закончил с временной текстурой: в запас (свободна) или, если её там нет, — удалить */
static void scratch_done(ID3D11Texture2D *t)
{
    if (!t) return;
    AcquireSRWLockExclusive(&g_poolk);
    for (int i = 0; i < POOL_N; i++)
        if (g_pool[i].t == t) { g_pool[i].busy = 0; g_pool[i].used = GetTickCount64(); break; }
    ReleaseSRWLockExclusive(&g_poolk);
    ID3D11Texture2D_Release(t);
}

void copy_tick(void)
{
    ULONGLONG now = GetTickCount64();
    ID3D11Texture2D *drop[POOL_N];
    int n = 0;
    AcquireSRWLockExclusive(&g_poolk);
    for (int i = 0; i < POOL_N; i++)
        if (g_pool[i].t && !g_pool[i].busy && now - g_pool[i].used > 2000) {
            drop[n++] = g_pool[i].t;
            ZeroMemory(&g_pool[i], sizeof g_pool[i]);
        }
    ReleaseSRWLockExclusive(&g_poolk);
    for (int i = 0; i < n; i++) ID3D11Texture2D_Release(drop[i]);
    if (n) LOGV("copy: %d temporary texture(s) released (unused for 2 s)", n);
}

/* область источника -> область приёмника с пересчётом (настоящие размеры областей разные). Где нельзя читать шейдером
 * или рисовать напрямую — временная текстура размера области. 1 — сделано. */
static int convert(ID3D11DeviceContext *real, ID3D11Resource *dst, const Tex *td, UINT dsub, const Rc *dr, ID3D11Resource *src,
                   const Tex *ts, UINT ssub, const Rc *sr)
{
    if (td->d.SampleDesc.Count != 1 || ts->d.SampleDesc.Count != 1) return 0;
    DXGI_FORMAT vf = fmt_view(ts->d.Format, td->d.Format);
    if (!vf) return 0;
    UINT smip = ssub % ts->d.MipLevels, dmip = dsub % td->d.MipLevels, slw, slh, srw, srh;
    res_mip_size(&ts->si, smip, &slw, &slh, &srw, &srh);
    int sdirect = (ts->d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && ts->d.ArraySize == 1 && src != dst;
    int ddirect = (td->d.BindFlags & D3D11_BIND_RENDER_TARGET) && td->d.ArraySize == 1;
    ID3D11Texture2D *ss = NULL, *sd = NULL;
    ID3D11Resource *from = src, *to = dst;
    Rc fr = *sr, tr = *dr;
    UINT fmip = smip, tmip = dmip, fw = srw, fh = srh;
    int ok = 1;
    if (!sdirect) {   /* источник нельзя читать шейдером: его область -> временная текстура 1:1 */
        fw = sr->x1 - sr->x0; fh = sr->y1 - sr->y0;
        ss = scratch(src, fw, fh, vf, D3D11_BIND_SHADER_RESOURCE);
        if (ss) {
            D3D11_BOX b = {sr->x0, sr->y0, 0, sr->x1, sr->y1, 1};
            ID3D11DeviceContext_CopySubresourceRegion(real, (ID3D11Resource *)ss, 0, 0, 0, 0, src, ssub, &b);
            from = (ID3D11Resource *)ss; fmip = 0;
            fr.x0 = fr.y0 = 0; fr.x1 = fw; fr.y1 = fh;
        } else ok = 0;
    }
    if (ok && !ddirect) {   /* в приёмник нельзя рисовать: рисуем во временную, из неё копия 1:1 */
        sd = scratch(dst, dr->x1 - dr->x0, dr->y1 - dr->y0, vf, D3D11_BIND_RENDER_TARGET);
        if (sd) {
            to = (ID3D11Resource *)sd; tmip = 0;
            tr.x0 = tr.y0 = 0; tr.x1 = dr->x1 - dr->x0; tr.y1 = dr->y1 - dr->y0;
        } else ok = 0;
    }
    ok = ok && blit(real, to, tmip, &tr, from, fmip, &fr, fw, fh, vf);
    if (ok && sd) ID3D11DeviceContext_CopySubresourceRegion(real, dst, dsub, dr->x0, dr->y0, 0, (ID3D11Resource *)sd, 0, NULL);
    scratch_done(ss);
    scratch_done(sd);
    return ok;
}

/* 1 — растянуто; 0 — не подходит. dst увеличен с коэффициентом k, x,y логические; box — в логических текселях src.
 * Для своих целей мода (мир -> экран): обе стороны должны допускать рисование напрямую. Приёмник может быть
 * многовыборочным (экран игры с MSAA), источник — нет: его выборки сводит вызывающий. */
int copy_stretch(ID3D11DeviceContext *real, ID3D11Resource *dst, Ratio k, UINT x, UINT y, ID3D11Resource *src,
                 const D3D11_BOX *box)
{
    Tex ts, td;
    if (!tex_get(dst, &td) || !tex_get(src, &ts)) return 0;
    if (!(td.d.BindFlags & D3D11_BIND_RENDER_TARGET) || !(ts.d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        ts.d.SampleDesc.Count != 1 || td.d.ArraySize != 1 || ts.d.ArraySize != 1)
        return 0;
    UINT l = box ? box->left : 0, t = box ? box->top : 0, r = box ? box->right : ts.d.Width, bt = box ? box->bottom : ts.d.Height;
    if (r <= l || bt <= t) return 1;
    UINT slw, slh, srw, srh;
    res_mip_size(&ts.si, 0, &slw, &slh, &srw, &srh);
    Rc sr = {res_edge(&ts.si, 0, 0, l), res_edge(&ts.si, 0, 1, t), res_edge(&ts.si, 0, 0, r), res_edge(&ts.si, 0, 1, bt)};
    Rc dr = {(UINT)to_phys(k, x), (UINT)to_phys(k, y), (UINT)to_phys(k, x + (r - l)), (UINT)to_phys(k, y + (bt - t))};
    return blit(real, dst, 0, &dr, src, 0, &sr, srw, srh, fmt_view(ts.d.Format, td.d.Format));
}

static void fwd_region(ID3D11DeviceContext *c, int one, UINT flags, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z,
                       ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
    /* вторая функция — слот ID3D11DeviceContext1 той же таблицы настоящего контекста (как у переходников обёртки) */
    if (one)
        ((PFN_CSR1)((ID3D11DeviceContext1 *)RC(c))->lpVtbl->CopySubresourceRegion1)(RC(c), dst, dsub, x, y, z, src, ssub, box, flags);
    else
        ORIGC(c, CopySubresourceRegion, PFN_CSR)(RC(c), dst, dsub, x, y, z, src, ssub, box);
}

static void said(volatile LONG *n, const char *what, const Tex *ts, const Tex *td, const Rc *sr, const Rc *dr)
{
    LONG k = InterlockedIncrement(n);
    if (k > 8) return;
    LOG("copy: %s: src fmt=%d samples=%u bind=0x%x real [%u,%u %ux%u], dst fmt=%d samples=%u bind=0x%x real [%u,%u %ux%u]%s", what,
        ts->d.Format, ts->d.SampleDesc.Count, ts->d.BindFlags, sr->x0, sr->y0, sr->x1 - sr->x0, sr->y1 - sr->y0, td->d.Format,
        td->d.SampleDesc.Count, td->d.BindFlags, dr->x0, dr->y0, dr->x1 - dr->x0, dr->y1 - dr->y0,
        k == 8 ? " (further lines of this kind are not logged)" : "");
}

/* копия области между двумя Texture2D, хотя бы одна из которых увеличена. Координаты игры — логические. */
static void copy_tex(ID3D11DeviceContext *c, int one, UINT flags, ID3D11Resource *dst, const Tex *td, UINT dsub, UINT x, UINT y,
                     UINT z, ID3D11Resource *src, const Tex *ts, UINT ssub, const D3D11_BOX *box)
{
    static volatile LONG n_crop, n_skip, n_conv;
    UINT smip = ssub % ts->d.MipLevels, dmip = dsub % td->d.MipLevels, slw, slh, srw, srh, dlw, dlh, drw, drh;
    res_mip_size(&ts->si, smip, &slw, &slh, &srw, &srh);
    res_mip_size(&td->si, dmip, &dlw, &dlh, &drw, &drh);
    /* область игры, обрезанная по обеим текстурам (за краями копия не определена и без мода) */
    UINT l = box ? box->left : 0, t = box ? box->top : 0, r = box ? box->right : slw, b = box ? box->bottom : slh;
    if (r > slw) r = slw;
    if (b > slh) b = slh;
    if (l >= r || t >= b || x >= dlw || y >= dlh) return;
    if (r - l > dlw - x) r = l + (dlw - x);
    if (b - t > dlh - y) b = t + (dlh - y);
    int whole = l == 0 && t == 0 && r == slw && b == slh && x == 0 && y == 0 && slw == dlw && slh == dlh;
    Rc sr = {res_edge(&ts->si, smip, 0, l), res_edge(&ts->si, smip, 1, t), res_edge(&ts->si, smip, 0, r), res_edge(&ts->si, smip, 1, b)};
    Rc dr = {res_edge(&td->si, dmip, 0, x), res_edge(&td->si, dmip, 1, y), res_edge(&td->si, dmip, 0, x + (r - l)),
             res_edge(&td->si, dmip, 1, y + (b - t))};
    /* мир меньше логического (коэффициент < 1): область в 1 тексель не должна схлопнуться в пустую */
    if (sr.x1 <= sr.x0) { if (sr.x0 >= srw) sr.x0 = srw - 1; sr.x1 = sr.x0 + 1; }
    if (sr.y1 <= sr.y0) { if (sr.y0 >= srh) sr.y0 = srh - 1; sr.y1 = sr.y0 + 1; }
    if (dr.x1 <= dr.x0) { if (dr.x0 >= drw) dr.x0 = drw - 1; dr.x1 = dr.x0 + 1; }
    if (dr.y1 <= dr.y0) { if (dr.y0 >= drh) dr.y0 = drh - 1; dr.y1 = dr.y0 + 1; }
    int ms = ts->d.SampleDesc.Count > 1 || td->d.SampleDesc.Count > 1;
    /* копия 1:1 верна, если обе увеличены одинаково (края округляются порознь — ширины могут разойтись на 1 px,
     * берём область источника) или настоящие размеры областей совпали (коэффициент мира 1) */
    int same = (ts->scaled && td->scaled && ratio_eq(ts->si.r, td->si.r)) ||
               (sr.x1 - sr.x0 == dr.x1 - dr.x0 && sr.y1 - sr.y0 == dr.y1 - dr.y0);

    if (!same && !ms && (g_cfg.stretch_copy || ts->scaled) && convert(RC(c), dst, td, dsub, &dr, src, ts, ssub, &sr)) {
        if ((!(ts->d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(td->d.BindFlags & D3D11_BIND_RENDER_TARGET)) &&
            InterlockedIncrement(&n_conv) <= 8)
            LOG("copy: converted through a temporary texture: src real %ux%u bind=0x%x usage=%d -> dst real %ux%u bind=0x%x usage=%d",
                sr.x1 - sr.x0, sr.y1 - sr.y0, ts->d.BindFlags, ts->d.Usage, dr.x1 - dr.x0, dr.y1 - dr.y0, td->d.BindFlags, td->d.Usage);
        return;
    }
    /* Г2: копия подресурса целиком при равных настоящих размерах — без области, как её дала игра. Буфер глубины (и
     * MSAA) D3D копирует только целиком: с областью, даже во весь размер, вызов недопустим и молча отбрасывается.
     * Игра без HDR так копирует глубину сцены для затенения HBAO+ — копия оставалась пустой, мир выглядел плоско. */
    if (same && whole && srw == drw && srh == drh && ts->d.SampleDesc.Count == td->d.SampleDesc.Count) {
        fwd_region(c, one, flags, dst, dsub, 0, 0, z, src, ssub, NULL);
        return;
    }
    if (ms) {
        /* MSAA копируется только целиком и только в такую же: иначе в D3D не передаём */
        if (ts->d.SampleDesc.Count == td->d.SampleDesc.Count && whole && srw == drw && srh == drh) {
            fwd_region(c, one, flags, dst, dsub, 0, 0, z, src, ssub, NULL);
            return;
        }
        said(&n_skip, "MSAA textures of different real size or sample count - skipped", ts, td, &sr, &dr);
        return;
    }
    /* копия 1:1 в настоящих размерах: область источника в точку приёмника, лишнее обрезано */
    UINT cw = sr.x1 - sr.x0, ch = sr.y1 - sr.y0;
    if (cw > drw - dr.x0) cw = drw - dr.x0;
    if (ch > drh - dr.y0) ch = drh - dr.y0;
    if (!same) said(&n_crop, "cannot convert, copied 1:1 what fits", ts, td, &sr, &dr);
    D3D11_BOX pb = {sr.x0, sr.y0, 0, sr.x0 + cw, sr.y0 + ch, 1};
    fwd_region(c, one, flags, dst, dsub, dr.x0, dr.y0, z, src, ssub, &pb);
}

static inline void region(ID3D11DeviceContext *c, int one, UINT flags, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z,
                          ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
    src = world_read(c, dst, src);   /* мир без HDR: back buffer пока в цели мира */
    if (g_cap) diag_copy(c, one ? "CopySubresourceRegion1" : "CopySubresourceRegion", dst, src);
    Tex ts, td;
    if ((res_scaled(src, NULL) || res_scaled(dst, NULL)) && tex_get(src, &ts) && tex_get(dst, &td)) {
        copy_tex(c, one, flags, dst, &td, dsub, x, y, z, src, &ts, ssub, box);
        return;
    }
    fwd_region(c, one, flags, dst, dsub, x, y, z, src, ssub, box);
}

static void STDMETHODCALLTYPE hk_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub, UINT x,
                                                       UINT y, UINT z, ID3D11Resource *src, UINT ssub,
                                                       const D3D11_BOX *box)
{
    region(c, 0, 0, dst, dsub, x, y, z, src, ssub, box);
}

static void STDMETHODCALLTYPE hk_CopySubresourceRegion1(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub, UINT x,
                                                        UINT y, UINT z, ID3D11Resource *src, UINT ssub,
                                                        const D3D11_BOX *box, UINT flags)
{
    region(c, 1, flags, dst, dsub, x, y, z, src, ssub, box);
}

static void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    src = world_read(c, dst, src);
    if (g_cap) diag_copy(c, "CopyResource", dst, src);
    Tex ts, td;
    if ((res_scaled(src, NULL) || res_scaled(dst, NULL)) && tex_get(src, &ts) && tex_get(dst, &td)) {
        UINT slw, slh, srw, srh, dlw, dlh, drw, drh;
        res_mip_size(&ts.si, 0, &slw, &slh, &srw, &srh);
        res_mip_size(&td.si, 0, &dlw, &dlh, &drw, &drh);
        if (srw != drw || srh != drh || ts.d.SampleDesc.Count != td.d.SampleDesc.Count) {
            /* настоящие размеры разные: по уровням, каждый — как копия области целиком */
            if (ts.d.ArraySize != 1 || td.d.ArraySize != 1) {
                static volatile LONG once;
                if (!InterlockedExchange(&once, 1))
                    LOG("copy: CopyResource between texture arrays of different real size (%ux%u, %ux%u) - skipped", srw, srh, drw, drh);
                return;
            }
            UINT n = ts.d.MipLevels < td.d.MipLevels ? ts.d.MipLevels : td.d.MipLevels;
            for (UINT m = 0; m < n; m++) copy_tex(c, 0, 0, dst, &td, m, 0, 0, 0, src, &ts, m, NULL);
            return;
        }
    }
    ORIGC(c, CopyResource, PFN_CR)(RC(c), dst, src);
}

/* В0: сведение выборок (ResolveSubresource) между текстурами разного настоящего размера. D3D требует один размер;
 * 1 — сделано модом (или отброшено), 0 — размеры сходятся, вызов можно передать как есть.
 * own — источник подменён целью мира (world.c): её рисуем сами и при равных размерах, как было проверено в игре. */
int copy_resolve(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub, ID3D11Resource *src, UINT ssub, DXGI_FORMAT f, int own)
{
    static volatile LONG n_said, n_skip;
    Tex ts, td;
    if (!(res_scaled(src, NULL) || res_scaled(dst, NULL)) || !tex_get(src, &ts) || !tex_get(dst, &td)) return 0;
    UINT slw, slh, srw, srh, dlw, dlh, drw, drh;
    res_mip_size(&ts.si, ssub % ts.d.MipLevels, &slw, &slh, &srw, &srh);
    res_mip_size(&td.si, dsub % td.d.MipLevels, &dlw, &dlh, &drw, &drh);
    Rc sr = {0, 0, srw, srh}, dr = {0, 0, drw, drh};
    if (srw == drw && srh == drh)
        return own && ts.d.SampleDesc.Count == 1 && convert(RC(c), dst, &td, dsub, &dr, src, &ts, ssub, &sr);
    if (InterlockedIncrement(&n_said) <= 8)
        LOG("resolve: real sizes differ (src %ux%u samples=%u, dst %ux%u) - converted by the mod", srw, srh, ts.d.SampleDesc.Count,
            drw, drh);
    int ok = 0;
    if (ts.d.SampleDesc.Count > 1) {
        /* свести во временную обычную текстуру настоящего размера источника, из неё — с пересчётом */
        DXGI_FORMAT tf = fmt_view(f, f) ? f : fmt_view(ts.d.Format, td.d.Format);
        ID3D11Texture2D *tmp = tf ? scratch(src, srw, srh, tf, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) : NULL;
        Tex tt;
        if (tmp && tex_get((ID3D11Resource *)tmp, &tt)) {
            typedef void(STDMETHODCALLTYPE *PFN_RS)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT, DXGI_FORMAT);
            ORIGC(c, ResolveSubresource, PFN_RS)(RC(c), (ID3D11Resource *)tmp, 0, src, ssub, tf);
            ok = convert(RC(c), dst, &td, dsub, &dr, (ID3D11Resource *)tmp, &tt, 0, &sr);
        }
        scratch_done(tmp);
    } else {
        ok = convert(RC(c), dst, &td, dsub, &dr, src, &ts, ssub, &sr);   /* источник без MSAA: игра сводит «как копию» */
    }
    if (!ok) said(&n_skip, "resolve between different real sizes cannot be converted - skipped", &ts, &td, &sr, &dr);
    return 1;
}

void copy_install(void)
{
    CTX_HOOK(CopySubresourceRegion, hk_CopySubresourceRegion);
    CTX_HOOK(CopySubresourceRegion1, hk_CopySubresourceRegion1);
    CTX_HOOK(CopyResource, hk_CopyResource);
}
