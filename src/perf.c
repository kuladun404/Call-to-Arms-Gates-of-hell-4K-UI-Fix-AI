/* perf=1 — режим замеров (план v3, этап Э1). Каждые 5 с строка «perf» в лог (perf_every=N — каждые N кадров, тесты):
 *  - время кадра между Present: среднее, медиана, 99 %, худший; рывки — кадры длиннее 2x медианы;
 *  - время внутри Present;
 *  - видеокарта, метки времени на непосредственном контексте: кадр (от Present до Present), из него интерфейс
 *    (от первого рисования интерфейса в экран до Present) и перенос мира своей целью (работа мода, только когда
 *    мир и интерфейс разного размера);
 *  - перепись раз в 64 кадра: рисований за кадр, из них интерфейса, и сколько рисований интерфейса в цель xk берут
 *    точечной выборкой цель x1 — кэш окна, который мод не увеличил (ошибка классификации; Э2).
 * Время своих перехватов на процессоре меряет внешний профилировщик (perf\goh_prof.exe): часы в каждом перехвате
 * стоили бы столько же, сколько меряют. Без perf=1 ничего из этого не работает. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>

#define NQ 8         /* кадров в пути у видеокарты */
#define NT 5         /* 0 начало кадра, 1 начало интерфейса, 2 и 3 перенос мира, 4 перед Present */
#define MAXF 8192

static ID3D11DeviceContext *g_ic;   /* настоящий непосредственный контекст устройства swap chain */
static ID3D11Query *g_qd[NQ], *g_qt[NQ][NT];
static int g_st[NQ];                /* 0 свободен, 1 кадр идёт, 2 ждёт данных */
static volatile LONG g_has[NQ];     /* поставлены: 1 начало интерфейса, 2 начало переноса, 4 конец переноса */
static volatile LONG g_cur = -1;
static int g_tried;

static LARGE_INTEGER g_qf, g_last, g_pre, g_t0;
static float g_ft[MAXF], g_pt[MAXF], g_gt[MAXF], g_ui[MAXF], g_cp[MAXF];
static int g_nft, g_npt, g_ngt, g_nui, g_ncp;
static LONG g_frames, g_frames0;

volatile LONG g_perf_cen;           /* перепись идёт в этом кадре (читают хуки рисования) */
static volatile LONG g_cd, g_cu, g_cx;
static LONG g_cframes;
static SRWLOCK g_szlk = SRWLOCK_INIT;
static char g_sizes[200];           /* размеры найденных кэшей x1, без повторов */

static double qms(LONGLONG d) { return (double)d * 1000.0 / (double)g_qf.QuadPart; }
static void ts(int k, int i) { g_ic->lpVtbl->End(g_ic, (ID3D11Asynchronous *)g_qt[k][i]); }

static void q_init(IDXGISwapChain *sc)
{
    ID3D11Device *d = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&d)) || !d) return;
    ID3D11DeviceContext *c = NULL;
    ID3D11Device_GetImmediateContext(d, &c);   /* обёртка мода; ссылку держим до конца — режим замеров */
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt = {D3D11_QUERY_TIMESTAMP, 0};
    int ok = c != NULL;
    for (int k = 0; k < NQ && ok; k++) {
        ok = SUCCEEDED(ID3D11Device_CreateQuery(d, &qd, &g_qd[k]));
        for (int i = 0; i < NT && ok; i++) ok = SUCCEEDED(ID3D11Device_CreateQuery(d, &qt, &g_qt[k][i]));
    }
    ID3D11Device_Release(d);
    if (!ok) { LOG("perf: GPU timestamps are not available - frame times only"); return; }
    g_ic = ctx_real(c);
    LOG("perf: GPU timestamps on");
}

/* перед Present: закрыть кадр, собрать готовые, открыть следующий */
static void gpu_end_begin(void)
{
    ID3D11DeviceContext *c = g_ic;
    LONG k = g_cur;
    if (k >= 0 && g_st[k] == 1) {
        ts(k, 4);
        c->lpVtbl->End(c, (ID3D11Asynchronous *)g_qd[k]);
        g_st[k] = 2;
    }
    for (int q = 0; q < NQ; q++) {
        if (g_st[q] != 2) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t[NT] = {0};
        LONG h = g_has[q];
        if (c->lpVtbl->GetData(c, (ID3D11Asynchronous *)g_qd[q], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        int ok = 1;
        for (int i = 0; i < NT && ok; i++) {
            if ((i == 1 && !(h & 1)) || (i == 2 && !(h & 2)) || (i == 3 && !(h & 4))) continue;
            ok = c->lpVtbl->GetData(c, (ID3D11Asynchronous *)g_qt[q][i], &t[i], sizeof t[i], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        }
        if (!ok) continue;
        if (!dj.Disjoint && dj.Frequency) {
            double f = 1000.0 / (double)dj.Frequency;
            if (g_ngt < MAXF) g_gt[g_ngt++] = (float)((double)(t[4] - t[0]) * f);
            if ((h & 1) && g_nui < MAXF) g_ui[g_nui++] = (float)((double)(t[4] - t[1]) * f);
            if ((h & 6) == 6 && g_ncp < MAXF) g_cp[g_ncp++] = (float)((double)(t[3] - t[2]) * f);
        }
        g_st[q] = 0;
    }
    LONG j = (k + 1 + NQ) % NQ;
    if (g_st[j] == 0) {
        g_has[j] = 0;
        c->lpVtbl->Begin(c, (ID3D11Asynchronous *)g_qd[j]);
        ts(j, 0);
        g_st[j] = 1;
        InterlockedExchange(&g_cur, j);
    } else {
        InterlockedExchange(&g_cur, -1);   /* видеокарта отстала на NQ кадров: этот кадр без меток */
    }
}

/* метка в кадре: what 1 — начало интерфейса (один раз), 2/4 — начало/конец переноса мира. Только на контексте g_ic */
static void mark(ID3D11DeviceContext *real, LONG what, int slot)
{
    LONG k = g_cur;
    if (!g_ic || real != g_ic || k < 0 || g_st[k] != 1 || (g_has[k] & what)) return;
    if (what == 4 && !(g_has[k] & 2)) return;
    ts(k, slot);
    g_has[k] |= what;
}
void perf_world_copy(ID3D11DeviceContext *real, int after) { mark(real, after ? 4 : 2, after ? 3 : 2); }

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static float pct(const float *s, int n, int p) { return n ? s[(long long)n * p / 100 < n ? (long long)n * p / 100 : n - 1] : 0; }
static double avg(const float *a, int n) { double s = 0; for (int i = 0; i < n; i++) s += a[i]; return n ? s / n : 0; }

static void report(LARGE_INTEGER q)
{
    static float s[MAXF], g[MAXF];
    double dt = qms(q.QuadPart - g_t0.QuadPart) / 1000.0;
    LONG frames = g_frames - g_frames0;
    int n = g_nft, gn = g_ngt;
    memcpy(s, g_ft, n * sizeof(float));
    qsort(s, n, sizeof(float), cmpf);
    memcpy(g, g_gt, gn * sizeof(float));
    qsort(g, gn, sizeof(float), cmpf);
    float med = pct(s, n, 50);
    int hitch = 0;
    for (int i = 0; i < n; i++) hitch += g_ft[i] > 2 * med;
    float pmax = 0;
    for (int i = 0; i < g_npt; i++) if (g_pt[i] > pmax) pmax = g_pt[i];
    LONG cf = g_cframes ? g_cframes : 1;
    char sz[220] = "";
    AcquireSRWLockExclusive(&g_szlk);
    if (g_sizes[0]) snprintf(sz, sizeof sz, " (%s)", g_sizes);
    g_sizes[0] = 0;
    ReleaseSRWLockExclusive(&g_szlk);
    SYSTEMTIME st;
    GetLocalTime(&st);
    ui4k_log("perf %02u:%02u:%02u %.1f s: %ld frames (%.1f fps); frame avg %.2f med %.2f p99 %.2f max %.2f ms, "
             "hitches (>2x median) %d; in Present avg %.3f max %.2f ms; gpu frame (n %d) avg %.2f med %.2f p99 %.2f ms, "
             "interface part (n %d) avg %.3f ms, world copy (n %d) avg %.3f ms; census %ld frames: draws %.0f, "
             "interface draws %.0f, x1 caches into xk targets %.1f per frame%s",
             st.wHour, st.wMinute, st.wSecond, dt, frames, dt > 0 ? frames / dt : 0, avg(g_ft, n), med, pct(s, n, 99),
             n ? s[n - 1] : 0, hitch, avg(g_pt, g_npt), pmax, gn, avg(g_gt, gn), pct(g, gn, 50), pct(g, gn, 99), g_nui,
             avg(g_ui, g_nui), g_ncp, avg(g_cp, g_ncp), g_cframes, (double)g_cd / cf, (double)g_cu / cf, (double)g_cx / cf, sz);
    g_nft = g_npt = g_ngt = g_nui = g_ncp = 0;
    g_cframes = 0;
    InterlockedExchange(&g_cd, 0);
    InterlockedExchange(&g_cu, 0);
    InterlockedExchange(&g_cx, 0);
    g_t0 = q;
    g_frames0 = g_frames;
}

void perf_present_pre(IDXGISwapChain *sc)
{
    LARGE_INTEGER q;
    if (!g_qf.QuadPart) QueryPerformanceFrequency(&g_qf);
    if (!g_tried) { g_tried = 1; q_init(sc); }
    if (g_ic) gpu_end_begin();
    QueryPerformanceCounter(&q);
    if (g_last.QuadPart && g_nft < MAXF) g_ft[g_nft++] = (float)qms(q.QuadPart - g_last.QuadPart);
    g_last = g_pre = q;
}

void perf_present_post(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (g_npt < MAXF) g_pt[g_npt++] = (float)qms(q.QuadPart - g_pre.QuadPart);
    if (g_perf_cen) g_cframes++;
    LONG f = ++g_frames;
    InterlockedExchange(&g_perf_cen, (f & 63) == 1);   /* кадр после 1-го, 65-го, ... Present */
    if (!g_t0.QuadPart) { g_t0 = q; g_frames0 = f; return; }
    if (g_cfg.perf_every > 0 ? f - g_frames0 >= g_cfg.perf_every : q.QuadPart - g_t0.QuadPart >= 5 * g_qf.QuadPart) report(q);
}

/* из хуков рисования (draw.c), после world_pre_draw */
void perf_draw(ID3D11DeviceContext *c)
{
    /* 19.11: рисования внутри Present — оверлеи (счётчик FPS RivaTuner): их состояние обёртка не видит, и рисование
     * выглядело как интерфейс игры. Кадр замера к этому моменту уже открыт (perf_present_pre) — «начало интерфейса»
     * попадало в самое начало кадра, интерфейс = весь кадр. Ни метки, ни переписи */
    if (in_present()) return;
    LONG k = g_cur;
    if (k >= 0 && !(g_has[k] & 1) && world_ui_draw(c)) mark(RC(c), 1, 1);
    if (!g_perf_cen) return;
    InterlockedIncrement(&g_cd);
    if (!world_ui_vs(c)) return;
    InterlockedIncrement(&g_cu);
    if (!CTXW(c)->st.rk.num) return;   /* цель не увеличена */
    /* кэш окна выводится 1:1 точечной выборкой; цели, которые читают сглаживающим фильтром (туман миникарты
     * 2048x2048, кэши со сглаживанием), — не ошибка классификации */
    ID3D11SamplerState *sm = NULL;
    RC(c)->lpVtbl->PSGetSamplers(RC(c), 0, 1, &sm);
    if (!sm) return;   /* нет сэмплера — по умолчанию линейный */
    D3D11_SAMPLER_DESC sd;
    ID3D11SamplerState_GetDesc(sm, &sd);
    ID3D11SamplerState_Release(sm);
    if (sd.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT) return;
    ID3D11ShaderResourceView *v[2] = {NULL, NULL};
    RC(c)->lpVtbl->PSGetShaderResources(RC(c), 0, 2, v);
    int hit = 0;
    for (int i = 0; i < 2; i++) {
        if (!v[i]) continue;
        ID3D11Resource *r = NULL;
        ID3D11ShaderResourceView_GetResource(v[i], &r);
        ID3D11Texture2D *t = NULL;
        if (r && SUCCEEDED(ID3D11Resource_QueryInterface(r, &IID_ID3D11Texture2D, (void **)&t)) && t) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(t, &d);
            if ((d.BindFlags & D3D11_BIND_RENDER_TARGET) && !res_ratio(r).num && !hit) {
                hit = 1;
                char one[24];
                snprintf(one, sizeof one, "%ux%u", d.Width, d.Height);
                AcquireSRWLockExclusive(&g_szlk);
                if (!strstr(g_sizes, one) && strlen(g_sizes) + strlen(one) + 2 < sizeof g_sizes)
                    snprintf(g_sizes + strlen(g_sizes), sizeof g_sizes - strlen(g_sizes), "%s%s", g_sizes[0] ? " " : "", one);
                ReleaseSRWLockExclusive(&g_szlk);
            }
            ID3D11Texture2D_Release(t);
        }
        if (r) ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(v[i]);
    }
    if (hit) InterlockedIncrement(&g_cx);
}
