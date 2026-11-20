/* F05: масштабированные ресурсы. Метка (private data) хранит логический размер mip0 — GetDesc отдаёт его игре
 * как есть, без деления (при ×1.5 деление физического неоднозначно). Сама D3D (вызов изнутри системной d3d11.dll,
 * например проверка при создании view) получает настоящее описание. */
#include <stdio.h>
#include "common.h"
#include "classify.h"
#include "origin.h"

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

/* Э2: функция движка «t:create» (буферы окон, виды eViewport, подпись у курсора) — ищется один раз в образе exe.
 * Со всеми частями по .pdata (вердикт 19.11, п. 4): MSVC выносит тело функции в части-продолжения; если после
 * обновления игры вызов CreateTexture2D окажется в такой части, основной записи мало (так было с функцией воды). */
static OriginRange g_or[ORIGIN_MAX * 8];
static int g_nor;
/* Реверс 19.11: отражение в воде — цель мира, но её тоже делает «t:create»: 0x6fea00 (квадрат DAT_1414a5eb0, флаги 3 —
 * цвет и глубина) из 0x6de9f0, которая читает «video/water_quality» и пересоздаёт отражение при смене качества воды
 * (запуск: 0x4d12b2 -> 0x6dc300 -> 0x6df7c0 -> 0x6de9f0 -> 0x6fea00 -> «t:create»). В кадре в неё ~440 рисований мира,
 * читает её шейдер воды. Игра держит её одного размера при любом разрешении — так и оставляем (не интерфейс). */
static OriginRange g_wr[ORIGIN_MAX * 8];   /* функции «video/water_quality» со всеми частями (.pdata) */
static int g_nwr;
static const BYTE *g_exe;
static volatile LONG g_or_tried;

void res_origin_init(void)
{
    if (!g_cfg.origin || InterlockedExchange(&g_or_tried, 1)) return;
    const BYTE *img = (const BYTE *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + ((const IMAGE_DOS_HEADER *)img)->e_lfanew);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    DWORD str = 0;
    OriginRange tf[ORIGIN_MAX], parts[ORIGIN_MAX * 8];
    int refs = 0, np = 0, n = origin_find(img, nt->OptionalHeader.SizeOfImage, "t:create", tf, ORIGIN_MAX, &str, &refs);
    for (int i = 0; i < n; i++)
        if (!origin_parts(img, nt->OptionalHeader.SizeOfImage, &tf[i], parts, &np, ORIGIN_MAX * 8) && np < ORIGIN_MAX * 8)
            parts[np++] = tf[i];
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (n > 0 && np > 0) {
        memcpy(g_or, parts, (size_t)np * sizeof *parts);
        g_exe = img;
        MemoryBarrier();
        g_nor = np;   /* последним: создание текстур в других потоках смотрит g_nor */
        LOG("origin: \"t:create\" at RVA 0x%lx, %d reference(s), function 0x%lx..0x%lx%s in %d part(s) (%.1f ms) - window caches of "
            "any shape are enlarged (keys=1: Ctrl+F7 in game - off/on)", str, refs, tf[0].begin, tf[0].end, n > 1 ? " and more" : "",
            np, ms);
    } else {
        LOG("origin: \"t:create\" %s in the game exe (%.1f ms) - only the shape rule decides which caches are enlarged",
            str ? "found but no code refers to it" : "not found", ms);
    }
    if (n > 0) {
        DWORD ws = 0;
        int wrefs = 0;
        OriginRange wf[ORIGIN_MAX];
        int nf = origin_find(img, nt->OptionalHeader.SizeOfImage, "video/water_quality", wf, ORIGIN_MAX, &ws, &wrefs);
        g_nwr = 0;
        for (int i = 0; i < nf; i++)   /* вызов отражения — в части-продолжении функции, не в её прологе */
            if (!origin_parts(img, nt->OptionalHeader.SizeOfImage, &wf[i], g_wr, &g_nwr, ORIGIN_MAX * 8) && g_nwr < ORIGIN_MAX * 8)
                g_wr[g_nwr++] = wf[i];
        if (g_nwr > 0)
            LOG("origin: water reflection - \"video/water_quality\" at RVA 0x%lx, %d reference(s), %d function(s) in %d part(s): its "
                "target from \"t:create\" is the world's and keeps the game's size", ws, wrefs, nf, g_nwr);
        else
            LOG("origin: \"video/water_quality\" not found (game update?) - the water reflection target is enlarged like an "
                "interface cache");
    }
}

void res_origin_key(void)
{
    static int was;
    if (!g_nor) return;
    int down = ui4k_hotkey(VK_F7, 0);
    if (down && !was) {
        LONG on = !InterlockedXor(&g_cfg.origin, 1);
        LOG("origin: %s (Ctrl+F7) - applies to window caches created from now on", on ? "ON" : "OFF");
    }
    was = down;
}

/* ---- Перерисовка кэшей окон после смены масштаба на ходу (18.11) ----
 * Реверс (шаг 17 и Ghidra 18.11): окно eDialog рисует свой кэш (+0xF8, буфер «t:create») заново только «грязным» (бит 0
 * флагов +0x48 снят). Перекладка окна (0xac10b0) сбрасывает буфер, лишь если логический размер окна не равен размеру
 * буфера — при смене масштаба он тот же, и кэш прежнего ×k растягивается (×1.5 -> ×2 мягко) до смены надписи. Сброс окна —
 * его слот 10 (0xac15f0; слот 9 — то же плюс отладочная пометка): снимает бит 0, сообщает родителям (слот 4), освобождает буфер; следующая отрисовка создаёт буфер
 * текущего размера (теперь — нового ×k) и рисует окно заново.
 * Окно мод узнаёт в момент создания его кэша: стек разматывается до вызывающего «t:create» — функции отрисовки окна
 * (слот 30 таблицы методов, 0xac0df0); окно — тот из сохраняемых регистров, чья таблица в слоте 30 указывает на эту самую
 * функцию и у кого в +0xF8 уже буфер. Не нашлось (другая версия игры) — окно не запоминается, ничего не трогается.
 * После смены масштаба — в потоке интерфейса, при следующем создании кэша — сброс всех окон с кэшем прежнего масштаба,
 * кроме рисуемого сейчас и его родителей (+0xF0; их — при следующем создании). */
#define UIW_MAX 8192
typedef struct { ID3D11Texture2D *tex; BYTE *win; void *buf; const void *render; Ratio r; } UiWin;
static UiWin g_uiw[UIW_MAX];
static volatile LONG g_nuiw;     /* граница занятых записей */
static SRWLOCK g_uiwlk = SRWLOCK_INIT;
static Ratio g_uiw_scale;        /* масштаб, под который все запомненные кэши свежие; {0,0} — ещё не начинали */
static SIZE_T g_exe_size;
static const GUID GUID_UIW = {0x1d7c44a2, 0x9b3e, 0x4f61, {0xa8, 0x0c, 0x52, 0x6e, 0x31, 0x9d, 0x47, 0xb5}};

static int uiw_readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || ((UINT_PTR)p & 7) || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if ((mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_EXECUTE_WRITECOPY)))
        return 0;
    return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

static int uiw_in_exe(const void *p) { return g_exe && (const BYTE *)p >= g_exe && (const BYTE *)p < g_exe + g_exe_size; }

/* окно с буфером buf (NULL — любым непустым), которое рисует функция render */
static int uiw_is_window(const BYTE *w, const void *render, const void *buf)
{
    if (!uiw_readable(w, 0x100)) return 0;
    void *const *vt = *(void *const *const *)w;
    if (!uiw_in_exe(vt) || !uiw_readable(vt, 31 * sizeof(void *))) return 0;
    void *b = *(void *const *)(w + 0xF8);
    return vt[30] == render && uiw_in_exe(vt[10]) && b && (!buf || b == buf);
}

/* начало функции по её записи раскрутки: части функции (сжатый пролог — у отрисовки окна сохранение RBX посередине,
 * 0xac0e18) имеют свои записи со ссылкой на основную (UNW_FLAG_CHAININFO) — идём по ссылкам до основной */
static const void *uiw_func_start(DWORD64 base, PRUNTIME_FUNCTION f)
{
    for (int i = 0; i < 8 && f; i++) {
        const BYTE *ui = (const BYTE *)base + f->UnwindData;
        if (!uiw_in_exe(ui)) break;
        BYTE flags = ui[0] >> 3, ncodes = ui[2];
        if (!(flags & 0x4)) break;   /* UNW_FLAG_CHAININFO */
        f = (PRUNTIME_FUNCTION)(ui + 4 + ((ncodes + 1) & ~1) * 2);
    }
    return f ? (const BYTE *)base + f->BeginAddress : NULL;
}

/* окно, чей кэш сейчас создаётся (см. выше); NULL — создаёт не окно или не узнали */
static BYTE *uiw_find(const void **render)
{
    CONTEXT c;
    RtlCaptureContext(&c);
    int in_tc = 0;
    for (int depth = 0; depth < 48; depth++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (fe) {
            PVOID hd = NULL;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &est, NULL);
        } else {
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip) return NULL;
        int tc = 0;
        if (uiw_in_exe((const void *)c.Rip)) {
            UINT_PTR rva = (UINT_PTR)c.Rip - (UINT_PTR)g_exe;
            for (int k = 0; k < g_nor; k++) tc |= rva > g_or[k].begin && rva <= g_or[k].end;
        }
        if (tc) { in_tc = 1; continue; }
        if (!in_tc) continue;
        /* c.Rip — возврат в вызывающего «t:create», сохраняемые регистры — как в момент вызова */
        DWORD64 b2 = 0;
        PRUNTIME_FUNCTION f2 = RtlLookupFunctionEntry(c.Rip, &b2, NULL);
        if (!f2 || !uiw_in_exe((const void *)c.Rip)) return NULL;
        const void *fn = uiw_func_start(b2, f2);
        DWORD64 regs[8] = {c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15};
        for (int i = 0; i < 8; i++)
            if (uiw_is_window((const BYTE *)regs[i], fn, NULL)) { *render = fn; return (BYTE *)regs[i]; }
        return NULL;
    }
    return NULL;
}

/* страж в private data кэша: текстура уничтожена — запись свободна */
typedef struct { IUnknownVtbl *vt; volatile LONG ref; LONG idx; ID3D11Texture2D *tex; } UiwGuard;
static HRESULT STDMETHODCALLTYPE uiwg_qi(IUnknown *u, REFIID id, void **o)
{
    if (IsEqualIID(id, &IID_IUnknown)) { *o = u; u->lpVtbl->AddRef(u); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE uiwg_addref(IUnknown *u) { return (ULONG)InterlockedIncrement(&((UiwGuard *)u)->ref); }
static ULONG STDMETHODCALLTYPE uiwg_release(IUnknown *u)
{
    UiwGuard *g = (UiwGuard *)u;
    LONG n = InterlockedDecrement(&g->ref);
    if (n) return (ULONG)n;
    AcquireSRWLockExclusive(&g_uiwlk);
    if (g_uiw[g->idx].tex == g->tex) {   /* запись ещё его (после сброса её мог занять новый кэш) */
        g_uiw[g->idx].tex = NULL;
        g_uiw[g->idx].win = NULL;
    }
    ReleaseSRWLockExclusive(&g_uiwlk);
    HeapFree(GetProcessHeap(), 0, g);
    return 0;
}
static IUnknownVtbl g_uiwg_vt = {uiwg_qi, uiwg_addref, uiwg_release};

typedef void (*PFN_WRESET)(void *);

static void uiw_sweep(BYTE *cur)
{
    BYTE *anc[64];
    int na = 0;
    for (BYTE *p = cur; p && na < 64 && uiw_readable(p + 0xF0, sizeof(void *)); p = *(BYTE **)(p + 0xF0)) anc[na++] = p;
    /* вердикт 19.11, п. 5: список — свой у каждого вызова (общий статический мог перезаписать другой поток, пока этот
     * сбрасывает окна вне блокировки); сбросы идут только после смены масштаба — выделение редкое */
    BYTE **todo = (BYTE **)HeapAlloc(GetProcessHeap(), 0, UIW_MAX * sizeof *todo);
    if (!todo) return;
    int nt = 0, left = 0, gone = 0;
    Ratio k = g_cfg.scale;
    AcquireSRWLockExclusive(&g_uiwlk);
    for (LONG i = 0; i < g_nuiw; i++) {
        UiWin *e = &g_uiw[i];
        if (!e->tex || ratio_eq(e->r, k)) continue;
        int busy = 0;
        for (int a = 0; a < na; a++) busy |= anc[a] == e->win;
        if (busy) { left++; continue; }
        if (uiw_is_window(e->win, e->render, e->buf)) todo[nt++] = e->win;
        else gone++;
        e->tex = NULL;   /* больше не трогаем: окно сброшено или уже не то (свой страж очистит позже — без вреда) */
        e->win = NULL;
    }
    ReleaseSRWLockExclusive(&g_uiwlk);
    /* сброс — вне блокировки: он освобождает буфер, а с ним и текстуру (и её страж) */
    for (int i = 0; i < nt; i++) ((PFN_WRESET)(*(void ***)todo[i])[10])(todo[i]);
    HeapFree(GetProcessHeap(), 0, todo);
    if (!left) g_uiw_scale = k;
    static volatile LONG n_log;
    if (nt + left + gone && InterlockedIncrement(&n_log) <= 40)
        LOG("ui_refresh: interface x%d/%d - %d window cache(s) of the old scale reset to be redrawn, %d left for later "
            "(being drawn now), %d no longer the same window", k.num, k.den, nt, left, gone);
}

/* создан кэш интерфейса t (коэффициент r): если его создаёт окно — запомнить; после смены масштаба — сброс старых */
static void uiw_created(ID3D11Texture2D *t, Ratio r)
{
    if (!g_cfg.ui_refresh || !g_nor || !g_exe) return;
    if (!g_exe_size) {
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(g_exe + ((const IMAGE_DOS_HEADER *)g_exe)->e_lfanew);
        g_exe_size = nt->OptionalHeader.SizeOfImage;
    }
    const void *render = NULL;
    BYTE *w = uiw_find(&render);
    static volatile LONG n_seen, n_miss;
    if (!w) {
        if (InterlockedIncrement(&n_miss) == 1) LOG("ui_refresh: an interface cache made not by a window (or the window was not recognised)");
        return;
    }
    if (InterlockedIncrement(&n_seen) == 1)
        LOG("ui_refresh: window caches recognised (window %p, drawn by exe+0x%llx) - after a scale change they are redrawn",
            (void *)w, (unsigned long long)((const BYTE *)render - g_exe));
    AcquireSRWLockExclusive(&g_uiwlk);
    LONG i;
    for (i = 0; i < g_nuiw && g_uiw[i].tex; i++) {}
    if (i < UIW_MAX) {
        g_uiw[i].tex = t; g_uiw[i].win = w; g_uiw[i].buf = *(void **)(w + 0xF8); g_uiw[i].render = render; g_uiw[i].r = r;
        if (i >= g_nuiw) g_nuiw = i + 1;
    }
    ReleaseSRWLockExclusive(&g_uiwlk);
    if (i < UIW_MAX) {
        UiwGuard *g = (UiwGuard *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *g);
        if (g) {
            g->vt = &g_uiwg_vt; g->ref = 1; g->idx = i; g->tex = t;
            ID3D11Texture2D_SetPrivateDataInterface(t, &GUID_UIW, (IUnknown *)g);
            IUnknown_Release((IUnknown *)g);
        }
    }
    if (!g_uiw_scale.den) g_uiw_scale = g_cfg.scale;
    else if (!ratio_eq(g_uiw_scale, g_cfg.scale)) uiw_sweep(w);
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d,
                                                    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    PFN_CT2D orig = VT_ORIG(dev, &h_ct2d, PFN_CT2D);
    void *ra = __builtin_return_address(0);
    if (d && g_cfg.ndiag_stk && (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {   /* diag_stack: кто создаёт цели таких размеров */
        static volatile LONG n;
        for (int i = 0; i < g_cfg.ndiag_stk; i++)
            if (d->Width == g_cfg.diag_stk[i][0] && d->Height == g_cfg.diag_stk[i][1] && InterlockedIncrement(&n) <= 400) {
                char t[96];
                snprintf(t, sizeof t, "create %ux%u fmt=%d bind=0x%x", d->Width, d->Height, d->Format, d->BindFlags);
                diag_stack(t);
                break;
            }
    }
    if (d && d->Usage == D3D11_USAGE_STAGING) {   /* картинки, которые читает процессор (скриншоты): кто и какого размера */
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 16) {
            char who[64];
            LOG("staging texture %ux%u fmt=%d cpu=0x%x by %s%s", d->Width, d->Height, d->Format, d->CPUAccessFlags,
                caller_name(ra, who, sizeof who), overlay_call(ra) ? " (Steam overlay: real sizes)" : "");
        }
    }
    if (!d || overlay_call(ra)) return orig(dev, d, init, out);
    Screen s = screen_get();
    TexClass c = tex_classify(d, init != NULL, (UINT)s.lw, (UINT)s.lh, g_cfg.scale);
    /* Э2: правило формы не берёт (квадрат, степени двойки) или видит экранную цель — а создаёт «t:create»: кэш окна */
    int by_origin = 0, refl = 0;
    if ((c != TEX_UI_RT || g_nwr) && g_nor && g_cfg.origin && tex_origin_candidate(d, init != NULL, g_cfg.scale)) {
        /* 1 — из «t:create»; 2 — из «t:create», которую позвало отражение в воде (не дальше 3 возвратов выше) */
        int o = origin_stack(g_exe, g_or, g_nor, g_wr, g_nwr, 24, 3);
        if (o == 2) {
            c = TEX_KEEP;
            refl = 1;
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 8)
                LOG("tex2D: water reflection %ux%u fmt=%d bind=0x%x - world target, kept at the game's size", d->Width, d->Height,
                    d->Format, d->BindFlags);
        } else if (o == 1 && c != TEX_UI_RT) {
            c = TEX_UI_RT;
            by_origin = 1;
        }
    }
    if (c == TEX_KEEP) {
        HRESULT hk = orig(dev, d, init, out);
        if (g_cfg.log >= 2 && (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
            static volatile LONG n_keep;
            if (InterlockedIncrement(&n_keep) <= 4000)
                LOGV("tex2D keep: %ux%u fmt=%d ms=%u bind=0x%x mips=%u%s -> %p", d->Width, d->Height, d->Format,
                     d->SampleDesc.Count, d->BindFlags, d->MipLevels,
                     refl ? " (water reflection)" : g_nor && origin_on_stack(g_exe, g_or, g_nor, 24) ? " (t:create)" : "",
                     out ? *out : NULL);
        }
        return hk;
    }
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
        LOG("tex2D %s%s: %ux%u -> %ux%u fmt=%d ms=%u bind=0x%x usage=%d mips=%u -> hr=0x%08lx %p",
            c == TEX_UI_RT ? "ui_rt" : scene ? "scene" : "screen", by_origin ? " (t:create)" : "", d->Width, d->Height, n.Width, n.Height, d->Format,
            d->SampleDesc.Count, d->BindFlags, d->Usage, d->MipLevels, hr, out ? *out : NULL);
    else if (!(k & (k - 1)))
        LOG("tex2D: %ld enlarged %s textures so far (each one is logged at log=2 only)", (long)k,
            c == TEX_UI_RT ? "interface cache" : "screen");
    if (g_cfg.diag_on_w && SUCCEEDED(hr)) diag_on_create(d->Width, d->Height);
    if (c == TEX_SCREEN && d->SampleDesc.Count > 1) msaa_seen(d->SampleDesc.Count);   /* В0: MSAA включили во время игры */
    if (SUCCEEDED(hr) && out && *out) {
        res_mark(*out, d->Width, d->Height, r);
        if (c == TEX_UI_RT) uiw_created(*out, r);   /* кэш окна: после смены масштаба — перерисовать */
    }
    return hr;
}

/* текстура без классификации и увеличения (своя цель мода) */
HRESULT res_create_raw(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, ID3D11Texture2D **out)
{
    return VT_ORIG(dev, &h_ct2d, PFN_CT2D)(dev, d, NULL, out);
}

/* то же с начальными пикселями (Э6: своя страница шрифта, собранная процессором) */
HRESULT res_create_raw_init(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                            ID3D11Texture2D **out)
{
    return VT_ORIG(dev, &h_ct2d, PFN_CT2D)(dev, d, init, out);
}

void res_hook_device(ID3D11Device *dev)
{
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateTexture2D, hk_CreateTexture2D, &h_ct2d);
}
