/* Э6: чёткие буквы шрифтов в игре. Опора, формат образцов и чистая часть — fontpages.h / fontpages_core.c.
 *
 * Путь (как игра рисует буквы; проверено в игре 18–19.11):
 *  1. UpdateSubresource игры в текстуру формы страницы (BGRA8, 1 mip, стороны — степени двойки): отпечаток; страница
 *     из образцов запоминается до копии в рабочую.
 *  2. CopyResource промежуточная -> рабочая (так игра заливает страницу): рабочая становится «живой» страницей. Видов
 *     (SRV) у неё несколько — игра создаёт два при загрузке и ещё позже, — мод помнит все.
 *  3. UpdateSubresource буфера вершин (так игра заливает полосы букв): копия вершин у мода (немного буферов).
 *  4. Draw полосой (TRIANGLESTRIP, шаг 36) при виде живой страницы в слоте 0 PS: вершины переписываются (fp_rewrite) —
 *     каждая своя буква своей ячейкой целиком, из своего атласа, в том же канале (маска вершины та же), тем же TextPS.
 *     Ячейка игры ×1 тесна для буквы ×K (краткая «й», точки) — поэтому не подмена страницы, а свои ячейки.
 *     Атлас — при первом рисовании страницы и при смене масштаба цели (процессором, ~1 мс, сотни КБ).
 *  5. Игра освобождает страницу (страж в её private data) — атлас уходит через 2 с.
 * Чего нет в образцах (другие языки, значки) — рисуется страницей игры как было. Выключено (fonts=0) — ни одного
 * перехвата. Ctrl+F10 — свои буквы / буквы игры (для надписей, которые рисуются после нажатия). */
#include "common.h"
#include "fontpages.h"
#include <stdio.h>
#include <stdlib.h>

static volatile LONG g_on;
static volatile LONG g_show = 1;   /* Ctrl+F10 */

/* ---- образцы: [0] — ×2, [1] — ×4; грузятся в фоне ---- */
static const UINT KM[2] = {2, 4};
static struct {
    volatile LONG state;   /* 0 не начат, 1 грузится, 2 готов, -1 нет файла или ошибка */
    HANDLE done;
    BYTE *file;
    FpData d;
} g_m[2];

static DWORD WINAPI load_thread(LPVOID p)
{
    int i = (int)(INT_PTR)p;
    WCHAR name[32], path[MAX_PATH];
    swprintf(name, 32, L"ui4k_fonts%u.dat", KM[i]);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    int ok = 0;
    DWORD size = 0, got = 0;
    HANDLE h = dir_file(path, name) ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL)
                                    : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        size = GetFileSize(h, NULL);
        g_m[i].file = size != INVALID_FILE_SIZE ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, size ? size : 1) : NULL;
        ok = g_m[i].file && ReadFile(h, g_m[i].file, size, &got, NULL) && got == size && fp_parse(g_m[i].file, size, &g_m[i].d);
        CloseHandle(h);
    }
    QueryPerformanceCounter(&b);
    if (ok) {
        UINT n = 0;
        for (UINT j = 0; j < g_m[i].d.npages; j++) n += g_m[i].d.pages[j].n;
        LOG("fonts: %ls - %u pages, %u letters x%u (%.1f MB, %.0f ms)", name, g_m[i].d.npages, n, KM[i], size / 1e6,
            (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    } else {
        LOG("fonts: %ls %s", name, h == INVALID_HANDLE_VALUE ? "not found next to the DLL" : "is damaged or of an old version - not used");
        if (g_m[i].file) HeapFree(GetProcessHeap(), 0, g_m[i].file);
        g_m[i].file = NULL;
    }
    InterlockedExchange(&g_m[i].state, ok ? 2 : -1);
    SetEvent(g_m[i].done);
    return 0;
}

static void need(int i)
{
    if (InterlockedCompareExchange(&g_m[i].state, 1, 0) != 0) return;
    HANDLE t = CreateThread(NULL, 0, load_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    if (t) CloseHandle(t);
    else { InterlockedExchange(&g_m[i].state, -1); SetEvent(g_m[i].done); }
}

static const FpData *master(int i, DWORD wait_ms)
{
    need(i);
    if (g_m[i].state == 1) WaitForSingleObject(g_m[i].done, wait_ms);
    return g_m[i].state == 2 ? &g_m[i].d : NULL;
}

/* образец для масштаба k: ×2 — свой, остальные — усреднение из ×4 (нет файла — из другого) */
static const FpData *master_for(Ratio k, DWORD wait_ms)
{
    int i = k.num == 2 * k.den ? 0 : 1;
    const FpData *d = master(i, wait_ms);
    return d ? d : master(1 - i, wait_ms);
}

static int page_shape(const D3D11_TEXTURE2D_DESC *d)
{
    return d->Format == DXGI_FORMAT_B8G8R8A8_UNORM && d->MipLevels == 1 && d->ArraySize == 1 && d->SampleDesc.Count == 1 &&
           d->Usage == D3D11_USAGE_DEFAULT && !d->CPUAccessFlags && !d->MiscFlags && d->Width >= 64 && d->Height >= 64 &&
           d->Width <= 4096 && d->Height <= 4096 && !(d->Width & (d->Width - 1)) && !(d->Height & (d->Height - 1));
}

static int tex_desc(ID3D11Resource *r, D3D11_TEXTURE2D_DESC *d)
{
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return 0;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, d);
    return 1;
}

/* ---- виды: SRV -> текстура формы страницы (NULL — вид чего-то другого). Ключ — сам вид: новый вид по адресу
 * освобождённого затирает старую запись, поэтому чужой вид не примется за вид страницы. ---- */
#define NSRV 8192
static struct { ID3D11ShaderResourceView *srv; ID3D11Resource *tex; } g_srv[NSRV];
static SRWLOCK g_srvlk = SRWLOCK_INIT;
static __thread int t_own;   /* свои вызовы мода — не запоминать */

static size_t slot_of(const void *p) { return ((size_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 51; }   /* 13 бит */

static void srv_put(ID3D11ShaderResourceView *v, ID3D11Resource *t)
{
    AcquireSRWLockExclusive(&g_srvlk);
    size_t i = slot_of(v), k;
    for (k = 0; k < NSRV; k++, i = (i + 1) & (NSRV - 1))
        if (!g_srv[i].srv || g_srv[i].srv == v) break;
    if (k == NSRV) i = slot_of(v);   /* полна — вытесняем */
    if (t || g_srv[i].srv == v) {    /* вид не страницы — только затереть старую запись по этому адресу */
        g_srv[i].srv = v;
        g_srv[i].tex = t;
    }
    ReleaseSRWLockExclusive(&g_srvlk);
}

static int srvs_of(ID3D11Resource *t, ID3D11ShaderResourceView **out, int max)
{
    int n = 0;
    AcquireSRWLockShared(&g_srvlk);
    for (size_t i = 0; i < NSRV && n < max; i++)
        if (g_srv[i].srv && g_srv[i].tex == t) out[n++] = g_srv[i].srv;
    ReleaseSRWLockShared(&g_srvlk);
    return n;
}

/* ---- живые страницы ---- */
#define NLIVE 64
#define NGSRV 8
typedef struct {
    ID3D11ShaderResourceView *volatile gsrv[NGSRV];   /* виды страницы игры */
    volatile LONG ngsrv;
    ID3D11Resource *game;                             /* рабочая страница игры (без ссылки: жива, пока жив страж) */
    UINT w, h;
    uint64_t hash;
    FpAtlas at;                                       /* раскладка своих букв (at.pl == NULL — нет) */
    ID3D11Texture2D *tex;                             /* атлас */
    ID3D11ShaderResourceView *srv;
    Ratio built, tried;                               /* для какого масштаба атлас есть / пробовали делать */
    volatile LONG used, said;
} Live;
static Live g_live[NLIVE];
static volatile LONG g_nlive;   /* граница просмотра */
static SRWLOCK g_livelk = SRWLOCK_INIT;

/* новый вид v текстуры t (t == NULL — не страница): убрать адрес v из чужих списков, добавить к своей странице */
static void live_view(ID3D11ShaderResourceView *v, ID3D11Resource *t)
{
    if (!g_nlive) return;
    AcquireSRWLockExclusive(&g_livelk);
    for (LONG j = 0; j < g_nlive; j++) {
        Live *L = &g_live[j];
        LONG n = L->ngsrv;
        for (LONG s = 0; s < n; s++)
            if (L->gsrv[s] == v && L->game != t) {   /* адрес освобождённого вида достался другому */
                L->gsrv[s] = L->gsrv[n - 1];
                InterlockedExchange(&L->ngsrv, --n);
                s--;
            }
        if (t && L->used && L->game == t && n < NGSRV) {
            int have = 0;
            for (LONG s = 0; s < n; s++) have |= L->gsrv[s] == v;
            if (!have) { L->gsrv[n] = v; MemoryBarrier(); InterlockedExchange(&L->ngsrv, n + 1); }
        }
    }
    ReleaseSRWLockExclusive(&g_livelk);
}

static VtHook h_csrv;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSRV)(ID3D11Device *, ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *,
                                             ID3D11ShaderResourceView **);

static HRESULT STDMETHODCALLTYPE hk_CreateShaderResourceView(ID3D11Device *dev, ID3D11Resource *r,
                                                             const D3D11_SHADER_RESOURCE_VIEW_DESC *vd, ID3D11ShaderResourceView **out)
{
    HRESULT hr = VT_ORIG(dev, &h_csrv, PFN_CSRV)(dev, r, vd, out);
    if (SUCCEEDED(hr) && out && *out && !t_own && g_on) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Resource *key = r && tex_desc(r, &d) && page_shape(&d) && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? r : NULL;
        srv_put(*out, key);
        live_view(*out, key);
    }
    return hr;
}

/* ---- залитые страницы до копии в рабочую ---- */
#define NPEND 16
static struct { ID3D11Resource *stg; UINT w, h; uint64_t hash; } g_pend[NPEND];
static volatile LONG g_npend;
static SRWLOCK g_pendlk = SRWLOCK_INIT;

void fontpages_upload(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp)
{
    (void)c;
    D3D11_TEXTURE2D_DESC d;
    if (!g_on || sub || box || !data || !tex_desc(r, &d) || !page_shape(&d)) return;
    uint64_t hash = fp_hash97((const uint8_t *)data, d.Width, d.Height, rp);
    const FpData *m = master_for(g_cfg.scale, 3000);   /* первая страница: образцы могли ещё грузиться */
    const FpPage *pg = m ? fp_find(m, d.Width, d.Height, hash) : NULL;
    if (!pg) {
        static volatile LONG n_log;
        if (g_cfg.log >= 2 && InterlockedIncrement(&n_log) <= 400)
            LOG("fonts: upload %ux%u %016llx bind=0x%x - not a font page of the samples", d.Width, d.Height,
                (unsigned long long)hash, d.BindFlags);
        return;
    }
    /* ревизия v3, п. 5: отпечаток — каждый 97-й байт; страница, изменённая другим модом (одна буква), могла бы его
     * сохранить — тогда поверх неё легли бы буквы оригинала. Совпал — сверить все байты (раз на страницу) */
    if (pg->has_crc && fp_crc32((const uint8_t *)data, d.Width, d.Height, rp) != pg->crc) {
        static volatile LONG n_mod;
        if (InterlockedIncrement(&n_mod) <= 20)
            LOG("fonts: page %ux%u %016llx has the fingerprint of a sample page but other pixels (changed by another mod?) - "
                "the game's letters", d.Width, d.Height, (unsigned long long)hash);
        return;
    }
    ID3D11Resource *drop = NULL;
    AcquireSRWLockExclusive(&g_pendlk);
    int at = -1;
    for (int i = 0; i < NPEND; i++)
        if (g_pend[i].stg == r || (at < 0 && !g_pend[i].stg)) { at = i; if (g_pend[i].stg == r) break; }
    if (at < 0) at = 0;   /* все заняты — старейшую не дождались */
    if (g_pend[at].stg != r) {
        /* ревизия v3, п. 6: держим ссылку до копии — иначе адрес освобождённой промежуточной мог достаться другой
         * текстуре, и приёмник её копии принялся бы за страницу */
        drop = g_pend[at].stg;
        if (!drop) InterlockedIncrement(&g_npend);
        ID3D11Resource_AddRef(r);
        g_pend[at].stg = r;
    }
    g_pend[at].w = d.Width; g_pend[at].h = d.Height; g_pend[at].hash = hash;
    ReleaseSRWLockExclusive(&g_pendlk);
    if (drop) ID3D11Resource_Release(drop);
    LOGV("fonts: upload %ux%u %016llx - font page, waiting for the copy", d.Width, d.Height, (unsigned long long)hash);
}

/* ---- кладбище: свои объекты, которые ещё могли быть привязаны, — освобождаются через 2 с (fontpages_tick) ---- */
static struct { IUnknown *o; DWORD t; } g_grave[256];
static volatile LONG g_ngrave;
static SRWLOCK g_gravelk = SRWLOCK_INIT;

static void bury(IUnknown *o)
{
    if (!o) return;
    AcquireSRWLockExclusive(&g_gravelk);
    int i;
    for (i = 0; i < 256 && g_grave[i].o; i++) {}
    if (i < 256) { g_grave[i].o = o; g_grave[i].t = GetTickCount(); o = NULL; InterlockedIncrement(&g_ngrave); }
    ReleaseSRWLockExclusive(&g_gravelk);
    if (o) IUnknown_Release(o);   /* переполнено — сразу */
}

static void vb_forget_all(void);

void fontpages_tick(void)
{
    static int was;
    if (g_on) {
        int down = ui4k_hotkey(VK_F10, 0);
        if (down && !was) {
            /* вердикт 19.11, п. 7: выключено — и копий вершин нет (fontpages_vb_wanted), путь игры. Прежние копии при
             * переключении забыть: пока копий не было, игра могла залить в те же буферы другое */
            LONG was_on = InterlockedXor(&g_show, 1);
            vb_forget_all();
            LOG("fonts: own letters %s (Ctrl+F10) - for text drawn from now on", was_on ? "OFF, vertex copies stopped" : "ON");
        }
        was = down;
    }
    if (!g_ngrave) return;
    DWORD now = GetTickCount();
    AcquireSRWLockExclusive(&g_gravelk);
    for (int i = 0; i < 256; i++)
        if (g_grave[i].o && now - g_grave[i].t > 2000) {
            IUnknown_Release(g_grave[i].o);
            g_grave[i].o = NULL;
            InterlockedDecrement(&g_ngrave);
        }
    ReleaseSRWLockExclusive(&g_gravelk);
}

/* ---- страж в private data рабочей страницы игры: D3D отпускает его, когда страница уничтожается ---- */
typedef struct { IUnknownVtbl *vt; volatile LONG ref; Live *L; } Guard;
static const GUID GUID_FP_GUARD = {0x3b8f0c11, 0x52d4, 0x4a6e, {0x8e, 0x21, 0x6c, 0x1d, 0x0f, 0x7a, 0x93, 0x45}};

static HRESULT STDMETHODCALLTYPE g_qi(IUnknown *u, REFIID id, void **o)
{
    if (IsEqualIID(id, &IID_IUnknown)) { *o = u; u->lpVtbl->AddRef(u); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE g_addref(IUnknown *u) { return (ULONG)InterlockedIncrement(&((Guard *)u)->ref); }
static ULONG STDMETHODCALLTYPE g_release(IUnknown *u)
{
    Guard *g = (Guard *)u;
    LONG n = InterlockedDecrement(&g->ref);
    if (n) return (ULONG)n;
    Live *L = g->L;
    AcquireSRWLockExclusive(&g_livelk);
    InterlockedExchange(&L->ngsrv, 0);
    bury((IUnknown *)L->srv);
    bury((IUnknown *)L->tex);
    L->srv = NULL;
    L->tex = NULL;
    fp_atlas_free(&L->at);
    L->game = NULL;
    InterlockedExchange(&L->used, 0);
    ReleaseSRWLockExclusive(&g_livelk);
    LOGV("fonts: game page %016llx released - its own letters go too", (unsigned long long)L->hash);
    HeapFree(GetProcessHeap(), 0, g);
    return 0;
}
static IUnknownVtbl g_guard_vt = {g_qi, g_addref, g_release};

void fontpages_copied(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    (void)c;
    if (!g_on || !g_npend || !src || !dst) return;
    AcquireSRWLockExclusive(&g_pendlk);
    int at = -1;
    for (int i = 0; i < NPEND; i++)
        if (g_pend[i].stg == src) { at = i; break; }
    UINT w = 0, h = 0;
    uint64_t hash = 0;
    ID3D11Resource *held = NULL;
    if (at >= 0) {
        w = g_pend[at].w; h = g_pend[at].h; hash = g_pend[at].hash;
        held = g_pend[at].stg;
        g_pend[at].stg = NULL;
        InterlockedDecrement(&g_npend);
    }
    ReleaseSRWLockExclusive(&g_pendlk);
    if (held) ID3D11Resource_Release(held);
    if (at < 0) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11ShaderResourceView *gs[NGSRV];
    int ngs = srvs_of(dst, gs, NGSRV);
    if (!tex_desc(dst, &d) || d.Width != w || d.Height != h || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        LOG("fonts: page %016llx copied into a texture of another kind - left as it is", (unsigned long long)hash);
        return;
    }
    AcquireSRWLockExclusive(&g_livelk);
    Live *L = NULL;
    for (LONG i = 0; i < g_nlive; i++)   /* та же рабочая страница (игра залила её заново) */
        if (g_live[i].used && g_live[i].game == dst) { L = &g_live[i]; break; }
    int fresh = !L;
    for (LONG i = 0; !L && i < NLIVE; i++)
        if (!InterlockedCompareExchange(&g_live[i].used, 1, 0)) L = &g_live[i];
    if (L) {
        if (!fresh && L->hash != hash) {   /* залита другая страница — атлас старой не годится */
            bury((IUnknown *)L->srv);
            bury((IUnknown *)L->tex);
            L->srv = NULL;
            L->tex = NULL;
            fp_atlas_free(&L->at);
        }
        L->w = w; L->h = h; L->hash = hash; L->game = dst;
        if (fresh || !L->at.pl) { L->built.num = 0; L->built.den = 1; }
        L->tried.num = 0; L->tried.den = 1;
        L->said = 0;
        if (fresh) {
            for (int s = 0; s < ngs; s++) L->gsrv[s] = gs[s];
            MemoryBarrier();
            InterlockedExchange(&L->ngsrv, ngs);
        }
        LONG idx = (LONG)(L - g_live);
        if (idx >= g_nlive) InterlockedExchange(&g_nlive, idx + 1);
    }
    ReleaseSRWLockExclusive(&g_livelk);
    if (!L) {
        LOG("fonts: more than %d live pages - page %016llx left as it is", NLIVE, (unsigned long long)hash);
        return;
    }
    if (fresh) {
        Guard *g = (Guard *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *g);
        if (g) {
            g->vt = &g_guard_vt; g->ref = 1; g->L = L;
            ID3D11Resource_SetPrivateDataInterface(dst, &GUID_FP_GUARD, (IUnknown *)g);   /* D3D держит свою ссылку */
            IUnknown_Release((IUnknown *)g);
        }
    }
    LOGV("fonts: page %ux%u %016llx is live (%d views)", w, h, (unsigned long long)hash, ngs);
}

/* атлас страницы (w x h, отпечаток hash) для масштаба k — без блокировок (ревизия v3, п. 3: создание текстур D3D под
 * g_livelk могло встретиться со стражем страницы, которого D3D отпускает изнутри уничтожения страницы, в любом потоке).
 * 1 — готов: *out, *tex, *srv новые, их место отдаёт вызывающий */
static int atlas_make(ID3D11DeviceContext *c, UINT pw, UINT ph, uint64_t hash, Ratio k, FpAtlas *out, ID3D11Texture2D **tex,
                      ID3D11ShaderResourceView **srv)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    ZeroMemory(out, sizeof *out);
    *tex = NULL;
    *srv = NULL;
    const FpData *m = master_for(k, 3000);
    const FpPage *pg = m ? fp_find(m, pw, ph, hash) : NULL;
    if (!pg || k.num <= 0 || k.den <= 0) return 0;
    FpAtlas at;
    if (!fp_atlas_layout(pg, m->K, (uint32_t)k.num, (uint32_t)k.den, &at)) {
        LOG("fonts: page %016llx - own letters for x%d/%d do not fit an atlas - the game's letters", (unsigned long long)hash,
            k.num, k.den);
        return 0;
    }
    BYTE *px = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)at.AW * at.AH * 4);
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext_GetDevice(RC(c), &dev);
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    int ok = px && dev && fp_atlas_pixels(&at, px);
    if (ok) {
        D3D11_TEXTURE2D_DESC td = {at.AW, at.AH, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {px, at.AW * 4, 0};
        t_own = 1;
        ok = SUCCEEDED(res_create_raw_init(dev, &td, &init, &t)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &v));
        t_own = 0;
    }
    if (dev) ID3D11Device_Release(dev);
    if (px) HeapFree(GetProcessHeap(), 0, px);
    if (!ok) {
        if (v) ID3D11ShaderResourceView_Release(v);
        if (t) ID3D11Texture2D_Release(t);
        fp_atlas_free(&at);
        LOG("fonts: page %016llx - atlas x%d/%d could not be made", (unsigned long long)hash, k.num, k.den);
        return 0;
    }
    *out = at;
    *tex = t;
    *srv = v;
    UINT blank = 0;
    for (UINT i = 0; i < at.n; i++) blank += at.pl[i].kind == FP_BLANK;
    QueryPerformanceCounter(&b);
    LOG("fonts: page %ux%u %016llx: own letters for interface x%d/%d - %u (+%u blank), atlas %ux%u from samples x%u: %.2f ms",
        pw, ph, (unsigned long long)hash, k.num, k.den, at.own, blank, at.AW, at.AH, m->K,
        (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    return 1;
}

/* ---- копии буферов вершин (игра заливает полосы букв UpdateSubresource в DEFAULT-буфер): мод держит ссылку, чтобы
 * адрес не достался другому буферу; копия сбрасывается, если буфер пишут иначе (Copy*). ---- */
#define NVB 8
#define VB_MAX (1u << 20)
typedef struct { ID3D11Buffer *b; BYTE *p; UINT size, lo, hi; DWORD used; LONG text; } Shadow;
static Shadow g_vb[NVB];
static volatile LONG g_nvb;
static SRWLOCK g_vblk = SRWLOCK_INIT;

/* ревизия v3, п. 4: буквы рисуются только на непосредственном контексте — копии только его заливок (отложенный контекст
 * выполнит свою позже, чем мод снял бы копию; 3D-буферы не копировать); описание буфера — у вызывающего (одно на заливку) */
int fontpages_vb_wanted(ID3D11DeviceContext *c)
{
    return g_on && g_show && g_nlive && CTXW(c)->type == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

/* все копии вершин недействительны (Ctrl+F10): буфер и место остаются, известной области нет */
static void vb_forget_all(void)
{
    AcquireSRWLockExclusive(&g_vblk);
    for (int i = 0; i < NVB; i++) g_vb[i].lo = g_vb[i].hi = 0;
    ReleaseSRWLockExclusive(&g_vblk);
}

void fontpages_vb_update(ID3D11Resource *r, const D3D11_BUFFER_DESC *dd, const D3D11_BOX *box, const void *data)
{
    if (!r || !data || !dd) return;
    const D3D11_BUFFER_DESC d = *dd;
    if (!(d.BindFlags & D3D11_BIND_VERTEX_BUFFER) || d.Usage != D3D11_USAGE_DEFAULT || d.ByteWidth > VB_MAX || !d.ByteWidth) return;
    UINT a = box ? box->left : 0, b = box ? box->right : d.ByteWidth;
    if (b > d.ByteWidth || a >= b) return;
    ID3D11Buffer *drop = NULL;
    AcquireSRWLockExclusive(&g_vblk);
    Shadow *e = NULL;
    for (int i = 0; i < NVB && !e; i++)
        if (g_vb[i].b == (ID3D11Buffer *)r) e = &g_vb[i];
    if (!e) {   /* свободное место, иначе давнее не текстовое, иначе давнее */
        for (int pass = 0; pass < 3 && !e; pass++)
            for (int i = 0; i < NVB; i++) {
                Shadow *s = &g_vb[i];
                if (pass == 0 ? !s->b : pass == 1 ? !s->text : 1)
                    if (!e || (s->b && (LONG)(s->used - e->used) < 0)) e = s;
            }
        if (e->b) drop = e->b;
        else InterlockedIncrement(&g_nvb);
        if (e->p && e->size < d.ByteWidth) { HeapFree(GetProcessHeap(), 0, e->p); e->p = NULL; }
        if (!e->p) e->p = (BYTE *)HeapAlloc(GetProcessHeap(), 0, d.ByteWidth);
        e->b = (ID3D11Buffer *)r;
        ID3D11Buffer_AddRef(e->b);
        e->size = d.ByteWidth;
        e->lo = e->hi = 0;
        e->text = 0;
    }
    if (e->p) {
        memcpy(e->p + a, data, b - a);
        if (e->hi > e->lo && a <= e->hi && b >= e->lo) {   /* смежно с известным — расширить */
            if (a < e->lo) e->lo = a;
            if (b > e->hi) e->hi = b;
        } else { e->lo = a; e->hi = b; }
    } else e->lo = e->hi = 0;
    e->used = GetTickCount();
    ReleaseSRWLockExclusive(&g_vblk);
    if (drop) ID3D11Buffer_Release(drop);
}

void fontpages_vb_touch(ID3D11Resource *r)
{
    if (!g_nvb || !r) return;
    int hit = 0;
    for (int i = 0; i < NVB; i++) hit |= g_vb[i].b == (ID3D11Buffer *)r;
    if (!hit) return;
    AcquireSRWLockExclusive(&g_vblk);
    for (int i = 0; i < NVB; i++)
        if (g_vb[i].b == (ID3D11Buffer *)r) g_vb[i].lo = g_vb[i].hi = 0;
    ReleaseSRWLockExclusive(&g_vblk);
}

/* ---- рисование букв: свои ячейки ---- */
typedef void(STDMETHODCALLTYPE *PFN_PSSR)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
typedef void(STDMETHODCALLTYPE *PFN_DRAW)(ID3D11DeviceContext *, UINT, UINT);

/* всё ниже — только непосредственный контекст (один поток по правилам D3D11) */
static BYTE *g_in, *g_own, *g_oth;
static UINT g_scap;              /* вершин в g_in/g_own/g_oth */
static ID3D11Buffer *g_dvb;      /* свой буфер вершин */
static UINT g_dvbcap;            /* байт */
static int g_mode = -1;          /* маска вершины -> канал страницы (fp_rewrite) */

static int scratch(UINT n)
{
    if (n <= g_scap) return 1;
    UINT cap = n < 4096 ? 4096 : n;
    BYTE *a = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)cap * FP_VSTRIDE);
    BYTE *b = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)(cap + 1) * FP_VSTRIDE);
    BYTE *c = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)(cap + 1) * FP_VSTRIDE);
    if (!a || !b || !c) {
        if (a) HeapFree(GetProcessHeap(), 0, a);
        if (b) HeapFree(GetProcessHeap(), 0, b);
        if (c) HeapFree(GetProcessHeap(), 0, c);
        return 0;
    }
    if (g_in) { HeapFree(GetProcessHeap(), 0, g_in); HeapFree(GetProcessHeap(), 0, g_own); HeapFree(GetProcessHeap(), 0, g_oth); }
    g_in = a; g_own = b; g_oth = c; g_scap = cap;
    return 1;
}

static int dyn_vb(ID3D11DeviceContext *c, UINT bytes)
{
    if (g_dvb && bytes <= g_dvbcap) return 1;
    UINT cap = bytes < (256u << 10) ? (256u << 10) : bytes;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext_GetDevice(RC(c), &dev);
    if (!dev) return 0;
    D3D11_BUFFER_DESC d = {cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *b = NULL;
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &d, NULL, &b);
    ID3D11Device_Release(dev);
    if (FAILED(hr) || !b) return 0;
    if (g_dvb) bury((IUnknown *)g_dvb);
    g_dvb = b;
    g_dvbcap = cap;
    return 1;
}

static int draw_own(ID3D11DeviceContext *c, Live *L, ID3D11Buffer *vb, UINT stride, UINT off, ID3D11ShaderResourceView *gs, UINT n, UINT s)
{
    CtxState *st = &CTXW(c)->st;
    Ratio k = st->rk;
    LONG ns = L->ngsrv, i;
    for (i = 0; i < ns && L->gsrv[i] != gs; i++) {}
    if (i == ns) return 0;   /* в слоте 0 уже не эта страница */
    /* вершина -> физический px цели: viewport игры (логический) и его перевод моделью (F07) */
    D3D11_VIEWPORT lv = st->vp[0], pv = lv;
    vp_to_phys(k, &pv);
    if (!(lv.Width > 0) || !(lv.Height > 0)) return 0;
    FpMap m;
    m.rx = (double)pv.Width / lv.Width;
    m.ry = (double)pv.Height / lv.Height;
    m.bx = pv.TopLeftX - lv.TopLeftX * m.rx;
    m.by = pv.TopLeftY - lv.TopLeftY * m.ry;
    if (!scratch(n)) return 0;
    /* вершины из копии буфера */
    size_t from = (size_t)off + (size_t)s * FP_VSTRIDE, len = (size_t)n * FP_VSTRIDE;
    Shadow *e = NULL;
    AcquireSRWLockShared(&g_vblk);
    for (int j = 0; j < NVB && !e; j++)
        if (g_vb[j].b == vb) e = &g_vb[j];
    int have = e && e->p && from >= e->lo && from + len <= e->hi;
    if (have) {
        memcpy(g_in, e->p + from, len);
        if (!e->text) InterlockedExchange(&e->text, 1);
    }
    ReleaseSRWLockShared(&g_vblk);
    if (!have) {
        static volatile LONG said;
        if (!InterlockedExchange(&said, 1))
            LOG("fonts: a text draw from a vertex buffer the mod has no copy of - drawn with the game's letters");
        return 0;
    }
    /* атлас для масштаба цели */
    AcquireSRWLockShared(&g_livelk);
    if (!L->used || L->ngsrv != ns) { ReleaseSRWLockShared(&g_livelk); return 0; }
    if (!ratio_eq(L->built, k)) {
        int again = !ratio_eq(L->tried, k);
        UINT pw = L->w, ph = L->h;
        uint64_t phash = L->hash;
        ReleaseSRWLockShared(&g_livelk);
        if (!again) return 0;
        FpAtlas nat;
        ID3D11Texture2D *nt;
        ID3D11ShaderResourceView *nv;
        int made = atlas_make(c, pw, ph, phash, k, &nat, &nt, &nv);   /* вне блокировки */
        AcquireSRWLockExclusive(&g_livelk);
        int same = L->used && L->hash == phash && L->w == pw && L->h == ph;   /* страницу не освободили, не залили другую */
        if (same) L->tried = k;
        if (same && made && !ratio_eq(L->built, k)) {
            bury((IUnknown *)L->srv);
            bury((IUnknown *)L->tex);
            fp_atlas_free(&L->at);
            L->at = nat; L->tex = nt; L->srv = nv; L->built = k;
            made = 0;   /* отдан странице */
        }
        ReleaseSRWLockExclusive(&g_livelk);
        if (made) { bury((IUnknown *)nv); bury((IUnknown *)nt); fp_atlas_free(&nat); }   /* не пригодился */
        AcquireSRWLockShared(&g_livelk);
        if (!L->used || !ratio_eq(L->built, k)) { ReleaseSRWLockShared(&g_livelk); return 0; }
    }
    uint32_t no = 0, nt = 0;
    FpCount cnt;
    int ok = fp_rewrite(&L->at, &m, &g_mode, g_in, n, g_own, &no, g_oth, &nt, &cnt) && dyn_vb(c, (no + nt) * FP_VSTRIDE);
    if (ok) {
        D3D11_MAPPED_SUBRESOURCE ms;
        ok = SUCCEEDED(ID3D11DeviceContext_Map(RC(c), (ID3D11Resource *)g_dvb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
        if (ok) {
            memcpy(ms.pData, g_own, (size_t)no * FP_VSTRIDE);
            memcpy((BYTE *)ms.pData + (size_t)no * FP_VSTRIDE, g_oth, (size_t)nt * FP_VSTRIDE);
            ID3D11DeviceContext_Unmap(RC(c), (ID3D11Resource *)g_dvb, 0);
        }
    }
    if (ok) {
        UINT st36 = FP_VSTRIDE, zero = 0;
        ID3D11DeviceContext_IASetVertexBuffers(RC(c), 0, 1, &g_dvb, &st36, &zero);
        if (no) {
            ORIGC(c, PSSetShaderResources, PFN_PSSR)(RC(c), 0, 1, &L->srv);
            ORIGC(c, Draw, PFN_DRAW)(RC(c), no, 0);
        }
        ORIGC(c, PSSetShaderResources, PFN_PSSR)(RC(c), 0, 1, &gs);
        if (nt) ORIGC(c, Draw, PFN_DRAW)(RC(c), nt, no);
        ID3D11DeviceContext_IASetVertexBuffers(RC(c), 0, 1, &vb, &stride, &off);
        if (!L->said && !InterlockedExchange(&L->said, 1))
            LOG("fonts: page %016llx drawn with own letter cells (x%d/%d; first draw: %u own, %u from the game page, %u blank)",
                (unsigned long long)L->hash, k.num, k.den, cnt.own, cnt.other, cnt.dropped);
    }
    ReleaseSRWLockShared(&g_livelk);
    return ok;
}

int fontpages_draw(ID3D11DeviceContext *c, UINT n, UINT s)
{
    CtxState *st = &CTXW(c)->st;
    Live *L = (Live *)st->fp_live;
    if (!g_show || !L || CTXW(c)->type != D3D11_DEVICE_CONTEXT_IMMEDIATE || st->topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
        !st->rk.num || ratio_is_one(st->rk) || !st->nvp || n < 4 || (n + 2) % 6)
        return 0;
    ID3D11Buffer *vb = NULL;
    UINT stride = 0, off = 0;
    ID3D11DeviceContext_IAGetVertexBuffers(RC(c), 0, 1, &vb, &stride, &off);
    ID3D11ShaderResourceView *gs = NULL;
    ID3D11DeviceContext_PSGetShaderResources(RC(c), 0, 1, &gs);
    int done = vb && gs && stride == FP_VSTRIDE && draw_own(c, L, vb, stride, off, gs, n, s);
    if (vb) ID3D11Buffer_Release(vb);
    if (gs) ID3D11ShaderResourceView_Release(gs);
    return done;
}

/* ---- привязка: что в слоте 0 — для fontpages_draw ---- */
static void STDMETHODCALLTYPE hk_PSSetShaderResources(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v)
{
    if (start == 0 && n) {
        void *lv = NULL;
        LONG nl = g_nlive;
        for (LONG j = 0; v && v[0] && j < nl && !lv; j++) {
            LONG ns = g_live[j].ngsrv;
            for (LONG s = 0; s < ns; s++)
                if (g_live[j].gsrv[s] == v[0]) { lv = &g_live[j]; break; }
        }
        CTXW(c)->st.fp_live = lv;
    }
    ORIGC(c, PSSetShaderResources, PFN_PSSR)(RC(c), start, n, v);
}

void fontpages_hook_device(ID3D11Device *dev)
{
    if (g_on) VT_PATCH(dev, ID3D11DeviceVtbl, CreateShaderResourceView, hk_CreateShaderResourceView, &h_csrv);
}

void fontpages_install(void)
{
    if (!g_cfg.fonts) { LOG("fonts: off (fonts=0) - the game's own letters"); return; }
    WCHAR p2[MAX_PATH], p4[MAX_PATH];
    int h2 = dir_file(p2, L"ui4k_fonts2.dat") && GetFileAttributesW(p2) != INVALID_FILE_ATTRIBUTES;
    int h4 = dir_file(p4, L"ui4k_fonts4.dat") && GetFileAttributesW(p4) != INVALID_FILE_ATTRIBUTES;
    if (!h2 && !h4) { LOG("fonts: no ui4k_fonts2.dat / ui4k_fonts4.dat next to the DLL - the game's own letters"); return; }
    for (int i = 0; i < 2; i++) g_m[i].done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!h2) InterlockedExchange(&g_m[0].state, -1);
    if (!h4) InterlockedExchange(&g_m[1].state, -1);
    CTX_HOOK(PSSetShaderResources, hk_PSSetShaderResources);
    InterlockedExchange(&g_on, 1);
    master_for(g_cfg.scale, 0);   /* начать загрузку нужных образцов сразу */
    LOG("fonts: sharp letters on - own letter cells (samples x2: %s, x4: %s; keys=1: Ctrl+F10 in game - own / game letters)",
        h2 ? "yes" : "no", h4 ? "yes" : "no");
}
