/* В1: мод сам объявляет процесс понимающим DPI (у игры такого объявления нет).
 *   test_dpi.exe <папка с d3d11.dll и ui4k.ini> on|off
 * on  — после загрузки DLL процесс «по монитору», размер экрана у Windows = настоящему режиму монитора;
 * off — ключ dpi_aware=0: мод ничего не меняет.
 * Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef HANDLE(WINAPI *PFN_GTDAC)(void);
typedef int(WINAPI *PFN_GAFDAC)(HANDLE);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int awareness(void)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    PFN_GTDAC get = (PFN_GTDAC)(void *)GetProcAddress(u, "GetThreadDpiAwarenessContext");
    PFN_GAFDAC conv = (PFN_GAFDAC)(void *)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
    return get && conv ? conv(get()) : -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: test_dpi <dir> on|off\nFAIL\n"); return 2; }
    int on = !strcmp(argv[2], "on");
    int before = awareness();
    if (before < 0) { printf("this Windows cannot report DPI awareness - skipped\nPASS\n"); return 0; }
    CHECK(before == 0, "the test itself must start DPI-unaware (like the game), got %d", before);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary failed %lu\nFAIL\n", GetLastError()); return 1; }
    int after = awareness();
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    printf("awareness %d -> %d, screen for the process %dx%d, monitor mode %lux%lu\n", before, after, sw, sh, dm.dmPelsWidth,
           dm.dmPelsHeight);
    if (on) {
        CHECK(after == 2, "after loading the mod the process must be per-monitor DPI aware, got %d", after);
        CHECK((DWORD)sw == dm.dmPelsWidth && (DWORD)sh == dm.dmPelsHeight,
              "Windows must report the real screen size to the process: %dx%d, monitor %lux%lu", sw, sh, dm.dmPelsWidth,
              dm.dmPelsHeight);
    } else {
        CHECK(after == before, "dpi_aware=0: the mod must not change DPI awareness (%d -> %d)", before, after);
    }
    snprintf(path, sizeof path, "%s\\ui4k.log", argv[1]);
    static char log[1 << 16];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f) fclose(f);
    log[n] = 0;
    CHECK(strstr(log, on ? "dpi: process was DPI-unaware, now per-monitor DPI aware" : "dpi_aware=0"), "dpi line in the log");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
