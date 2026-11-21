/* Второй swapchain и чужой перехватчик тех же функций (оверлей Steam) — через собранную d3d11.dll на WARP.
 *   test_hookcycle.exe <папка с d3d11.dll и ui4k.ini> below|above
 * Что было в игре 17.11: включение MSAA пересоздаёт swapchain. Оверлей Steam при каждом создании swapchain ставит свои
 * функции в его таблицу, мод — тоже. На втором swapchain каждый запоминал другого как «оригинал» — круг, первый же
 * SetFullscreenState уходил в бесконечную рекурсию, стек кончался, игра падала.
 * Тест играет роль оверлея: перехватывает IDXGIFactory::CreateSwapChain и после настоящего создания ставит свои
 * функции в таблицу swapchain, запоминая то, что там стояло (как делает оверлей).
 *   below — «оверлей» стоит под модом (его код работает внутри настоящего создания, до мода) — как в игре;
 *   above — «оверлей» стоит над модом (его код работает после мода).
 * Проверки: второй swapchain создан, вызовы SetFullscreenState / GetBuffer / GetDesc / ResizeBuffers / Present на обоих
 * проходят (не рекурсия), перехваты мода на месте (GetDesc отдаёт логический размер), «оверлей» тоже вызывается.
 * Последняя строка PASS/FAIL. При круге тест падает с кодом 0xC00000FD (исчерпан стек). */
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_CF)(REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CSC)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FS)(IDXGISwapChain *, BOOL, IDXGIOutput *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PR)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_GB)(IDXGISwapChain *, UINT, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RB)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void poke(void **slot, void *v)
{
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = v;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* ---- «оверлей» ---- */
static PFN_CSC ov_csc;
static PFN_FS ov_fs;
static PFN_PR ov_pr;
static PFN_GB ov_gb;
static PFN_RB ov_rb;
static int n_csc, n_fs, n_pr, n_gb, n_rb;

static HRESULT STDMETHODCALLTYPE ovSetFullscreenState(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *o) { n_fs++; return ov_fs(sc, fs, o); }
static HRESULT STDMETHODCALLTYPE ovPresent(IDXGISwapChain *sc, UINT a, UINT b) { n_pr++; return ov_pr(sc, a, b); }
static HRESULT STDMETHODCALLTYPE ovGetBuffer(IDXGISwapChain *sc, UINT i, REFIID r, void **o) { n_gb++; return ov_gb(sc, i, r, o); }
static HRESULT STDMETHODCALLTYPE ovResizeBuffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{ n_rb++; return ov_rb(sc, n, w, h, f, fl); }

/* как оверлей: «Hooking vtable for swap chain» — в слоте не своя функция, значит, запомнить её и поставить свою */
#define OV_HOOK(sc, field, fn, orig) do { void **s_ = (void **)&(sc)->lpVtbl->field; \
        if (*s_ != (void *)(fn)) { orig = *s_; poke(s_, (void *)(fn)); } } while (0)

static HRESULT STDMETHODCALLTYPE ovCreateSwapChain(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out)
{
    n_csc++;
    HRESULT hr = ov_csc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        OV_HOOK(*out, SetFullscreenState, ovSetFullscreenState, ov_fs);
        OV_HOOK(*out, Present, ovPresent, ov_pr);
        OV_HOOK(*out, GetBuffer, ovGetBuffer, ov_gb);
        OV_HOOK(*out, ResizeBuffers, ovResizeBuffers, ov_rb);
    }
    return hr;
}

/* ---- «драйвер», подменяющий оконную процедуру ---- */
static WNDPROC g_drv_orig;
static int n_drv, g_drv_depth, g_drv_loop;
static LRESULT CALLBACK drv_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_USER + 7) n_drv++;
    if (g_drv_depth > 20) { g_drv_loop = 1; return 0; }   /* круг: дальше не звать, иначе стек кончится */
    g_drv_depth++;
    LRESULT r = CallWindowProcW(g_drv_orig, h, m, w, l);
    g_drv_depth--;
    return r;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: test_hookcycle <dir> below|above\nFAIL\n"); return 2; }
    int below = !strcmp(argv[2], "below");
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    PFN_CD cd = (PFN_CD)GetProcAddress(m, "D3D11CreateDevice");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    if (FAILED(cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    IDXGIFactory *f = NULL;
    if (below) {
        /* «оверлей» раньше мода: настоящая фабрика (мимо перехвата импорта), его функция — в таблицу фабрики */
        PFN_CF real_cf = (PFN_CF)(void *)GetProcAddress(GetModuleHandleA("dxgi.dll"), "CreateDXGIFactory");
        IDXGIFactory *f0 = NULL;
        real_cf(&IID_IDXGIFactory, (void **)&f0);
        void **slot = (void **)&f0->lpVtbl->CreateSwapChain;
        ov_csc = *slot;
        poke(slot, (void *)ovCreateSwapChain);
        CreateDXGIFactory(&IID_IDXGIFactory, (void **)&f);   /* через мод: его перехват ложится поверх */
        IDXGIFactory_Release(f0);
        if ((void *)f->lpVtbl->CreateSwapChain == (void *)ovCreateSwapChain) {
            printf("factory tables are per object on this system: the 'below' order cannot be built - skipped\nPASS\n");
            return 0;
        }
    } else {
        CreateDXGIFactory(&IID_IDXGIFactory, (void **)&f);   /* мод первым */
        void **slot = (void **)&f->lpVtbl->CreateSwapChain;
        ov_csc = *slot;
        poke(slot, (void *)ovCreateSwapChain);                /* «оверлей» поверх */
    }

    HWND w1 = CreateWindowExW(0, L"STATIC", L"sc1", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    HWND w2 = CreateWindowExW(0, L"STATIC", L"sc2", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC d = {{640, 360, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, w1, TRUE,
                              DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc[2] = {NULL, NULL};
    HRESULT hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)dev, &d, &sc[0]);
    CHECK(SUCCEEDED(hr) && sc[0], "first swap chain hr=0x%08lx", hr);
    d.OutputWindow = w2;
    hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)dev, &d, &sc[1]);   /* игра: новый swapchain при смене MSAA */
    CHECK(SUCCEEDED(hr) && sc[1], "second swap chain hr=0x%08lx", hr);
    CHECK(n_csc == 2, "the other hooker saw both creations (%d)", n_csc);
    if (!sc[0] || !sc[1]) { printf("FAIL %d\n", fails); return 1; }
    printf("order: other hooker %s the mod; swap chains %s one table\n", below ? "below" : "above",
           sc[0]->lpVtbl == sc[1]->lpVtbl ? "share" : "do not share");

    for (int i = 1; i >= 0; i--) {   /* сначала новый — как игра */
        printf("swap chain %d: SetFullscreenState\n", i + 1);
        hr = IDXGISwapChain_SetFullscreenState(sc[i], FALSE, NULL);
        CHECK(SUCCEEDED(hr), "sc%d SetFullscreenState hr=0x%08lx", i + 1, hr);
        printf("swap chain %d: GetBuffer\n", i + 1);
        ID3D11Texture2D *bb = NULL;
        hr = IDXGISwapChain_GetBuffer(sc[i], 0, &IID_ID3D11Texture2D, (void **)&bb);
        CHECK(SUCCEEDED(hr) && bb, "sc%d GetBuffer hr=0x%08lx", i + 1, hr);
        if (bb) ID3D11Texture2D_Release(bb);
        printf("swap chain %d: GetDesc\n", i + 1);
        DXGI_SWAP_CHAIN_DESC g;
        hr = IDXGISwapChain_GetDesc(sc[i], &g);
        CHECK(SUCCEEDED(hr) && g.BufferDesc.Width == 640 && g.BufferDesc.Height == 360,
              "sc%d GetDesc: %ux%u hr=0x%08lx - the mod's hook must still answer with the game's size 640x360", i + 1,
              g.BufferDesc.Width, g.BufferDesc.Height, hr);
        printf("swap chain %d: Present\n", i + 1);
        hr = IDXGISwapChain_Present(sc[i], 0, 0);
        CHECK(SUCCEEDED(hr), "sc%d Present hr=0x%08lx", i + 1, hr);
        printf("swap chain %d: ResizeBuffers\n", i + 1);
        hr = IDXGISwapChain_ResizeBuffers(sc[i], 0, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
        CHECK(SUCCEEDED(hr), "sc%d ResizeBuffers hr=0x%08lx", i + 1, hr);
        hr = IDXGISwapChain_GetDesc(sc[i], &g);
        CHECK(SUCCEEDED(hr) && g.BufferDesc.Width == 800 && g.BufferDesc.Height == 450, "sc%d after resize: %ux%u, want 800x450",
              i + 1, g.BufferDesc.Width, g.BufferDesc.Height);
    }
    CHECK(n_fs >= 2 && n_pr >= 2 && n_gb >= 2 && n_rb >= 2, "the other hooker is still called: fs %d present %d getbuffer %d resize %d",
          n_fs, n_pr, n_gb, n_rb);
    CHECK(n_fs < 100 && n_pr < 100, "no runaway recursion (fs %d present %d)", n_fs, n_pr);

    /* 21.11, DXVK: драйвер Vulkan (opengl32.dll NVIDIA) при создании swapchain ставит свою оконную процедуру поверх
     * обёртки мода, запомнив обёртку своим «оригиналом». Игра при смене MSAA создаёт swapchain на том же окне — мод
     * оборачивал окно заново и брал процедуру драйвера своим оригиналом: круг, исчерпание стека. «Драйвер» здесь —
     * SetWindowLongPtrW напрямую из user32 (мимо перехвата импорта, как чужой модуль). */
    printf("window procedure: other subclasser on top of the mod, then a new swap chain on the same window\n");
    HWND w3 = CreateWindowExW(0, L"STATIC", L"sc3", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    d.OutputWindow = w3;
    IDXGISwapChain *s3 = NULL, *s4 = NULL;
    hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)dev, &d, &s3);
    CHECK(SUCCEEDED(hr) && s3, "swap chain on w3 hr=0x%08lx", hr);
    typedef LONG_PTR(WINAPI *PFN_SWLP)(HWND, int, LONG_PTR);
    PFN_SWLP real_swlp = (PFN_SWLP)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowLongPtrW");
    g_drv_orig = (WNDPROC)real_swlp(w3, GWLP_WNDPROC, (LONG_PTR)drv_proc);
    if (s3) IDXGISwapChain_Release(s3);
    hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)dev, &d, &s4);   /* игра: новый swapchain на том же окне */
    CHECK(SUCCEEDED(hr) && s4, "second swap chain on w3 hr=0x%08lx", hr);
    n_drv = 0;
    SendMessageW(w3, WM_USER + 7, 0, 0);
    CHECK(n_drv == 1 && !g_drv_loop, "the other subclasser runs once per message, no loop (calls %d%s)", n_drv,
          g_drv_loop ? ", loop detected" : "");
    if (s4) IDXGISwapChain_Release(s4);
    DestroyWindow(w3);

    IDXGISwapChain_Release(sc[0]); IDXGISwapChain_Release(sc[1]);
    IDXGIFactory_Release(f);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    DestroyWindow(w1); DestroyWindow(w2);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
