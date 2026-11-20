/* Разведка для F14: снимок кадра в ui4k.log. diag=1 в ini, в игре Ctrl+F11 — пишутся два следующих кадра
 * (от Present до Present): каждое рисование/dispatch/очистка/копия с целями, текстурами, шейдером, сэмплером, viewport.
 * Цель — понять цепочку кадра: где кончается 3D-сцена, как попадает в back buffer, куда рисуется интерфейс.
 * diag_at=N — снимок сам на N-м Present (тесты).
 * diag_on=WxH — снимок сам, когда игра создаёт кэш интерфейса этого логического размера (иконки инвентаря игра рисует
 * один раз, при открытии окна — Ctrl+F11 их не застаёт). До 3 раз за запуск (например, до и после смены масштаба);
 * 6 кадров с этого места.
 * Ресурс: id:WxH[*] fN — логический размер, * = увеличен модом, N = DXGI_FORMAT. Без diag=1 ничего не ставится. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "common.h"

volatile LONG g_cap;          /* 1 = идёт снимок */
static volatile LONG g_frames; /* сколько Present осталось до конца снимка */
static volatile LONG g_seq;    /* номер события */
static volatile LONG g_presents; /* для diag_at */

/* ---- 20.11: «запись прошлого» (diag_ring=МБ; «часть мира на краткое мгновение промигивает поверх основной картинки»).
 * Мигание длится кадр-два, а снимок по Ctrl+F11 начинается после нажатия — уже без него. В этом режиме снимок идёт всё
 * время, но строки уходят не в лог, а в кольцевой буфер в памяти (новые затирают самые старые); Ctrl+F11 (или diag_at=N —
 * на N-м кадре, тесты) пишет буфер в ui4k_ring_N.txt рядом с DLL: последние секунды кадров до нажатия. Игра заметно
 * медленнее (каждое рисование описывается текстом) — режим только для поиска ошибок. ---- */
static char *g_ring;                /* NULL — режима нет, строки снимка идут в лог */
static SIZE_T g_rsz;
static ULONGLONG g_rpos;            /* сколько байт записано всего */
static SRWLOCK g_rlk = SRWLOCK_INIT;
static volatile LONG g_frame_no, g_dumps;

static void ring_write(const char *p, SIZE_T n)
{
    while (n) {
        SIZE_T off = (SIZE_T)(g_rpos % g_rsz), k = g_rsz - off < n ? g_rsz - off : n;
        memcpy(g_ring + off, p, k);
        g_rpos += k; p += k; n -= k;
    }
}

/* строка снимка: в буфер (с номером потока) или в лог */
static void emit(const char *line)
{
    if (!g_ring) { ui4k_log("%s", line); return; }
    char h[24];
    int k = snprintf(h, sizeof h, "[%lu] ", GetCurrentThreadId());
    SIZE_T n = strlen(line);
    AcquireSRWLockExclusive(&g_rlk);
    ring_write(h, (SIZE_T)k);
    ring_write(line, n);
    ring_write("\n", 1);
    ReleaseSRWLockExclusive(&g_rlk);
}

static void ring_dump(void)
{
    AcquireSRWLockExclusive(&g_rlk);
    ULONGLONG pos = g_rpos;
    SIZE_T n = pos < g_rsz ? (SIZE_T)pos : g_rsz;
    char *tmp = (char *)HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (tmp && pos <= g_rsz) memcpy(tmp, g_ring, n);
    else if (tmp) {
        SIZE_T off = (SIZE_T)(pos % g_rsz);
        memcpy(tmp, g_ring + off, g_rsz - off);
        memcpy(tmp + (g_rsz - off), g_ring, off);
    }
    ReleaseSRWLockExclusive(&g_rlk);
    if (!tmp) { LOG("diag ring: no memory to write the record"); return; }
    SIZE_T s = 0;
    if (pos > g_rsz) { while (s < n && tmp[s] != '\n') s++; if (s < n) s++; }   /* самая старая строка обрезана — с следующей */
    LONG k = InterlockedIncrement(&g_dumps);
    WCHAR name[40], path[MAX_PATH];
    swprintf(name, 40, L"ui4k_ring_%ld.txt", (long)k);
    FILE *f = dir_file(path, name) ? _wfopen(path, L"wb") : NULL;
    if (f) { fwrite(tmp + s, 1, n - s, f); fclose(f); }
    HeapFree(GetProcessHeap(), 0, tmp);
    LOG("diag ring: %.1f MB of the last frames (up to frame %ld) %s %ls (Ctrl+F11 #%ld)", (double)(n - s) / 1048576.0,
        (long)g_frame_no, f ? "written to" : "could NOT be written to", name, (long)k);
}

static void ring_init(void)
{
    if (g_ring || g_cfg.diag_ring <= 0) return;
    SIZE_T sz = (SIZE_T)g_cfg.diag_ring << 20;
    g_ring = (char *)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_ring) { LOG("diag ring: %d MB not available - Ctrl+F11 snapshots as usual", g_cfg.diag_ring); return; }
    g_rsz = sz;
    InterlockedExchange(&g_cap, 1);   /* снимок идёт всё время — в буфер */
    LOG("diag ring: the last %d MB of frames are kept in memory - Ctrl+F11 writes them to ui4k_ring_N.txt (the game is slower)",
        g_cfg.diag_ring);
}

static const char *ctx_tag(ID3D11DeviceContext *c)
{
    return CTXW(c)->type == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "I" : "D";
}

static void res_str(ID3D11Resource *r, char *b, size_t n)
{
    if (!r) { snprintf(b, n, "-"); return; }
    unsigned id = (unsigned)((ULONG_PTR)r & 0xffffffu);
    ID3D11Texture2D *t = NULL;
    if (SUCCEEDED(ID3D11Resource_QueryInterface(r, &IID_ID3D11Texture2D, (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(t, &d);   /* из нашей DLL — логический размер */
        snprintf(b, n, "%06x:%ux%u%s f%d%s", id, d.Width, d.Height, res_scaled(r, NULL) ? "*" : "", d.Format,
                 d.ArraySize > 1 ? "[arr]" : "");
        ID3D11Texture2D_Release(t);
        return;
    }
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    snprintf(b, n, "%06x:dim%d", id, dim);
}

static void view_str(ID3D11View *v, char *b, size_t n)
{
    if (!v) { snprintf(b, n, "-"); return; }
    ID3D11Resource *r = NULL;
    ID3D11View_GetResource(v, &r);
    res_str(r, b, n);
    if (r) ID3D11Resource_Release(r);
}


/* ---- разбор байткода шейдеров (DXBC SHEX/SHDR): чем шейдер читает текстуры ----
 * S = sample/gather (по UV — растяжение бесплатно), L = ld/ld_ms (по номеру пикселя — при R != M читает не то),
 * R = resinfo (сам узнаёт размер текстуры), P = читает позицию пикселя (SV_Position). */
#define SH_S 1
#define SH_L 2
#define SH_R 4
#define SH_P 8
#define SHMAP 16384
static struct { void *k; int f; } g_sh[SHMAP];
static SRWLOCK g_shl = SRWLOCK_INIT;

static int sh_scan(const void *code, SIZE_T len)
{
    const BYTE *b = (const BYTE *)code;
    if (!b || len < 32 || memcmp(b, "DXBC", 4)) return 0;
    UINT nch = *(const UINT *)(b + 28);
    for (UINT i = 0; i < nch && 32 + i * 4 + 4 <= len; i++) {
        UINT off = *(const UINT *)(b + 32 + i * 4);
        if (off + 8 > len) continue;
        if (memcmp(b + off, "SHEX", 4) && memcmp(b + off, "SHDR", 4)) continue;
        const UINT *t = (const UINT *)(b + off + 8);
        UINT n = t[1], f = 0;
        if (off + 8 + (SIZE_T)n * 4 > len) return 0;
        for (UINT p = 2; p < n;) {
            UINT op = t[p] & 0x7ff, l = (t[p] >> 24) & 0x7f;
            if (op == 0x35) l = p + 1 < n ? t[p + 1] : 0;   /* customdata: длина в следующем слове */
            if (op >= 0x45 && op <= 0x4a) f |= SH_S;           /* sample, sample_c, _lz, _l, _d, _b */
            if (op == 0x6d || (op >= 0x7e && op <= 0x80)) f |= SH_S;   /* gather4 */
            if (op == 0x2d || op == 0x2e) f |= SH_L;           /* ld, ld_ms */
            if (op == 0x3d) f |= SH_R;                         /* resinfo */
            if (op == 0x64) f |= SH_P;                         /* dcl_input_ps_siv (позиция) */
            if (!l) break;
            p += l;
        }
        return f | 0x100;
    }
    return 0;
}

static void sh_put(void *k, int f)
{
    if (!k) return;
    AcquireSRWLockExclusive(&g_shl);
    for (UINT i = (UINT)(((ULONG_PTR)k >> 4) * 2654435761u) % SHMAP, j = 0; j < SHMAP; j++, i = (i + 1) % SHMAP)
        if (!g_sh[i].k || g_sh[i].k == k) { g_sh[i].k = k; g_sh[i].f = f; break; }
    ReleaseSRWLockExclusive(&g_shl);
}

static int sh_get(void *k)
{
    int f = -1;
    if (!k) return f;
    AcquireSRWLockShared(&g_shl);
    for (UINT i = (UINT)(((ULONG_PTR)k >> 4) * 2654435761u) % SHMAP, j = 0; j < SHMAP && g_sh[i].k; j++, i = (i + 1) % SHMAP)
        if (g_sh[i].k == k) { f = g_sh[i].f; break; }
    ReleaseSRWLockShared(&g_shl);
    return f;
}

static const char *sh_flags(void *k, char *b)
{
    int f = sh_get(k);
    if (f < 0) { strcpy(b, "?"); return b; }
    int n = 0;
    if (f & SH_S) b[n++] = 'S';
    if (f & SH_L) b[n++] = 'L';
    if (f & SH_R) b[n++] = 'R';
    if (f & SH_P) b[n++] = 'P';
    if (!n) b[n++] = '-';
    b[n] = 0;
    return b;
}

static VtHook h_cps, h_ccs, h_cvs;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSH)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, void **);
static HRESULT STDMETHODCALLTYPE hk_CreatePixelShader(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, void **o)
{
    HRESULT hr = VT_ORIG(d, &h_cps, PFN_CSH)(d, c, n, l, o);
    if (SUCCEEDED(hr) && o) sh_put(*o, sh_scan(c, n));
    return hr;
}
/* Текст каждого compute- и вершинного шейдера — в ui4k_cs.txt / ui4k_vs.txt ("=== cs|vs XXXXXX" — тот же id, что в снимке).
 * Дизассемблер — системный d3dcompiler_47 (System32), не копия из папки игры. */
typedef HRESULT(WINAPI *PFN_DISASM)(const void *, SIZE_T, UINT, const char *, ID3DBlob **);
static SRWLOCK g_csl = SRWLOCK_INIT;
static void sh_dump(const char *tag, const WCHAR *file, int *opened, void *sh, const void *code, SIZE_T len)
{
    static PFN_DISASM dis;
    static int tried;
    AcquireSRWLockExclusive(&g_csl);
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        dis = m ? (PFN_DISASM)GetProcAddress(m, "D3DDisassemble") : NULL;
        if (!dis) LOG("diag: D3DDisassemble not available, no shader text");
    }
    ID3DBlob *b = NULL;
    if (dis && SUCCEEDED(dis(code, len, 0, NULL, &b)) && b) {
        WCHAR p[MAX_PATH];
        FILE *f = dir_file(p, file) ? _wfopen(p, (*opened)++ ? L"ab" : L"wb") : NULL;
        if (f) {
            const BYTE *h = (const BYTE *)code + 4;   /* DXBC: 16 байт контрольной суммы — постоянный id шейдера */
            fprintf(f, "=== %s %06x len %u hash %02x%02x%02x%02x%02x%02x%02x%02x\r\n", tag, (unsigned)((ULONG_PTR)sh & 0xffffff),
                    (unsigned)len, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
            fwrite(b->lpVtbl->GetBufferPointer(b), 1, strnlen(b->lpVtbl->GetBufferPointer(b), b->lpVtbl->GetBufferSize(b)), f);
            fputs("\r\n", f);
            fclose(f);
        }
        b->lpVtbl->Release(b);
    }
    ReleaseSRWLockExclusive(&g_csl);
}
static int g_cs_opened, g_vs_opened;

static HRESULT STDMETHODCALLTYPE hk_CreateComputeShader(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, void **o)
{
    HRESULT hr = VT_ORIG(d, &h_ccs, PFN_CSH)(d, c, n, l, o);
    if (SUCCEEDED(hr) && o) { sh_put(*o, sh_scan(c, n)); sh_dump("cs", L"ui4k_cs.txt", &g_cs_opened, *o, c, n); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_CreateVertexShader(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, void **o)
{
    HRESULT hr = VT_ORIG(d, &h_cvs, PFN_CSH)(d, c, n, l, o);
    if (SUCCEEDED(hr) && o) { sh_put(*o, sh_scan(c, n)); sh_dump("vs", L"ui4k_vs.txt", &g_vs_opened, *o, c, n); }
    return hr;
}

/* семплеры игры: смещение уровня детальности (MipLODBias) и пределы — первые 300 в лог */
static VtHook h_css;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSS)(ID3D11Device *, const D3D11_SAMPLER_DESC *, ID3D11SamplerState **);
static HRESULT STDMETHODCALLTYPE hk_CreateSamplerState(ID3D11Device *d, const D3D11_SAMPLER_DESC *sd, ID3D11SamplerState **o)
{
    HRESULT hr = VT_ORIG(d, &h_css, PFN_CSS)(d, sd, o);
    static volatile LONG n;
    if (sd && InterlockedIncrement(&n) <= 300)
        ui4k_log("diag: CreateSamplerState filter=0x%x aniso=%u bias=%.2f lod=%.1f..%.1f -> %06x", sd->Filter, sd->MaxAnisotropy,
                 sd->MipLODBias, sd->MinLOD, sd->MaxLOD, (unsigned)((ULONG_PTR)(o && SUCCEEDED(hr) ? *o : NULL) & 0xffffff));
    return hr;
}

void diag_hook_device(ID3D11Device *dev)
{
    ring_init();
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateSamplerState, hk_CreateSamplerState, &h_css);
    VT_PATCH(dev, ID3D11DeviceVtbl, CreatePixelShader, hk_CreatePixelShader, &h_cps);
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateComputeShader, hk_CreateComputeShader, &h_ccs);
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateVertexShader, hk_CreateVertexShader, &h_cvs);
}

static int cb_list(ID3D11Buffer **cb, int n, char *b, size_t sz)
{
    int p = 0;
    for (int i = 0; i < n; i++) if (cb[i]) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc(cb[i], &d);
        p += snprintf(b + p, sz - p, " %d=%06x/%u", i, (unsigned)((ULONG_PTR)cb[i] & 0xffffff), d.ByteWidth / 4);
        ID3D11Buffer_Release(cb[i]);
    }
    if (!p) b[0] = 0;
    return p;
}

#define AP(...) do { int n_ = snprintf(line + p, sizeof line - p, __VA_ARGS__); if (n_ > 0) p += n_;                         if (p >= (int)sizeof line) p = (int)sizeof line - 1; } while (0)
/* текущее состояние вывода и пиксельного шейдера на настоящем контексте */
void diag_draw(ID3D11DeviceContext *c, const char *what, UINT a, UINT b, int compute)
{
    ID3D11DeviceContext *r = RC(c);
    char line[4096], s[96];
    int p = snprintf(line, sizeof line, "#%ld %s %s %u/%u", InterlockedIncrement(&g_seq), ctx_tag(c), what, a, b);
    if (compute) {
        ID3D11ComputeShader *cs = NULL;
        ID3D11DeviceContext_CSGetShader(r, &cs, NULL, NULL);
        char fl[8];
        AP(" cs=%06x[%s] uav=[", (unsigned)((ULONG_PTR)cs & 0xffffff), sh_flags(cs, fl));
        if (cs) ID3D11ComputeShader_Release(cs);
        ID3D11UnorderedAccessView *u[4] = {0};
        ID3D11DeviceContext_CSGetUnorderedAccessViews(r, 0, 4, u);
        for (int i = 0; i < 4; i++) if (u[i]) { view_str((ID3D11View *)u[i], s, sizeof s); AP(" %d=%s", i, s); ID3D11UnorderedAccessView_Release(u[i]); }
        ID3D11ShaderResourceView *t[6] = {0};
        ID3D11DeviceContext_CSGetShaderResources(r, 0, 6, t);
        AP(" ] srv=[");
        for (int i = 0; i < 6; i++) if (t[i]) { view_str((ID3D11View *)t[i], s, sizeof s); AP(" %d=%s", i, s); ID3D11ShaderResourceView_Release(t[i]); }
        AP(" ]");
        emit(line);
        return;
    }
    ID3D11RenderTargetView *rt[8] = {0};
    ID3D11DepthStencilView *ds = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(r, 8, rt, &ds);
    AP(" rt=[");
    for (int i = 0; i < 8; i++) if (rt[i]) { view_str((ID3D11View *)rt[i], s, sizeof s); AP(" %d=%s", i, s); ID3D11RenderTargetView_Release(rt[i]); }
    view_str((ID3D11View *)ds, s, sizeof s);
    AP(" ] ds=%s", s);
    if (ds) ID3D11DepthStencilView_Release(ds);
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_VSGetShader(r, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(r, &ps, NULL, NULL);
    char fv[8], fp[8], cbs[256];
    AP(" vs=%06x[%s] ps=%06x[%s]", (unsigned)((ULONG_PTR)vs & 0xffffff),
                  sh_flags(vs, fv), (unsigned)((ULONG_PTR)ps & 0xffffff), sh_flags(ps, fp));
    ID3D11Buffer *cb[6] = {0};
    ID3D11DeviceContext_VSGetConstantBuffers(r, 0, 6, cb);
    if (cb_list(cb, 6, cbs, sizeof cbs)) AP(" vcb=[%s ]", cbs);
    ID3D11DeviceContext_PSGetConstantBuffers(r, 0, 6, cb);
    if (cb_list(cb, 6, cbs, sizeof cbs)) AP(" pcb=[%s ]", cbs);
    ID3D11DepthStencilState *dss = NULL;
    UINT sref = 0;
    ID3D11DeviceContext_OMGetDepthStencilState(r, &dss, &sref);
    if (dss) {
        D3D11_DEPTH_STENCIL_DESC dd;
        ID3D11DepthStencilState_GetDesc(dss, &dd);
        AP(" z=%d%s%s", dd.DepthEnable, dd.DepthEnable && dd.DepthWriteMask ? "w" : "",
                      dd.StencilEnable ? "s" : "");
        ID3D11DepthStencilState_Release(dss);
    } else {
        AP(" z=1(def)");
    }
    if (vs) ID3D11VertexShader_Release(vs);
    if (ps) ID3D11PixelShader_Release(ps);
    ID3D11ShaderResourceView *t[8] = {0};
    ID3D11DeviceContext_PSGetShaderResources(r, 0, 8, t);
    AP(" srv=[");
    for (int i = 0; i < 8; i++) if (t[i]) {
        view_str((ID3D11View *)t[i], s, sizeof s);
        AP(" %d=%s", i, s);
        /* детальность текстуры, которую игра сама ограничила: MinLOD ресурса и первый мип вида (подгрузка текстур) */
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11ShaderResourceView_GetDesc(t[i], &vd);
        if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && vd.Texture2D.MostDetailedMip)
            AP("@mip%u+%u", vd.Texture2D.MostDetailedMip, vd.Texture2D.MipLevels);
        if (CTXW(c)->type == D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            ID3D11Resource *rr = NULL;
            ID3D11ShaderResourceView_GetResource(t[i], &rr);
            if (rr) {
                FLOAT ml = ID3D11DeviceContext_GetResourceMinLOD(r, rr);
                if (ml > 0) AP("@lod%.2f", ml);
                ID3D11Resource_Release(rr);
            }
        }
        ID3D11ShaderResourceView_Release(t[i]);
    }
    AP(" ]");
    ID3D11SamplerState *sm[4] = {0};
    ID3D11DeviceContext_PSGetSamplers(r, 0, 4, sm);
    for (int i = 0; i < 4; i++) if (sm[i]) {
        D3D11_SAMPLER_DESC sd;
        ID3D11SamplerState_GetDesc(sm[i], &sd);
        AP(" smp%d=0x%x", i, sd.Filter);
        if (sd.Filter == D3D11_FILTER_ANISOTROPIC) AP("a%u", sd.MaxAnisotropy);
        /* смещение и пределы уровня детальности, если игра их задала */
        if (sd.MipLODBias != 0 || sd.MinLOD > 0 || sd.MaxLOD < 1000) AP("(bias%.2f lod%.1f..%.1f)", sd.MipLODBias, sd.MinLOD, sd.MaxLOD);
        ID3D11SamplerState_Release(sm[i]);
    }
    D3D11_VIEWPORT v;
    UINT nv = 1;
    ID3D11DeviceContext_RSGetViewports(r, &nv, &v);   /* настоящий контекст — физический viewport */
    if (nv) AP(" vp=%.0f,%.0f %.0fx%.0f(phys)", v.TopLeftX, v.TopLeftY, v.Width, v.Height);
    D3D11_RECT sr;
    UINT nsr = 1;
    ID3D11DeviceContext_RSGetScissorRects(r, &nsr, &sr);   /* 20.11: ножницы — кусок мира не на своём месте */
    if (nsr) AP(" sc=%ld,%ld-%ld,%ld", sr.left, sr.top, sr.right, sr.bottom);
    emit(line);
}

void diag_copy(ID3D11DeviceContext *c, const char *what, ID3D11Resource *dst, ID3D11Resource *src)
{
    char a[96], b[96], line[300];
    res_str(dst, a, sizeof a);
    res_str(src, b, sizeof b);
    snprintf(line, sizeof line, "#%ld %s %s dst=%s src=%s", InterlockedIncrement(&g_seq), ctx_tag(c), what, a, b);
    emit(line);
}

void diag_note(ID3D11DeviceContext *c, const char *fmt, ...)
{
    char line[4096], out[4160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    snprintf(out, sizeof out, "#%ld %s %s", InterlockedIncrement(&g_seq), c ? ctx_tag(c) : "-", line);
    emit(out);
}

/* 20.11: запись игры в прочий буфер констант (не тот, что правит мод). Сцена VS (232 числа, shader_constants.h игры):
 * позиция камеры c2 и матрица вида c3..c6 — числа 8..27; остальные — первые 16 чисел (положение объекта, кости) */
void diag_cb_raw(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n)
{
    if (!f || !n) return;
    UINT from = n == 232 ? 8 : 0, to = n == 232 ? 28 : (n < 16 ? n : 16);
    char b[600];
    int p = snprintf(b, sizeof b, "CB%u raw %06x:", n, (unsigned)((ULONG_PTR)r & 0xffffff));
    for (UINT i = from; i < to && p < (int)sizeof b - 16; i++) p += snprintf(b + p, sizeof b - p, " %.6g", f[i]);
    diag_note(c, "%s", b);
}

/* стек вызовов здесь — адреса внутри exe (rva, «e...»), чужие модули точкой; tag — что происходит */
void diag_stack(const char *tag)
{
    void *fr[24];
    USHORT k = RtlCaptureStackBackTrace(2, 24, fr, NULL);
    const BYTE *exe = (const BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    SIZE_T sz = nt->OptionalHeader.SizeOfImage;
    char b[700];
    int p = snprintf(b, sizeof b, "diag_stack: %s thread %lu:", tag, GetCurrentThreadId());
    for (USHORT i = 0; i < k && p < (int)sizeof b - 24; i++) {
        const BYTE *a = (const BYTE *)fr[i];
        if (a >= exe && a < exe + sz) p += snprintf(b + p, sizeof b - p, " e%llx", (unsigned long long)(a - exe));
        else p += snprintf(b + p, sizeof b - p, " .");
    }
    ui4k_log("%s", b);
}

void diag_on_create(UINT w, UINT h)
{
    static volatile LONG fired;
    if (w != g_cfg.diag_on_w || h != g_cfg.diag_on_h || g_cap || InterlockedIncrement(&fired) > 3) return;   /* до 3 снимков */
    Screen s = screen_get();
    ui4k_log("=== diag: capture start (cache %ux%u created); screen logical %ldx%ld physical %ldx%ld, scale %d/%d", w, h,
             s.lw, s.lh, s.pw, s.ph, g_cfg.scale.num, g_cfg.scale.den);
    InterlockedExchange(&g_seq, 0);
    InterlockedExchange(&g_frames, 6);
    InterlockedExchange(&g_cap, 1);
}

void diag_present(void)
{
    static int was;
    int down = ui4k_hotkey(VK_F11, 1);
    if (g_ring) {   /* «запись прошлого»: граница кадра со временем; Ctrl+F11 — буфер в файл */
        LONG fno = InterlockedIncrement(&g_frame_no);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char m[80];
        snprintf(m, sizeof m, "=== frame %ld %02u:%02u:%02u.%03u", (long)fno, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        emit(m);
        if ((down && !was) || (g_cfg.diag_at && fno == g_cfg.diag_at)) ring_dump();
    } else if (g_cap) {
        LONG left = InterlockedDecrement(&g_frames);
        ui4k_log("=== diag: Present (frames left %ld)", left);
        if (left <= 0) {
            InterlockedExchange(&g_cap, 0);
            ui4k_log("=== diag: capture end");
        }
    } else if ((down && !was) || (g_cfg.diag_at && InterlockedIncrement(&g_presents) == g_cfg.diag_at)) {
        Screen s = screen_get();
        ui4k_log("=== diag: capture start; screen logical %ldx%ld physical %ldx%ld, scale %d/%d", s.lw, s.lh,
                 s.pw, s.ph, g_cfg.scale.num, g_cfg.scale.den);
        InterlockedExchange(&g_seq, 0);
        InterlockedExchange(&g_frames, g_cfg.diag_at ? 2 : 4);   /* Ctrl+F11: 4 кадра — сравнивать значения между кадрами */
        InterlockedExchange(&g_cap, 1);
    }
    was = down;

    /* плавность: Ctrl+F8 — 10 с времени кадров (между Present), в лог: FPS, худшие кадры, рывки (> 2x медианы) */
    static int was12, on;   /* was12: Ctrl+F8 (F12 занят снимками Steam) */
    static LARGE_INTEGER t0, last, f;
    static float ft[4096];
    static int nft;
    int d12 = ui4k_hotkey(VK_F8, 1);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (d12 && !was12 && !on) {
        QueryPerformanceFrequency(&f);
        t0 = last = now; nft = 0; on = 1;
        Screen s = screen_get();
        ui4k_log("=== frametime: start (10 s); screen logical %ldx%ld, interface %d/%d, world %d/%d", s.lw, s.lh,
                 g_cfg.scale.num, g_cfg.scale.den, g_cfg.scene.num, g_cfg.scene.den);
    } else if (on) {
        if (nft < 4096) ft[nft++] = (float)((now.QuadPart - last.QuadPart) * 1000.0 / f.QuadPart);
        last = now;
        if ((now.QuadPart - t0.QuadPart) >= 10 * f.QuadPart || nft >= 4096) {
            on = 0;
            float srt[4096], sum = 0;
            for (int i = 0; i < nft; i++) { srt[i] = ft[i]; sum += ft[i]; }
            for (int i = 1; i < nft; i++) { float v = srt[i]; int j = i; while (j > 0 && srt[j - 1] > v) { srt[j] = srt[j - 1]; j--; } srt[j] = v; }
            float med = nft ? srt[nft / 2] : 0, p99 = nft ? srt[nft * 99 / 100] : 0, mx = nft ? srt[nft - 1] : 0;
            int hitch = 0;
            for (int i = 0; i < nft; i++) hitch += ft[i] > 2 * med;
            ui4k_log("=== frametime: %d frames, avg %.1f fps, median %.2f ms, 99%% %.2f ms, max %.2f ms, hitches (>2x median) %d",
                     nft, nft ? 1000.0 * nft / sum : 0, med, p99, mx, hitch);
        }
    }
    was12 = d12;
}

/* ---- очистки и resolve (только при diag=1); рисование — в draw.c ---- */
#define PASS(field, PFN, ...) ORIGC(c, field, PFN)(RC(c), __VA_ARGS__)
typedef void(STDMETHODCALLTYPE *PFN_CDSV)(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);

void diag_view_note(ID3D11DeviceContext *c, const char *what, ID3D11View *v)
{
    char s[96];
    view_str(v, s, sizeof s);
    diag_note(c, "%s %s", what, s);
}
static void STDMETHODCALLTYPE hk_ClearDepthStencilView(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT f, FLOAT d, UINT8 st)
{
    if (g_cap) { char s[96]; view_str((ID3D11View *)v, s, sizeof s); diag_note(c, "ClearDSV %s", s); }
    PASS(ClearDepthStencilView, PFN_CDSV, v, f, d, st);
}
/* подгрузка текстур: игра ограничивает самый подробный мип ресурса. Первые 300 вызовов, вехи и всё во время снимка */
typedef void(STDMETHODCALLTYPE *PFN_SRML)(ID3D11DeviceContext *, ID3D11Resource *, FLOAT);
static void STDMETHODCALLTYPE hk_SetResourceMinLOD(ID3D11DeviceContext *c, ID3D11Resource *res, FLOAT lod)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (g_cap || k <= 300 || !(k & (k - 1))) {
        char s[96];
        res_str(res, s, sizeof s);
        ui4k_log("diag: SetResourceMinLOD %s lod=%.2f (call %ld)", s, lod, (long)k);
    }
    PASS(SetResourceMinLOD, PFN_SRML, res, lod);
}

void diag_install(void)
{
    CTX_HOOK(ClearDepthStencilView, hk_ClearDepthStencilView);
    CTX_HOOK(SetResourceMinLOD, hk_SetResourceMinLOD);
    LOG("diag: Ctrl+F11 in game writes 2 frames to this log");
}
