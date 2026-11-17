/* F03: список режимов через собранную d3d11.dll (настоящий монитор этой машины).
 *   test_modes.exe <папка с d3d11.dll и ui4k.ini> <num> <den> [--off]
 * <num>/<den> — scale из ini; --off — fit_native=0 (список не меняется). Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <dxgi.h>
#include "../src/scale.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef BOOL(WINAPI *PFN_EDS)(LPCWSTR, DWORD, DEVMODEW *);
static PFN_EDS real_eds;

static int real_has(LPCWSTR dev, long w, long h)
{
    DEVMODEW d = {0};
    d.dmSize = sizeof d;
    for (DWORD i = 0; real_eds(dev, i, &d); i++)
        if ((long)d.dmPelsWidth == w && (long)d.dmPelsHeight == h) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) { printf("usage: test_modes <dir> <num> <den> [--off]\nFAIL\n"); return 2; }
    int off = argc > 4 && !strcmp(argv[4], "--off");
    Ratio k = {atoi(argv[2]), atoi(argv[3])};
    real_eds = (PFN_EDS)GetProcAddress(GetModuleHandleA("user32.dll"), "EnumDisplaySettingsW");

    /* настоящий родной режим и число режимов — до загрузки DLL */
    DEVMODEW d = {0};
    d.dmSize = sizeof d;
    long pw = 0, ph = 0;
    DWORD nreal = 0;
    long gw = 0, gh = 0;   /* наибольший режим списка */
    for (; real_eds(NULL, nreal, &d); nreal++)
        if ((long long)d.dmPelsWidth * d.dmPelsHeight > (long long)gw * gh) { gw = d.dmPelsWidth; gh = d.dmPelsHeight; }
    /* С5: родное = рабочий стол (с DSR / VSR наибольший режим списка больше монитора) */
    DEVMODEW desk = {0};
    desk.dmSize = sizeof desk;
    if (nreal && real_eds(NULL, ENUM_REGISTRY_SETTINGS, &desk)) { pw = desk.dmPelsWidth; ph = desk.dmPelsHeight; }
    if (!pw) { printf("no display modes (no monitor?) - skipped\nPASS\n"); return 0; }
    Ratio r = off ? k : ratio_fit(k, pw, ph);
    long nlw = 0, nlh = 0;
    int exact = mode_to_log(r, pw, ph, &nlw, &nlh);
    printf("native %ldx%ld, k=%d/%d -> %d/%d, logical %ldx%ld (exact %d), %lu real modes\n", pw, ph, k.num, k.den, r.num,
           r.den, nlw, nlh, exact, nreal);

    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    if (!LoadLibraryA(path)) { printf("LoadLibrary failed\nFAIL\n"); return 1; }

    /* 1. EnumDisplaySettingsW(i) через IAT этого exe */
    DWORD n = 0;
    long mw = 0, mh = 0;
    for (; EnumDisplaySettingsW(NULL, n, &d); n++) {
        if ((long long)d.dmPelsWidth * d.dmPelsHeight > (long long)mw * mh) { mw = d.dmPelsWidth; mh = d.dmPelsHeight; }
        if (!off) {
            long qw = (long)to_phys(r, d.dmPelsWidth), qh = (long)to_phys(r, d.dmPelsHeight);
            if (!real_has(NULL, qw, qh)) { CHECK(0, "mode %lux%lu -> %ldx%ld is not a real mode", d.dmPelsWidth, d.dmPelsHeight, qw, qh); break; }
        }
    }
    if (off) {
        CHECK(n == nreal && mw == gw && mh == gh, "fit_native=0: list unchanged (%lu of %lu, max %ldx%ld)", n, nreal, mw, mh);
    } else {
        CHECK(n > 0 && n <= nreal, "list size %lu (real %lu)", n, nreal);
        if (exact) CHECK(mw == nlw && mh == nlh, "max mode %ldx%ld, want %ldx%ld", mw, mh, nlw, nlh);
        printf("EnumDisplaySettingsW: %lu logical modes, max %ldx%ld\n", n, mw, mh);
    }

    /* 2. DXGI: первый выход с режимами */
    IDXGIFactory *f = NULL;
    CHECK(SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&f)), "factory");
    IDXGIOutput *o = NULL;
    IDXGIAdapter *a = NULL;
    for (UINT i = 0; f && !o && SUCCEEDED(IDXGIFactory_EnumAdapters(f, i, &a)); i++) {
        if (FAILED(IDXGIAdapter_EnumOutputs(a, 0, &o))) o = NULL;
        IDXGIAdapter_Release(a);
    }
    if (!o) {
        printf("no DXGI output - DXGI part skipped\n");
    } else {
        DXGI_OUTPUT_DESC od;
        IDXGIOutput_GetDesc(o, &od);
        const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT m = 0;
        CHECK(SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, fmt, 0, &m, NULL)) && m > 0, "mode count %u", m);
        DXGI_MODE_DESC *md = (DXGI_MODE_DESC *)calloc(m + 1, sizeof *md);
        UINT got = m;
        CHECK(SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, fmt, 0, &got, md)) && got == m, "mode list %u of %u", got, m);
        UINT small = m > 1 ? 1 : 0, g2 = small;
        if (m > 1) CHECK(IDXGIOutput_GetDisplayModeList(o, fmt, 0, &g2, md) == DXGI_ERROR_MORE_DATA && g2 == 1, "MORE_DATA");
        IDXGIOutput_GetDisplayModeList(o, fmt, 0, &got, md);
        UINT xw = 0, xh = 0;
        for (UINT i = 0; i < got; i++) {
            if ((ULONGLONG)md[i].Width * md[i].Height > (ULONGLONG)xw * xh) { xw = md[i].Width; xh = md[i].Height; }
            if (!off && !real_has(od.DeviceName, (long)to_phys(r, md[i].Width), (long)to_phys(r, md[i].Height))) {
                CHECK(0, "DXGI mode %ux%u is not logical", md[i].Width, md[i].Height);
                break;
            }
        }
        printf("GetDisplayModeList: %u modes, max %ux%u\n", got, xw, xh);
        if (!off && exact) {
            CHECK(xw == (UINT)nlw && xh == (UINT)nlh, "DXGI max %ux%u, want %ldx%ld", xw, xh, nlw, nlh);
            DXGI_MODE_DESC want = md[0], c;
            want.Width = (UINT)nlw; want.Height = (UINT)nlh;
            want.RefreshRate.Numerator = 0; want.RefreshRate.Denominator = 0;
            want.Scaling = DXGI_MODE_SCALING_UNSPECIFIED; want.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
            HRESULT hr = IDXGIOutput_FindClosestMatchingMode(o, &want, &c, NULL);
            CHECK(SUCCEEDED(hr) && c.Width == (UINT)nlw && c.Height == (UINT)nlh, "FindClosest %ldx%ld -> %ux%u hr=0x%08lx",
                  nlw, nlh, c.Width, c.Height, hr);
        }
        free(md);
        IDXGIOutput_Release(o);
    }
    if (f) IDXGIFactory_Release(f);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
