#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <share.h>

static FILE *g_logf;
static SRWLOCK g_loglock = SRWLOCK_INIT;

/* Ревизия кода 17.11, п. 4: лог открывается с запретом записи другим. Если его уже пишет другая копия игры (так было
 * 17.11: две копии при одном запуске, вторая открыла лог заново и затёрла начало первой), эта пишет свой — ui4k.2.log
 * и дальше; чужой не трогает. Читать лог во время игры можно. */
void log_open(const WCHAR *path)
{
    if (g_logf) return;
    g_logf = _wfsopen(path, L"w", _SH_DENYWR);
    if (g_logf) return;
    WCHAR alt[MAX_PATH];
    for (int i = 2; i <= 4 && !g_logf; i++) {
        lstrcpynW(alt, path, MAX_PATH - 8);
        WCHAR *dot = wcsrchr(alt, L'.');
        if (!dot) break;
        wsprintfW(dot, L".%d.log", i);
        g_logf = _wfsopen(alt, L"w", _SH_DENYWR);
    }
    if (g_logf) ui4k_log("log: %ls is written by another copy of the game - this copy writes %ls", path, alt);
}

void ui4k_log(const char *fmt, ...)
{
    if (!g_logf) return;
    AcquireSRWLockExclusive(&g_loglock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_logf, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list a;
    va_start(a, fmt);
    vfprintf(g_logf, fmt, a);
    va_end(a);
    fputc('\n', g_logf);
    fflush(g_logf);
    ReleaseSRWLockExclusive(&g_loglock);
}
