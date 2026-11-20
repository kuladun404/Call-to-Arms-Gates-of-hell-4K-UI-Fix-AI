/* F04: swapchain в физическом размере, игре — логический.
 * Путь игры (реверс 18.11, шаг 16; всё — в оконном потоке, внутри оконной процедуры):
 *  - запуск: D3D11CreateDeviceAndSwapChain(окно 1360x768, 1 буфер, оконный);
 *  - полный экран: ResizeBuffers(2, 0x0) -> SetFullscreenState(TRUE) -> ResizeBuffers(0x0) -> ResizeTarget(режим);
 *  - окно: ResizeBuffers(1) -> SetFullscreenState(FALSE) -> ResizeTarget(режим), окно — по custom_mode профиля;
 *  - смена разрешения в полном экране: SetFullscreenState(TRUE) -> ResizeBuffers(2) -> ResizeTarget(режим).
 * Логический размер берём из того, что задаёт игра; если она даёт 0 (размер окна) —
 * из физического через size_to_log, но уже известный логический не пересчитываем (screen_set_physical). */
#include "common.h"

volatile DWORD g_cdsc_tid;
volatile DWORD g_present_tid;   /* поток, который сейчас внутри настоящего Present: его рисования — оверлеев (in_present) */

static void place_window(IDXGISwapChain *sc, const char *why);   /* поток внутри D3D11CreateDeviceAndSwapChain: фабрика зовётся изнутри, desc уже ×k */

void sc_scale_desc(DXGI_SWAP_CHAIN_DESC *d)
{
    input_watch_window(d->OutputWindow);   /* F10: окно, созданное до наших хуков, тоже получает логические координаты */
    mon_track(d->OutputWindow, "swap chain");
    /* back buffer читается шейдером: копию экрана другого размера (стекло под плашкой поверх интерфейса) растягиваем,
     * а без этого флага Resolve/Copy брали бы левый верхний угол. Памяти не стоит. */
    d->BufferUsage |= DXGI_USAGE_SHADER_INPUT;
    UINT w = d->BufferDesc.Width, h = d->BufferDesc.Height;
    if (!w || !h) {   /* 0 = DXGI возьмёт размер окна (он физический) */
        RECT r;
        if (d->OutputWindow && GetClientRect(d->OutputWindow, &r))
            screen_set_physical((UINT)(r.right - r.left), (UINT)(r.bottom - r.top));
        return;
    }
    if (!preset_startup(w, h)) preset_apply(w, h);   /* L8: строка списка игры -> масштаб и мир (запуск — строка профиля) */
    place_window(NULL, "swap chain");   /* второй монитор: строка другого монитора — окно туда до полного экрана */
    screen_set_logical(w, h);
    screen_set_format(d->BufferDesc.Format);
    d->BufferDesc.Width = (UINT)to_phys(g_cfg.scale, w);
    d->BufferDesc.Height = (UINT)to_phys(g_cfg.scale, h);
}

static VtHook h_getbuf, h_getdesc, h_rb, h_rt, h_fs, h_f_csc, h_pr;
typedef HRESULT(STDMETHODCALLTYPE *PFN_PR)(IDXGISwapChain *, UINT, UINT);
static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT si, UINT f)
{
    if (!g_presented) InterlockedExchange(&g_presented, 1);   /* Э9: дальше смена масштаба — на ходу */
    copy_tick();       /* ревизия кода п. 5: запас временных текстур */
    fontpages_tick();  /* Э6: свои страницы шрифтов, отпущенные игрой */
    if (g_cfg.d3d_debug) {   /* сообщения слоя отладки — в лог */
        ID3D11Device *d = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&d)) && d) { d3dbg_drain(d); ID3D11Device_Release(d); }
    }
    world_present();   /* мир без HDR: кадр без интерфейса — мир на экран */
    if (g_cfg.diag) diag_present();
    res_origin_key();   /* Э2: Ctrl+F7 */
    cb_key();           /* Э3: Ctrl+F6 */
    draw_key();         /* Э5: Ctrl+F9 */
    if (g_cfg.perf) perf_present_pre(sc);   /* Э1: время кадра, метки видеокарты */
    /* 19.11: внутри настоящего Present рисуют оверлеи (RivaTuner — счётчик FPS, Steam, Discord) через ту же обёртку
     * контекста, а своё состояние ставят мимо неё (SwapDeviceContextState): для мода их рисование выглядит как
     * интерфейс игры — режим замеров ставил по нему «начало интерфейса» в самом начале кадра */
    /* ревизия 20.11, Ф2: их цели, viewport и шейдеры ложатся в состояние обёртки, а настоящий контекст они возвращают мимо
     * неё — после Present в обёртке снова то, что поставила игра */
    CtxPlain keep;
    int kept = ctx_present_save(&keep);
    InterlockedExchange((volatile LONG *)&g_present_tid, (LONG)GetCurrentThreadId());
    HRESULT hr = VT_GUARDED(VT_ORIG(sc, &h_pr, PFN_PR)(sc, si, f));
    InterlockedExchange((volatile LONG *)&g_present_tid, 0);
    if (kept) ctx_present_restore(&keep);
    if (g_cfg.perf) perf_present_post();
    world_frame_start();   /* следующий кадр снова с подменой — только после Present (снимки экрана оверлеями) */
    return hr;
}
typedef HRESULT(STDMETHODCALLTYPE *PFN_GETBUF)(IDXGISwapChain *, UINT, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SCDESC)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RB)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RT)(IDXGISwapChain *, const DXGI_MODE_DESC *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);

/* физический размер буфера -> логический: известный экран или size_to_log */
static void phys_to_log(UINT pw, UINT ph, UINT *lw, UINT *lh)
{
    Screen s = screen_get();
    if (s.lw && (UINT)s.pw == pw && (UINT)s.ph == ph) { *lw = (UINT)s.lw; *lh = (UINT)s.lh; return; }
    *lw = (UINT)size_to_log(g_cfg.scale, pw);
    *lh = (UINT)size_to_log(g_cfg.scale, ph);
}

static HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out)
{
    HRESULT hr = VT_GUARDED(VT_ORIG(sc, &h_getbuf, PFN_GETBUF)(sc, i, riid, out));
    if (SUCCEEDED(hr) && out && *out) {
        ID3D11Texture2D *t = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_ID3D11Texture2D, (void **)&t)) && t) {
            if (i == 0) world_set_bb(t);
            ScaledInfo si;
            int had = res_scaled((ID3D11Resource *)t, &si);
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(t, &d);   /* с меткой — логический; без — настоящий */
            if (had) { d.Width = (UINT)to_phys(si.r, si.lw); d.Height = (UINT)to_phys(si.r, si.lh); }
            UINT lw, lh;
            phys_to_log(d.Width, d.Height, &lw, &lh);
            /* L8: строка списка сменилась при том же физическом экране — буфер прежний, логический размер новый */
            if (!had || si.lw != lw || si.lh != lh || !ratio_eq(si.r, g_cfg.scale)) {
                res_mark(t, lw, lh, g_cfg.scale);
                LOG("back buffer %p: %ux%u marked as logical %ux%u", t, d.Width, d.Height, lw, lh);
            }
            ID3D11Texture2D_Release(t);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDesc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d)
{
    HRESULT hr = VT_GUARDED(VT_ORIG(sc, &h_getdesc, PFN_SCDESC)(sc, d));
    if (SUCCEEDED(hr) && d && !overlay_call(__builtin_return_address(0)))
        phys_to_log(d->BufferDesc.Width, d->BufferDesc.Height, &d->BufferDesc.Width, &d->BufferDesc.Height);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    HRESULT hr;
    world_release_bb();     /* наши ссылки на виды back buffer не должны мешать пересозданию */
    screen_set_format(f);   /* 0 = прежний */
    if (w && h) {
        preset_apply(w, h);
        screen_set_logical(w, h);
        hr = VT_GUARDED(VT_ORIG(sc, &h_rb, PFN_RB)(sc, n, (UINT)to_phys(g_cfg.scale, w), (UINT)to_phys(g_cfg.scale, h), f, fl));
    } else {
        hr = VT_GUARDED(VT_ORIG(sc, &h_rb, PFN_RB)(sc, n, w, h, f, fl));
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(hr) && SUCCEEDED(VT_ORIG(sc, &h_getdesc, PFN_SCDESC)(sc, &d)))
            screen_set_physical(d.BufferDesc.Width, d.BufferDesc.Height);
    }
    LOG("ResizeBuffers(%u, %ux%u) -> 0x%08lx", n, w, h, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_ResizeTarget(IDXGISwapChain *sc, const DXGI_MODE_DESC *m)
{
    DXGI_MODE_DESC d;
    int remark = 0;
    config_poll_profile();   /* «Применить» в игре: HDR мог смениться вместе со строкой */
    if (m) {
        d = *m;
        if (m->Width && m->Height) {
            if (preset_apply(m->Width, m->Height)) remark = 1;   /* «Применить» в игре: тут меняются масштаб и мир */
            screen_set_logical(m->Width, m->Height);
            d.Width = (UINT)to_phys(g_cfg.scale, m->Width);
            d.Height = (UINT)to_phys(g_cfg.scale, m->Height);
        }
        LOG("ResizeTarget(%ux%u @%u/%u) -> %ux%u", m->Width, m->Height, m->RefreshRate.Numerator,
            m->RefreshRate.Denominator, d.Width, d.Height);
        m = &d;
    }
    place_window(sc, "ResizeTarget");   /* второй монитор: строка другого монитора — игра переезжает туда */
    HRESULT hr = VT_GUARDED(VT_ORIG(sc, &h_rt, PFN_RT)(sc, m));
    /* физический экран тот же — игра не позовёт ResizeBuffers; back buffer сразу получает новый логический размер,
     * иначе игра пересоздаст свои экранные цели по старому */
    if (remark) {
        ID3D11Texture2D *t = NULL;
        if (SUCCEEDED(hk_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&t)) && t) ID3D11Texture2D_Release(t);
        /* и «окно сменило размер» — игра перестраивает интерфейс и экранные цели (обёртка окна делит на новый k) */
        DXGI_SWAP_CHAIN_DESC sd;
        if (SUCCEEDED(VT_ORIG(sc, &h_getdesc, PFN_SCDESC)(sc, &sd)) && sd.OutputWindow) {
            Screen s2 = screen_get();
            PostMessageW(sd.OutputWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(s2.pw, s2.ph));
            LOG("preset: posted WM_SIZE %ldx%ld (game %ldx%ld)", s2.pw, s2.ph, s2.lw, s2.lh);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_SetFullscreenState(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *o)
{
    if (fs && !o) place_window(sc, "SetFullscreenState");   /* полный экран — на мониторе строки */
    HRESULT hr = VT_GUARDED(VT_ORIG(sc, &h_fs, PFN_FS)(sc, fs, o));
    /* второй монитор: куда игра просит и где оказалась */
    WCHAR want[32] = L"-", got[32] = L"-";
    DXGI_OUTPUT_DESC od;
    if (o && SUCCEEDED(IDXGIOutput_GetDesc(o, &od))) lstrcpynW(want, od.DeviceName, 32);
    IDXGIOutput *co = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetContainingOutput(sc, &co)) && co) {
        if (SUCCEEDED(IDXGIOutput_GetDesc(co, &od))) lstrcpynW(got, od.DeviceName, 32);
        IDXGIOutput_Release(co);
    }
    LOG("SetFullscreenState(%d, output %ls) -> 0x%08lx, containing output %ls", fs, want, hr, got);
    mon_track(NULL, "SetFullscreenState");
    return hr;
}

/* Второй монитор (17.11): строка списка принадлежит своему монитору (modes.c), а полный экран DXGI включает на мониторе
 * окна. Если окно игры на мониторе другого размера, мод переносит окно на монитор строки; в полном экране — выйти,
 * перенести, войти (то же делает сама игра при смене режима). sc == NULL — swapchain ещё нет (окно в окне). */
static void place_window(IDXGISwapChain *sc, const char *why)
{
    HWND h = NULL;
    POINT to;
    if (!mon_misplaced(&h, &to)) return;
    BOOL fs = FALSE;
    IDXGIOutput *t = NULL;
    if (sc && SUCCEEDED(IDXGISwapChain_GetFullscreenState(sc, &fs, &t)) && t) IDXGIOutput_Release(t);
    HRESULT h1 = S_OK, h2 = S_OK;
    if (fs) h1 = VT_GUARDED(VT_ORIG(sc, &h_fs, PFN_FS)(sc, FALSE, NULL));
    BOOL ok = SetWindowPos(h, NULL, to.x, to.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (fs) h2 = VT_GUARDED(VT_ORIG(sc, &h_fs, PFN_FS)(sc, TRUE, NULL));
    LOG("monitor: the entry is for another monitor - game window moved to %ld,%ld%s (%s) -> %d, 0x%08lx 0x%08lx", to.x, to.y,
        fs ? " (full screen: out, move, in)" : "", why, ok, (unsigned long)h1, (unsigned long)h2);
    mon_track(NULL, why);
}

void sc_hook(IDXGISwapChain *sc)
{
    VT_PATCH(sc, IDXGISwapChainVtbl, GetBuffer, hk_GetBuffer, &h_getbuf);
    VT_PATCH(sc, IDXGISwapChainVtbl, GetDesc, hk_GetDesc, &h_getdesc);
    VT_PATCH(sc, IDXGISwapChainVtbl, ResizeBuffers, hk_ResizeBuffers, &h_rb);
    VT_PATCH(sc, IDXGISwapChainVtbl, ResizeTarget, hk_ResizeTarget, &h_rt);
    VT_PATCH(sc, IDXGISwapChainVtbl, SetFullscreenState, hk_SetFullscreenState, &h_fs);
    /* С4: всегда. Конец кадра нужен миру без HDR и при presets=0 + render=...: без него подмена цели мира после
     * первого перехода к интерфейсу не возвращалась, и мир дальше рисовался прямо в экран без теста глубины */
    VT_PATCH(sc, IDXGISwapChainVtbl, Present, hk_Present, &h_pr);
    LOG("swapchain %p hooked", sc);
}

static HRESULT STDMETHODCALLTYPE hk_FactoryCreateSwapChain(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d,
                                                           IDXGISwapChain **out)
{
    if (g_cdsc_tid == GetCurrentThreadId()) return VT_GUARDED(VT_ORIG(f, &h_f_csc, PFN_CSC)(f, dev, d, out));
    DXGI_SWAP_CHAIN_DESC n;
    if (d) {
        LOG("IDXGIFactory::CreateSwapChain %ux%u samples=%u windowed=%d hwnd=%p", d->BufferDesc.Width, d->BufferDesc.Height,
            d->SampleDesc.Count, d->Windowed, d->OutputWindow);
        config_poll_profile();                       /* игра только что применила настройки: HDR, MSAA */
        msaa_swapchain(d->SampleDesc.Count, 1);
        n = *d;
        sc_scale_desc(&n);
        d = &n;
    }
    /* Игра создаёт новый swapchain при смене MSAA. Оверлей Steam внутри настоящего создания заново ставит свои
     * перехваты в общую таблицу swapchain; если там стоят перехваты мода, он берёт их своим оригиналом, а мод его —
     * своим: круг и исчерпание стека на первом же SetFullscreenState (вылет 17.11). На время создания мод свои
     * перехваты снимает и ставит обратно после. */
    PFN_CSC real = VT_ORIG(f, &h_f_csc, PFN_CSC);
    vt_suspend();
    HRESULT hr = VT_GUARDED(real(f, dev, d, out));
    vt_resume();
    if (SUCCEEDED(hr) && out && *out) sc_hook(*out);
    return hr;
}

static void hook_factory(void *obj)
{
    IDXGIFactory *f = NULL;
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)obj, &IID_IDXGIFactory, (void **)&f)) && f) {
        VT_PATCH(f, IDXGIFactoryVtbl, CreateSwapChain, hk_FactoryCreateSwapChain, &h_f_csc);
        if (g_cfg.fit_native) modes_hook_factory(f);   /* F03 */
        IDXGIFactory_Release(f);
    }
}

typedef HRESULT(WINAPI *PFN_CF)(REFIID, void **);
static PFN_CF real_cf, real_cf1;
static HRESULT WINAPI hk_CreateDXGIFactory(REFIID riid, void **out)
{
    HRESULT hr = real_cf(riid, out);
    if (SUCCEEDED(hr) && out && *out) hook_factory(*out);
    LOG("CreateDXGIFactory -> 0x%08lx", hr);
    return hr;
}
static HRESULT WINAPI hk_CreateDXGIFactory1(REFIID riid, void **out)
{
    HRESULT hr = real_cf1(riid, out);
    if (SUCCEEDED(hr) && out && *out) hook_factory(*out);
    LOG("CreateDXGIFactory1 -> 0x%08lx", hr);
    return hr;
}

void sc_install_iat(void)
{
    HMODULE exe = GetModuleHandleW(NULL);
    int a = iat_patch(exe, "dxgi.dll", "CreateDXGIFactory", hk_CreateDXGIFactory, (void **)&real_cf);
    int b = iat_patch(exe, "dxgi.dll", "CreateDXGIFactory1", hk_CreateDXGIFactory1, (void **)&real_cf1);
    LOG("IAT: dxgi CreateDXGIFactory %d, CreateDXGIFactory1 %d", a, b);
}
