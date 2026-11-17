/* F10: координаты User32 через собранную d3d11.dll при ×1.5. Прокси перехватывает импорт ЭТОГО exe, поэтому
 * обычные вызовы user32 ниже идут через хуки, а «настоящие» значения берём через GetProcAddress.
 * Курсор не двигаем и ClipCursor не зовём — тест не трогает рабочий стол.
 *   test_input.exe <папка с d3d11.dll и ui4k.ini (scale=1.5)>
 * Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ×1.5: те же правила, что в src/scale.h */
static long phys(long x) { return (2 * x * 3 + 2) / 4; }            /* round half up, x >= 0 */
static long size_log(long p) { return p * 2 / 3; }

static HMODULE U;
#define REAL(name, T) ((T)GetProcAddress(U, #name))
typedef BOOL(WINAPI *PFN_RECT)(HWND, RECT *);
typedef int(WINAPI *PFN_GSM)(int);
typedef BOOL(WINAPI *PFN_GCP)(POINT *);

static UINT last_msg;
static LPARAM last_l;
static int proc_id;
static LRESULT nc_result;
static LRESULT CALLBACK proc1(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE || m == WM_SIZE || m == WM_USER + 1) { last_msg = m; last_l = l; proc_id = 1; return 0; }
    if (m == WM_NCHITTEST) { last_l = l; nc_result = DefWindowProcW(h, m, w, l); return nc_result; }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT CALLBACK proc2(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEMOVE) { last_msg = m; last_l = l; proc_id = 2; return 0; }
    return CallWindowProcW(proc1, h, m, w, l);
}

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: test_input <dir>\nFAIL\n"); return 2; }
    U = GetModuleHandleA("user32.dll");

    /* окно, созданное ДО загрузки прокси (как окно игры до хуков); его процедуру прокси обернёт при создании swapchain */
    HWND early = CreateWindowExW(0, L"STATIC", L"early", WS_POPUP, 0, 0, 300, 200, NULL, NULL, NULL, NULL);
    SetWindowLongPtrW(early, GWLP_WNDPROC, (LONG_PTR)proc1);

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed\nFAIL\n"); return 1; }

    /* 1. метрики и монитор */
    int rcx = REAL(GetSystemMetrics, PFN_GSM)(SM_CXSCREEN), rcy = REAL(GetSystemMetrics, PFN_GSM)(SM_CYSCREEN);
    CHECK(GetSystemMetrics(SM_CXSCREEN) == size_log(rcx) && GetSystemMetrics(SM_CYSCREEN) == size_log(rcy),
          "SM_CXSCREEN %d (real %d)", GetSystemMetrics(SM_CXSCREEN), rcx);
    CHECK(GetSystemMetrics(SM_CXBORDER) == REAL(GetSystemMetrics, PFN_GSM)(SM_CXBORDER), "border metric untouched");
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    CHECK(mi.rcMonitor.right == size_log(rcx), "monitor right %ld, want %ld", mi.rcMonitor.right, size_log(rcx));
    /* настоящие пиксели режима (у процесса без DPI-awareness GetSystemMetrics уже уменьшен Windows — не эталон) */
    typedef BOOL(WINAPI *PFN_EDS)(LPCWSTR, DWORD, DEVMODEW *);
    DEVMODEW dm = {0}, rdm = {0};
    dm.dmSize = rdm.dmSize = sizeof dm;
    if (REAL(EnumDisplaySettingsW, PFN_EDS)(NULL, ENUM_CURRENT_SETTINGS, &rdm) &&
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm))
        CHECK((long)dm.dmPelsWidth == size_log((long)rdm.dmPelsWidth), "EnumDisplaySettings %lu, real %lu", dm.dmPelsWidth,
              rdm.dmPelsWidth);
    POINT cp, rp;
    REAL(GetCursorPos, PFN_GCP)(&rp);
    GetCursorPos(&cp);
    /* курсор мог сдвинуться между вызовами — проверяем только согласованность с правилом на прочитанном значении */
    CHECK(phys(cp.x) <= rp.x + 2 && cp.x <= size_log(rp.x) + 2, "GetCursorPos %ld vs real %ld", cp.x, rp.x);

    /* 2. окно: создаётся ×1.5, игре — логические прямоугольники */
    WNDCLASSEXW wc = {sizeof wc, 0, proc1, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"ui4k_test", NULL};
    CHECK(RegisterClassExW(&wc) != 0, "RegisterClassExW");
    HWND h = CreateWindowExW(0, L"ui4k_test", L"t", WS_POPUP, 1000, 600, 400, 300, NULL, NULL, NULL, NULL);
    CHECK(h != NULL, "CreateWindowExW");
    RECT r, rr;
    REAL(GetWindowRect, PFN_RECT)(h, &rr);
    CHECK(rr.left == 1500 && rr.top == 900 && rr.right == 2100 && rr.bottom == 1350, "real window [%ld %ld %ld %ld]",
          rr.left, rr.top, rr.right, rr.bottom);
    GetWindowRect(h, &r);
    CHECK(r.left == 1000 && r.top == 600 && r.right == 1400 && r.bottom == 900, "GetWindowRect [%ld %ld %ld %ld]",
          r.left, r.top, r.right, r.bottom);
    GetClientRect(h, &r);
    CHECK(r.right == 400 && r.bottom == 300, "GetClientRect %ldx%ld", r.right, r.bottom);
    CHECK(GetWindowLongPtrW(h, GWLP_WNDPROC) == (LONG_PTR)proc1, "GetWindowLongPtr returns game's proc");

    /* 3. сообщения: физические координаты приходят логическими */
    SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(643, 449));
    CHECK(last_msg == WM_MOUSEMOVE && GET_X_LPARAM(last_l) == 428 && GET_Y_LPARAM(last_l) == 299,
          "WM_MOUSEMOVE (%d,%d), want (428,299)", GET_X_LPARAM(last_l), GET_Y_LPARAM(last_l));
    SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(644, 450));
    CHECK(GET_X_LPARAM(last_l) == 429 && GET_Y_LPARAM(last_l) == 300, "WM_MOUSEMOVE edge (%d,%d)", GET_X_LPARAM(last_l),
          GET_Y_LPARAM(last_l));
    SendMessageW(h, WM_SIZE, 0, MAKELPARAM(3840, 2160));
    CHECK(LOWORD(last_l) == 2560 && HIWORD(last_l) == 1440, "WM_SIZE %ux%u", LOWORD(last_l), HIWORD(last_l));
    SendMessageW(h, WM_USER + 1, 0, MAKELPARAM(643, 449));
    CHECK(LOWORD(last_l) == 643, "other messages untouched");
    /* DefWindowProcW получает исходные (физические) координаты: точка (1800,1100) внутри окна в физике
     * [1500..2100]x[900..1350]; игре — (1200,733), что в физике было бы вне окна */
    SendMessageW(h, WM_NCHITTEST, 0, MAKELPARAM(1800, 1100));
    CHECK(GET_X_LPARAM(last_l) == 1200 && nc_result == HTCLIENT, "WM_NCHITTEST game sees %d, DefWindowProc -> %lld",
          GET_X_LPARAM(last_l), (long long)nc_result);

    /* 4. игра меняет процедуру: обёртка остаётся, новая процедура получает логические координаты */
    LONG_PTR prev = SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)proc2);
    CHECK(prev == (LONG_PTR)proc1, "SetWindowLongPtr returns previous game proc");
    CHECK(GetWindowLongPtrW(h, GWLP_WNDPROC) == (LONG_PTR)proc2, "GetWindowLongPtr returns new proc");
    SendMessageW(h, WM_MOUSEMOVE, 0, MAKELPARAM(643, 449));
    CHECK(proc_id == 2 && GET_X_LPARAM(last_l) == 428, "after SetWindowLongPtr: proc %d x %d", proc_id, GET_X_LPARAM(last_l));

    /* 5. точки экран <-> клиент */
    POINT p = {1100, 650};
    ScreenToClient(h, &p);
    CHECK(p.x == 100 && p.y == 50, "ScreenToClient (%ld,%ld)", p.x, p.y);
    ClientToScreen(h, &p);
    CHECK(p.x == 1100 && p.y == 650, "ClientToScreen (%ld,%ld)", p.x, p.y);
    POINT q = {1100, 650};
    MapWindowPoints(NULL, h, &q, 1);
    CHECK(q.x == 100 && q.y == 50, "MapWindowPoints (%ld,%ld)", q.x, q.y);

    /* 6. SetWindowPos в логике */
    SetWindowPos(h, NULL, 200, 100, 600, 400, SWP_NOZORDER | SWP_NOACTIVATE);
    REAL(GetWindowRect, PFN_RECT)(h, &rr);
    CHECK(rr.left == 300 && rr.top == 150 && rr.right == 1200 && rr.bottom == 750, "SetWindowPos real [%ld %ld %ld %ld]",
          rr.left, rr.top, rr.right, rr.bottom);
    DestroyWindow(h);

    /* 7. окно, созданное до прокси: при создании swapchain его процедура оборачивается */
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    DXGI_SWAP_CHAIN_DESC scd = {{200, 100, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0},
                                DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, early, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    CHECK(SUCCEEDED(hr), "swapchain on early window hr=0x%08lx", hr);
    proc_id = 0;
    SendMessageW(early, WM_MOUSEMOVE, 0, MAKELPARAM(643, 449));
    CHECK(proc_id == 1 && GET_X_LPARAM(last_l) == 428, "early window: proc %d x %d", proc_id, GET_X_LPARAM(last_l));
    if (sc) IDXGISwapChain_Release(sc);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    DestroyWindow(early);

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
