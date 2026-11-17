/* L8: подписи строк в списке разрешений игры — без мода и без правки файлов игры.
 * Игра при запуске перебирает папку localizations (FindFirstFileW/FindNextFileW "localizations\*.*") и читает
 * все .pak оттуда; подпись значения ищет по ключу "options/video/resolution#/<ШxВ>", без перевода пишет само ШxВ.
 * Мы пишем свой ui4k_labels.pak рядом с DLL (zip без сжатия: <язык>/interface/text/ui4k.po) и при переборе папки
 * показываем игре ещё один файл ui4k.pak; его открытие уводим в наш файл. В папке игры ничего не появляется,
 * список модов не меняется — мультиплеер видит то же, что без нас.
 * При diag=1 — ещё лог всех обращений к localizations / mods / .po (разведка). */
#include <stdio.h>
#include <string.h>
#include "common.h"
#include <shlobj.h>

#define VNAME L"ui4k.pak"
static WCHAR g_pak[MAX_PATH];          /* настоящий файл: <папка DLL>\ui4k_labels.pak */
static int g_on;                       /* пакет записан — подмешиваем */

/* ---- подписи ---- */
typedef struct { char mode[24]; char ru[128], en[96]; } Label;
static Label g_lab[64];
static int g_nlab;

void loc_add(const char *mode, const char *ru, const char *en)
{
    if (g_nlab >= (int)(sizeof g_lab / sizeof *g_lab)) return;
    lstrcpynA(g_lab[g_nlab].mode, mode, sizeof g_lab[0].mode);
    lstrcpynA(g_lab[g_nlab].ru, ru, sizeof g_lab[0].ru);
    lstrcpynA(g_lab[g_nlab].en, en, sizeof g_lab[0].en);
    g_nlab++;
}

static size_t po_text(char *b, size_t n, int pot)
{
    size_t p = (size_t)snprintf(b, n, "msgid \"\"\nmsgstr \"\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\n");
    for (int i = 0; i < g_nlab && p < n; i++)
        p += (size_t)snprintf(b + p, n - p, "msgctxt \"options/video/resolution#/%s\"\nmsgid \"%s\"\nmsgstr \"%s\"\n\n",
                              g_lab[i].mode, pot ? g_lab[i].en : g_lab[i].mode, pot ? "" : g_lab[i].ru);
    return p < n ? p : n;
}

/* ---- zip (stored) ---- */
static DWORD crc32(const BYTE *d, size_t n)
{
    DWORD c = 0xffffffff;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

typedef struct { BYTE *b; size_t n, cap; } Buf;
static void put(Buf *z, const void *d, size_t n)
{
    if (z->n + n <= z->cap) memcpy(z->b + z->n, d, n);
    z->n += n;
}
static void put16(Buf *z, unsigned v) { BYTE t[2] = {(BYTE)v, (BYTE)(v >> 8)}; put(z, t, 2); }
static void put32(Buf *z, DWORD v) { put16(z, v & 0xffff); put16(z, v >> 16); }

/* на диске уже ровно эти байты */
static int same_file(const WCHAR *p, const BYTE *b, size_t n)
{
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static BYTE tmp[40000];
    LARGE_INTEGER sz;
    DWORD rd = 0;
    int ok = GetFileSizeEx(h, &sz) && sz.QuadPart == (LONGLONG)n && n <= sizeof tmp && ReadFile(h, tmp, (DWORD)n, &rd, NULL) &&
             rd == n && !memcmp(tmp, b, n);
    CloseHandle(h);
    return ok;
}

static int put_file(const WCHAR *p, const BYTE *b, size_t n, DWORD *err)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { *err = GetLastError(); return 0; }
    DWORD w = 0;
    BOOL ok = WriteFile(h, b, (DWORD)n, &w, NULL) && w == n;
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) DeleteFileW(p);   /* недописанный пакет игре не показывать */
    return ok;
}

static int write_pak(void)
{
    static char po[2][16384];
    static const char *names[2] = {"ru/interface/text/ui4k.po", "default/interface/text/ui4k.pot"};
    size_t len[2] = {po_text(po[0], sizeof po[0], 0), po_text(po[1], sizeof po[1], 1)};
    static BYTE out[40000];
    Buf z = {out, 0, sizeof out};
    DWORD off[2], crc[2];
    for (int i = 0; i < 2; i++) {
        off[i] = (DWORD)z.n;
        crc[i] = crc32((BYTE *)po[i], len[i]);
        put32(&z, 0x04034b50); put16(&z, 10); put16(&z, 0); put16(&z, 0); put16(&z, 0); put16(&z, 0x21);
        put32(&z, crc[i]); put32(&z, (DWORD)len[i]); put32(&z, (DWORD)len[i]);
        put16(&z, (unsigned)strlen(names[i])); put16(&z, 0);
        put(&z, names[i], strlen(names[i])); put(&z, po[i], len[i]);
    }
    DWORD cd = (DWORD)z.n;
    for (int i = 0; i < 2; i++) {
        put32(&z, 0x02014b50); put16(&z, 20); put16(&z, 10); put16(&z, 0); put16(&z, 0); put16(&z, 0); put16(&z, 0x21);
        put32(&z, crc[i]); put32(&z, (DWORD)len[i]); put32(&z, (DWORD)len[i]);
        put16(&z, (unsigned)strlen(names[i])); put16(&z, 0); put16(&z, 0); put16(&z, 0); put16(&z, 0);
        put32(&z, 0); put32(&z, off[i]); put(&z, names[i], strlen(names[i]));
    }
    DWORD cdn = (DWORD)z.n - cd;
    put32(&z, 0x06054b50); put16(&z, 0); put16(&z, 0); put16(&z, 2); put16(&z, 2); put32(&z, cdn); put32(&z, cd); put16(&z, 0);
    if (z.n > z.cap) { LOG("labels: pak too big"); return 0; }
    /* Ревизия кода 17.11, пп. 3–4: рядом с DLL; уже лежит такой же — им и пользуемся (вторая копия игры: файл занят
     * первой, 17.11 подписи у неё выключались); записать нельзя (папка игры только для чтения) — в папку пользователя
     * %LOCALAPPDATA%\ui4k. Игре путь не виден: открытие ui4k.pak подменяется по имени. */
    WCHAR cand[2][MAX_PATH], ad[MAX_PATH] = L"";
    int nc = 0, user = -1;
    if (dir_file(cand[nc], L"ui4k_labels.pak")) nc++;   /* ревизия кода 4, п. 4: путь не помещается — сразу папка пользователя */
    if (g_appdata[0]) lstrcpynW(ad, g_appdata, MAX_PATH - 24);
    else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, ad)) && lstrlenW(ad) < MAX_PATH - 24) lstrcatW(ad, L"\\ui4k");
    else ad[0] = 0;
    if (ad[0]) { wsprintfW(cand[nc], L"%s\\ui4k_labels.pak", ad); user = nc++; }
    for (int i = 0; i < nc; i++) {
        if (i == user) CreateDirectoryW(ad, NULL);
        if (same_file(cand[i], out, z.n)) {
            lstrcpyW(g_pak, cand[i]);
            LOG("labels: %d labels - %ls is already up to date", g_nlab, g_pak);
            return 1;
        }
        DWORD err = 0;
        if (put_file(cand[i], out, z.n, &err)) {
            lstrcpyW(g_pak, cand[i]);
            LOG("labels: %d labels -> %ls (%u bytes)", g_nlab, g_pak, (unsigned)z.n);
            return 1;
        }
        LOG("labels: cannot write %ls (%lu)%s", cand[i], err, i + 1 < nc ? " - trying the user folder" : "");
    }
    return 0;
}

/* ---- пути ---- */
static void norm(const WCHAR *p, WCHAR *b)
{
    lstrcpynW(b, p ? p : L"", MAX_PATH);
    CharLowerW(b);
    for (WCHAR *q = b; *q; q++) if (*q == L'\\') *q = L'/';
}
static int ends(const WCHAR *s, const WCHAR *t)
{
    size_t a = wcslen(s), b = wcslen(t);
    return a >= b && !wcscmp(s + a - b, t);
}
static int is_virtual(const WCHAR *p)
{
    WCHAR b[MAX_PATH];
    norm(p, b);
    return g_on && ends(b, L"/localizations/" VNAME);
}
static int is_locdir_scan(const WCHAR *p)
{
    WCHAR b[MAX_PATH];
    norm(p, b);
    return ends(b, L"/localizations/*.*") || ends(b, L"/localizations/*");
}
static int interesting(const WCHAR *p)
{
    WCHAR b[MAX_PATH];
    norm(p, b);
    return wcsstr(b, L"localiz") || wcsstr(b, L"/mods") || wcsstr(b, L".po");
}

/* ---- перебор папки: после последнего настоящего файла — наш ---- */
static struct { HANDLE h; int done; } g_scan[8];
static SRWLOCK g_sl = SRWLOCK_INIT;

static HANDLE WINAPI hk_FindFirstFileW(LPCWSTR p, LPWIN32_FIND_DATAW d)
{
    HANDLE h = FindFirstFileW(p, d);
    if (g_cfg.diag && interesting(p)) LOG("fs: FindFirstFileW '%ls' -> %s", p, h == INVALID_HANDLE_VALUE ? "none" : "found");
    if (g_on && h != INVALID_HANDLE_VALUE && is_locdir_scan(p)) {
        AcquireSRWLockExclusive(&g_sl);
        for (int i = 0; i < 8; i++)
            if (!g_scan[i].h) { g_scan[i].h = h; g_scan[i].done = 0; break; }
        ReleaseSRWLockExclusive(&g_sl);
    }
    return h;
}

static BOOL WINAPI hk_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW d)
{
    BOOL r = FindNextFileW(h, d);
    if (r || GetLastError() != ERROR_NO_MORE_FILES) return r;
    int mine = 0;
    AcquireSRWLockExclusive(&g_sl);
    for (int i = 0; i < 8; i++)
        if (g_scan[i].h == h && !g_scan[i].done) { g_scan[i].done = 1; mine = 1; break; }
    ReleaseSRWLockExclusive(&g_sl);
    if (!mine) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_pak, GetFileExInfoStandard, &a)) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    ZeroMemory(d, sizeof *d);
    d->dwFileAttributes = a.dwFileAttributes;
    d->ftCreationTime = a.ftCreationTime; d->ftLastAccessTime = a.ftLastAccessTime; d->ftLastWriteTime = a.ftLastWriteTime;
    d->nFileSizeHigh = a.nFileSizeHigh; d->nFileSizeLow = a.nFileSizeLow;
    lstrcpyW(d->cFileName, VNAME);
    LOG("labels: ui4k.pak shown in localizations");
    return TRUE;
}

static BOOL WINAPI hk_FindClose(HANDLE h)
{
    AcquireSRWLockExclusive(&g_sl);
    for (int i = 0; i < 8; i++)
        if (g_scan[i].h == h) g_scan[i].h = NULL;
    ReleaseSRWLockExclusive(&g_sl);
    return FindClose(h);
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t)
{
    int v = is_virtual(p);
    HANDLE h = CreateFileW(v ? g_pak : p, a, s, sa, c, f, t);
    if (v) LOG("labels: open ui4k.pak -> %p", h);
    else if (g_cfg.diag && interesting(p)) LOG("fs: CreateFileW '%ls' acc=0x%lx -> %p", p, a, h);
    return h;
}

static DWORD WINAPI hk_GetFileAttributesW(LPCWSTR p)
{
    DWORD r = GetFileAttributesW(is_virtual(p) ? g_pak : p);
    if (g_cfg.diag && interesting(p)) LOG("fs: GetFileAttributesW '%ls' -> 0x%lx", p, r);
    return r;
}

static BOOL WINAPI hk_GetFileAttributesExW(LPCWSTR p, GET_FILEEX_INFO_LEVELS l, LPVOID i)
{
    BOOL r = GetFileAttributesExW(is_virtual(p) ? g_pak : p, l, i);
    if (g_cfg.diag && interesting(p)) LOG("fs: GetFileAttributesExW '%ls' -> %d", p, r);
    return r;
}

static HANDLE WINAPI hk_FindFirstFileExW(LPCWSTR p, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
{
    HANDLE h = FindFirstFileExW(p, l, d, o, f, fl);
    if (g_cfg.diag && interesting(p)) LOG("fs: FindFirstFileExW '%ls' -> %s", p, h == INVALID_HANDLE_VALUE ? "none" : "found");
    return h;
}

void loc_install(void)
{
    if (!g_cfg.labels && !g_cfg.diag) return;
    if (g_cfg.labels && g_nlab) g_on = write_pak();   /* g_pak — куда удалось */
    HMODULE exe = GetModuleHandleW(NULL);
    static const struct { const char *n; void *h; } K[] = {
        {"FindFirstFileW", hk_FindFirstFileW}, {"FindNextFileW", hk_FindNextFileW}, {"FindClose", hk_FindClose},
        {"CreateFileW", hk_CreateFileW}, {"GetFileAttributesW", hk_GetFileAttributesW},
        {"GetFileAttributesExW", hk_GetFileAttributesExW}, {"FindFirstFileExW", hk_FindFirstFileExW},
    };
    int ok = 0, n = (int)(sizeof K / sizeof *K);
    for (int i = 0; i < n; i++) ok += iat_patch(exe, "kernel32.dll", K[i].n, K[i].h, NULL);
    LOG("IAT: kernel32 file %d/%d hooked (labels %s)", ok, n, g_on ? "on" : "off");
}
