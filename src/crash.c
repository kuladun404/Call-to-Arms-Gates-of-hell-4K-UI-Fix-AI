/* Запись вылета (21.11, 2.1-alpha): при смене MSAA на ходу с DXVK игра падала в ntdll без следа в ui4k.log.
 * Векторный обработчик видит исключение первым (до обработчиков игры, Steam, DXVK). Сам он почти не тратит стек: при
 * переполнении стека его почти нет (первая версия обработчика падала сама). Он копирует запись исключения и контекст
 * в статическую память и будит заранее созданный поток записи; тот пишет в лог: код, адрес (модуль + смещение), для
 * нарушения доступа — что и по какому адресу, стек — раскрутка по таблицам функций x64 (RtlVirtualUnwind) по памяти
 * упавшего потока, и самые частые адреса возврата на стеке (круг вызовов при переполнении). Исключение не
 * обрабатывается — дальше идёт как шло. Не больше CRASH_MAX записей за запуск и одна на адрес. */
#include <stdio.h>
#include <string.h>
#include "common.h"

#define CRASH_MAX 6
static volatile LONG g_crash_n;
static void *g_crash_seen[CRASH_MAX];
static HANDLE g_go, g_done;
static volatile LONG g_busy;
static EXCEPTION_RECORD g_er;
static CONTEXT g_ctx;
static DWORD g_tid;

/* «модуль+0xсмещение» для адреса; модуль — только имя файла */
static void where(DWORD64 a, char *out, size_t n)
{
    HMODULE m = NULL;
    WCHAR path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)a, &m) && m && GetModuleFileNameW(m, path, MAX_PATH)) {
        const WCHAR *f = wcsrchr(path, L'\\');
        snprintf(out, n, "%ls+0x%llx", f ? f + 1 : path, (unsigned long long)(a - (DWORD64)m));
    } else {
        snprintf(out, n, "0x%llx", (unsigned long long)a);
    }
}

static const char *code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "divide by zero";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    default: return NULL;
    }
}

/* адрес похож на адрес возврата: внутри исполняемой памяти модуля */
static int code_addr(DWORD64 a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void report(void)
{
    static char line[2400], w[160];
    const char *name = code_name(g_er.ExceptionCode);
    where((DWORD64)g_er.ExceptionAddress, w, sizeof w);
    int p = snprintf(line, sizeof line, "CRASH? %s (0x%08lx) at %s, thread %lu", name ? name : "?", g_er.ExceptionCode, w, g_tid);
    if (g_er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && g_er.NumberParameters >= 2)
        p += snprintf(line + p, sizeof line - p, ", %s address %p",
                      g_er.ExceptionInformation[0] == 0 ? "reading" : g_er.ExceptionInformation[0] == 1 ? "writing" : "executing",
                      (void *)g_er.ExceptionInformation[1]);
    LOG("%s (first chance - if the game closes right after this, this is why)", line);

    /* стек: раскрутка копии контекста по памяти упавшего потока */
    CONTEXT c = g_ctx;
    p = snprintf(line, sizeof line, "CRASH? stack:");
    for (int f = 0; f < 40 && c.Rip && p < (int)sizeof line - 120; f++) {
        where(c.Rip, w, sizeof w);
        p += snprintf(line + p, sizeof line - p, " %s", w);
        DWORD64 base = 0;
        RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        MEMORY_BASIC_INFORMATION mi;
        if (!fn) {   /* листовая функция: адрес возврата на вершине стека */
            if (!c.Rsp || !VirtualQuery((void *)c.Rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void *hd = NULL;
        DWORD64 est = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &hd, &est, NULL);
    }
    LOG("%s", line);

    /* самые частые адреса возврата на стеке: при переполнении стека — какие функции зовут друг друга по кругу */
    MEMORY_BASIC_INFORMATION mi;
    if (!g_ctx.Rsp || !VirtualQuery((void *)g_ctx.Rsp, &mi, sizeof mi)) return;
    DWORD64 *s = (DWORD64 *)(g_ctx.Rsp & ~7ull), *e = (DWORD64 *)((BYTE *)mi.BaseAddress + mi.RegionSize);
    if (e - s > 262144) e = s + 262144;   /* до 2 МБ */
    static DWORD64 adr[64];
    static unsigned cnt[64];
    int na = 0;
    for (DWORD64 *q = s; q < e; q++) {
        DWORD64 v = *q;
        int i;
        for (i = 0; i < na && adr[i] != v; i++) {}
        if (i < na) { cnt[i]++; continue; }
        if (na < 64 && code_addr(v)) { adr[na] = v; cnt[na++] = 1; }
    }
    p = snprintf(line, sizeof line, "CRASH? return addresses on the stack (%lld KB from the top), most frequent:",
                 (long long)((BYTE *)e - (BYTE *)s) / 1024);
    for (int k = 0; k < 12; k++) {
        int b = -1;
        for (int i = 0; i < na; i++)
            if (cnt[i] && (b < 0 || cnt[i] > cnt[b])) b = i;
        if (b < 0) break;
        where(adr[b], w, sizeof w);
        p += snprintf(line + p, sizeof line - p, " %s x%u", w, cnt[b]);
        cnt[b] = 0;
    }
    LOG("%s", line);
}

static DWORD WINAPI writer(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_go, INFINITE);
        report();
        SetEvent(g_done);
    }
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (!code_name(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < CRASH_MAX; i++)
        if (g_crash_seen[i] == er->ExceptionAddress) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crash_n) > CRASH_MAX || InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_crash_seen[g_crash_n - 1] = er->ExceptionAddress;
    g_er = *er;
    g_ctx = *ep->ContextRecord;
    g_tid = GetCurrentThreadId();
    SetEvent(g_go);
    WaitForSingleObject(g_done, 8000);
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_install(void)
{
    g_go = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = g_go && g_done ? CreateThread(NULL, 256 * 1024, writer, NULL, 0, NULL) : NULL;
    if (!t) return;
    CloseHandle(t);
    if (AddVectoredExceptionHandler(1, crash_veh)) LOG("crash: fatal exceptions are written to this log (where the game fell)");
}
