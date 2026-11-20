/* В1: игра не объявляет себя понимающей DPI. При масштабе Windows не 100% система тогда сама делит размеры окна, экрана
 * и координаты мыши (на 1.5 при 150%), а мод делит ещё раз на свой масштаб: мышь промахивается, окно не того размера.
 * До сих пор это закрывала галочка совместимости «Переопределить масштабирование: Приложение» у call_to_arms.exe
 * (флаг HIGHDPIAWARE в реестре, привязан к полному пути exe и к пользователю Windows) — у другого игрока её нет.
 * Мод объявляет процесс понимающим DPI сам, тем же способом, что и галочка (по монитору). Если процесс уже понимает
 * DPI (галочка стоит) — ничего не меняется. */
#include "common.h"

typedef HANDLE(WINAPI *PFN_GTDAC)(void);
typedef int(WINAPI *PFN_GAFDAC)(HANDLE);
typedef BOOL(WINAPI *PFN_SPDAC)(HANDLE);
typedef BOOL(WINAPI *PFN_SPDA)(void);
typedef UINT(WINAPI *PFN_GDFS)(void);

/* 0 не понимает, 1 системный, 2 по монитору; -1 — старая Windows, спросить нельзя */
static int awareness(HMODULE u)
{
    PFN_GTDAC get = (PFN_GTDAC)(void *)GetProcAddress(u, "GetThreadDpiAwarenessContext");
    PFN_GAFDAC conv = (PFN_GAFDAC)(void *)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
    return get && conv ? conv(get()) : -1;
}

static const char *name(int a)
{
    return a == 0 ? "DPI-unaware" : a == 1 ? "system DPI aware" : a == 2 ? "per-monitor DPI aware" : "unknown";
}

void dpi_init(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    int before = awareness(u);
    PFN_GDFS sysdpi = (PFN_GDFS)(void *)GetProcAddress(u, "GetDpiForSystem");
    UINT dpi = sysdpi ? sysdpi() : 0;
    if (!g_cfg.dpi_aware || before > 0) {
        LOG("dpi: process is %s (Windows scale %u%%)%s", name(before), dpi * 100 / 96,
            before > 0 ? "" : "; dpi_aware=0 - left as is, sizes and mouse are wrong unless Windows scale is 100%");
        return;
    }
    PFN_SPDAC set = (PFN_SPDAC)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    BOOL ok = set && set((HANDLE)(LONG_PTR)-3);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE — как галочка совместимости */
    if (!ok) {
        PFN_SPDA old = (PFN_SPDA)(void *)GetProcAddress(u, "SetProcessDPIAware");
        ok = old && old();
    }
    int after = awareness(u);
    if (sysdpi && after > 0) dpi = sysdpi();   /* не понимающему DPI процессу Windows отвечает 96 (100%) — спросить заново */
    LOG("dpi: process was %s, now %s%s (Windows scale %u%%)", name(before), name(after), ok ? "" : " - could not change it",
        dpi * 100 / 96);
}
