/* F01: ui4k.ini рядом с DLL. */
#include <string.h>
#include <stdio.h>
#include <shlobj.h>
#include "common.h"
#include "profile.h"

Config g_cfg = {1, {2, 1}, 1, {156, 84}, 2, 0, 1, 1, 0, 0, 100, 0, {2, 1}, 1, 1, 1, {0}, 0, -1, 0, 0, 0, 0, -1, 0, 1, -1, 2, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, {{0}}, 0, 0};   /* ..., origin, cb_names, thick_lines, fonts, ui_refresh, keys */
WCHAR g_dir[MAX_PATH];

/* ревизия кода 4, п. 4: <папка DLL> + имя. Не помещается в MAX_PATH — пустая строка и 0 (файл просто не открывается) */
int dir_file(WCHAR *out, const WCHAR *name)
{
    if (lstrlenW(g_dir) + lstrlenW(name) >= MAX_PATH) { out[0] = 0; return 0; }
    lstrcpyW(out, g_dir);
    lstrcatW(out, name);
    return 1;
}

static int ini_int(const WCHAR *ini, const WCHAR *key, int def)
{
    return (int)GetPrivateProfileIntW(L"ui4k", key, def, ini);
}

/* Профиль игры: самый свежий Documents\my games\gates of hell\profiles\*\options.set. Только чтение.
 *  hdr          — мир без HDR игра рисует прямо в экран: экранные картинки надо делать в размере мира с самого начала;
 *  antialiasing — MSAA: с ним мод в игре ещё не проверен, включаем безопасный режим (В0). */
WCHAR g_next_d3d11[MAX_PATH];      /* next_d3d11=: полный путь d3d11.dll, которую грузить вместо системной; пусто — системная */
int g_next_auto;                   /* 1 — g_next_d3d11 не из настройки, а найден рядом (d3d11_dxvk.dll из дополнения) */
WCHAR g_appdata[MAX_PATH];       /* папка пользователя для пакета подписей; пусто — %LOCALAPPDATA%\ui4k (тесты: test_appdata=) */
static WCHAR g_profiles[MAX_PATH];   /* папка профилей; пусто — Documents\my games\gates of hell\profiles (тесты: test_profiles=) */
static int g_ini_hdr = -2, g_ini_aa = -2;   /* заданы в ui4k.ini — профиль для них не смотрим */
static unsigned g_pmw, g_pmh;               /* {mode "WxH"} профиля — строка, выбранная в игре */

static void read_profile(int *hdr, int *aa)
{
    *hdr = -1; *aa = -1;
    WCHAR docs[MAX_PATH], pat[MAX_PATH], best[MAX_PATH] = L"";
    if (g_profiles[0]) lstrcpyW(docs, g_profiles);
    else {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs)) || lstrlenW(docs) > MAX_PATH - 40) return;
        lstrcatW(docs, L"\\my games\\gates of hell\\profiles");
    }
    if (lstrlenW(docs) > MAX_PATH - 3) return;   /* ревизия кода 4, п. 4: ниже docs + «\*» */
    wsprintfW(pat, L"%s\\*", docs);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    FILETIME bt = {0, 0};
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        WCHAR f[MAX_PATH];
        if (lstrlenW(docs) + lstrlenW(fd.cFileName) + 14 > MAX_PATH - 1) continue;   /* «\<папка>\options.set» не помещается */
        wsprintfW(f, L"%s\\%s\\options.set", docs, fd.cFileName);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(f, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &bt) > 0) {
            bt = a.ftLastWriteTime;
            lstrcpyW(best, f);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best[0]) return;
    HANDLE fh = CreateFileW(best, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    static char buf[65536];
    DWORD n = 0;
    ReadFile(fh, buf, sizeof buf - 1, &n, NULL);
    CloseHandle(fh);
    buf[n] = 0;
    *hdr = profile_hdr(buf);
    *aa = profile_aa(buf);
    unsigned w = 0, hh = 0;
    if (profile_mode(buf, &w, &hh)) { g_pmw = w; g_pmh = hh; }
}

/* строка разрешения из профиля игры (при запуске: какой монитор у игры — modes.c); 0 — не знаем */
int config_profile_mode(UINT *w, UINT *h)
{
    if (!g_pmw || !g_pmh) return 0;
    *w = g_pmw; *h = g_pmh;
    return 1;
}

/* HDR и MSAA игра меняет без перезапуска: «Применить» записывает options.set и сразу пересоздаёт экранные цели
 * (проверка в игре 17.11). Перечитываем профиль, когда игра создаёт экранную цель или меняет режим; не чаще раза
 * в 300 мс. 1 — что-то изменилось. */
int config_poll_profile(void)
{
    static volatile LONG busy;
    static ULONGLONG at;
    if (g_ini_hdr != -2 && g_ini_aa != -2) return 0;
    ULONGLONG now = GetTickCount64();
    if (now - at < 300 || InterlockedExchange(&busy, 1)) return 0;
    at = now;
    int ph, pa, ch = 0;
    read_profile(&ph, &pa);
    if (g_ini_hdr == -2 && ph >= 0 && ph != g_cfg.game_hdr) {
        LOG("game settings: HDR is now %s (was %s)", ph ? "on" : "off", g_cfg.game_hdr < 0 ? "unknown" : g_cfg.game_hdr ? "on" : "off");
        g_cfg.game_hdr = ph;
        world_hdr_changed();
        ch = 1;
    }
    if (g_ini_aa == -2 && pa >= 0) msaa_profile(pa);
    InterlockedExchange(&busy, 0);
    return ch;
}

void config_load(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    WCHAR *s = wcsrchr(g_dir, L'\\');
    if (s) s[1] = 0;
    WCHAR ini[MAX_PATH], logp[MAX_PATH];
    dir_file(ini, L"ui4k.ini");    /* путь не помещается — пусто: настройки по умолчанию, без лога */
    dir_file(logp, L"ui4k.log");

    g_cfg.enabled = ini_int(ini, L"enabled", 1);
    g_cfg.log = ini_int(ini, L"log", 1);
    if (g_cfg.log) log_open(logp);

    g_cfg.scan_cb = ini_int(ini, L"scan_cb", 0);
    g_cfg.stretch_copy = ini_int(ini, L"stretch_copy", 1);
    g_cfg.fit_native = ini_int(ini, L"fit_native", 1);
    g_cfg.diag = ini_int(ini, L"diag", 0);
    g_cfg.perf = ini_int(ini, L"perf", 0);               /* Э1: режим замеров (perf.c) */
    g_cfg.perf_every = ini_int(ini, L"perf_every", 0);
    g_cfg.origin = ini_int(ini, L"origin", 1);           /* Э2: кэши окон по происхождению */
    g_cfg.cb_names = ini_int(ini, L"cb_names", 1);       /* Э3: экранные числа буфера сцены по именам */
    g_cfg.thick_lines = ini_int(ini, L"thick_lines", 1); /* Э5: линии интерфейса под xk */
    g_cfg.fonts = ini_int(ini, L"fonts", 1);             /* Э6: чёткие страницы шрифтов */
    g_cfg.ui_refresh = ini_int(ini, L"ui_refresh", 1);   /* смена масштаба на ходу: кэши окон заново */
    g_cfg.keys = ini_int(ini, L"keys", 0);               /* ревизия v3, п. 2: клавиши-переключатели мода */
    if (g_cfg.keys) LOG("keys: Ctrl+F6 / F7 / F9 / F10 switch mod features (only while the game window is in front)");
    if (g_cfg.perf) LOG("perf: measurement mode on - a \"perf\" line every %s", g_cfg.perf_every > 0 ? "few frames (perf_every)" : "5 s");
    g_cfg.lum_fix = ini_int(ini, L"lum_fix", 1);
    g_cfg.labels = ini_int(ini, L"labels", 1);
    WCHAR nx[MAX_PATH];   /* DXVK и т. п.: следующая d3d11.dll в цепочке вместо системной (путь от папки DLL или полный) */
    GetPrivateProfileStringW(L"ui4k", L"next_d3d11", L"", nx, MAX_PATH, ini);
    g_next_d3d11[0] = 0;
    if (!lstrcmpiW(nx, L"system")) {
        /* next_d3d11=system — всегда системная, даже если рядом лежит d3d11_dxvk.dll */
    } else if (nx[0] && (nx[1] == L':' || (nx[0] == L'\\' && nx[1] == L'\\'))) {
        lstrcpyW(g_next_d3d11, nx);
    } else if (nx[0]) {
        dir_file(g_next_d3d11, nx);
    } else if (dir_file(g_next_d3d11, L"d3d11_dxvk.dll") && GetFileAttributesW(g_next_d3d11) == INVALID_FILE_ATTRIBUTES) {
        g_next_d3d11[0] = 0;   /* не задано и рядом нет DXVK из дополнения: системная */
    } else {
        g_next_auto = 1;       /* d3d11_dxvk.dll рядом, в настройке пусто: взят сам (под Wine — нет, см. proxy.c) */
    }
    GetPrivateProfileStringW(L"ui4k", L"test_profiles", L"", g_profiles, MAX_PATH, ini);
    GetPrivateProfileStringW(L"ui4k", L"test_appdata", L"", g_appdata, MAX_PATH, ini);
    GetPrivateProfileStringW(L"ui4k", L"test_shader_pak", L"", g_shaderpak, MAX_PATH, ini);
    GetPrivateProfileStringW(L"ui4k", L"test_shader_cache", L"", g_shadercache, MAX_PATH, ini);
    g_cfg.game_hdr = g_ini_hdr = ini_int(ini, L"game_hdr", -2);   /* -2: смотреть в профиль игры; 0/1 — задать (тесты) */
    int aa = g_ini_aa = ini_int(ini, L"game_aa", -2);             /* то же для MSAA: -2 — профиль; 0 / число выборок */
    if (g_cfg.game_hdr == -2 || aa == -2) {
        int ph, pa;
        read_profile(&ph, &pa);
        if (g_cfg.game_hdr == -2) g_cfg.game_hdr = ph;
        if (aa == -2) aa = pa;
    }
    g_cfg.game_aa = aa;
    if (g_ini_aa == -2) msaa_profile(aa);
    g_cfg.msaa_safe = ini_int(ini, L"msaa_safe", 0);
    g_cfg.dpi_aware = ini_int(ini, L"dpi_aware", 1);
    g_cfg.hbao_radius = ini_int(ini, L"hbao_radius", -1);   /* -1 — номер по устройству буфера (cb.c) */
    g_cfg.dof_radius = ini_int(ini, L"dof_radius", 2);
    g_cfg.box_step = ini_int(ini, L"box_step", 1);      /* Э8: шаг уменьшения иконок инвентаря в настоящих текселях */
    g_cfg.presets = ini_int(ini, L"presets", 1);
    g_cfg.all_monitors = ini_int(ini, L"all_monitors", 1);
    g_cfg.d3d_debug = ini_int(ini, L"d3d_debug", 0);
    {   /* тесты всем набором: build.py --debug-layer */
        WCHAR ev[8];
        if (GetEnvironmentVariableW(L"UI4K_D3D_DEBUG", ev, 8) && ev[0] == L'1') g_cfg.d3d_debug = 1;
    }
    {
        /* monitor=WxH — родное разрешение задано вручную (С5); test_monitor — то же, прежнее имя для тестов */
        static const WCHAR *const key[4] = {L"monitor", L"test_monitor", L"test_dsr", L"test_monitor2"};
        for (int i = 0; i < 4; i++) {
            WCHAR tm[32];
            GetPrivateProfileStringW(L"ui4k", key[i], L"", tm, 32, ini);
            WCHAR *e;
            long w = wcstol(tm, &e, 10);
            if (w <= 0 || (*e != L'x' && *e != L'X')) continue;
            long h = wcstol(e + 1, NULL, 10);
            if (h <= 0) continue;
            if (i < 2) { g_cfg.test_mw = (int)w; g_cfg.test_mh = (int)h; g_cfg.test_mon |= i == 1; }
            else if (i == 2) { g_cfg.test_dsr_w = (int)w; g_cfg.test_dsr_h = (int)h; }
            else { g_cfg.test_m2w = (int)w; g_cfg.test_m2h = (int)h; }
        }
    }
    {
        WCHAR rb[64];
        GetPrivateProfileStringW(L"ui4k", L"render", L"100", rb, 64, ini);
        WCHAR *e;
        long v = wcstol(rb, &e, 10);
        if (*e == L'x' || *e == L'X') { g_cfg.render_w = (int)v; g_cfg.render = 0; }   /* 2560x1440: высота по пропорции */
        else g_cfg.render = (int)v;
        if (g_cfg.render_w && (g_cfg.render_w < 320 || g_cfg.render_w > 16384)) { g_cfg.render_w = 0; g_cfg.render = 100; }
        if (!g_cfg.render_w && (g_cfg.render < 25 || g_cfg.render > 200)) g_cfg.render = 100;
    }
    g_cfg.diag_at = ini_int(ini, L"diag_at", 0);
    g_cfg.diag_ring = ini_int(ini, L"diag_ring", 0);   /* 20.11: «запись прошлого», МБ (diag.c) */
    if (g_cfg.diag_ring > 4096) g_cfg.diag_ring = 4096;
    if (g_cfg.diag_ring > 0) g_cfg.diag = 1;
    {   /* ui_vs=b7e24d535a3a5d52 — контрольная сумма DXBC (байты 4..11) ещё одного VS интерфейса */
        WCHAR hb[64];
        GetPrivateProfileStringW(L"ui4k", L"ui_vs", L"", hb, 64, ini);
        int ok = lstrlenW(hb) >= 16;
        for (int i = 0; ok && i < 8; i++) {
            int v = 0;
            for (int j = 0; j < 2; j++) {
                WCHAR ch = hb[i * 2 + j];
                int d = ch >= L'0' && ch <= L'9' ? ch - L'0' : ch >= L'a' && ch <= L'f' ? ch - L'a' + 10 : ch >= L'A' && ch <= L'F' ? ch - L'A' + 10 : -1;
                if (d < 0) ok = 0;
                v = v * 16 + d;
            }
            g_cfg.ui_vs[i] = (BYTE)v;
        }
        g_cfg.has_ui_vs = ok;
    }
    {   /* diag_on=WxH — разведка: снимок начинается с создания кэша этого размера (то, что игра рисует один раз) */
        WCHAR b[32];
        unsigned w = 0, h = 0;
        GetPrivateProfileStringW(L"ui4k", L"diag_on", L"", b, 32, ini);
        if (swscanf(b, L"%ux%u", &w, &h) == 2 && w && h) { g_cfg.diag_on_w = w; g_cfg.diag_on_h = h; g_cfg.diag = 1; }
    }
    {   /* diag_stack=105x120,1024x1024 — разведка: кто в игре создаёт текстуры таких размеров (стек вызовов в лог) */
        WCHAR b[256];
        GetPrivateProfileStringW(L"ui4k", L"diag_stack", L"", b, 256, ini);
        const WCHAR *p = b;
        g_cfg.ndiag_stk = 0;
        while (*p && g_cfg.ndiag_stk < 8) {
            unsigned w = 0, h = 0;
            if (swscanf(p, L"%ux%u", &w, &h) == 2 && w && h) {
                g_cfg.diag_stk[g_cfg.ndiag_stk][0] = w; g_cfg.diag_stk[g_cfg.ndiag_stk][1] = h; g_cfg.ndiag_stk++;
            }
            while (*p && *p != L',') p++;
            if (*p == L',') p++;
        }
        if (g_cfg.ndiag_stk) g_cfg.diag = 1;
    }
    if (g_cfg.diag_at) g_cfg.diag = 1;
    {
        WCHAR b[256], *p, *e;
        GetPrivateProfileStringW(L"ui4k", L"cb_fix", L"156,84", b, 256, ini);
        g_cfg.ncbfix = 0;
        for (p = b; *p && g_cfg.ncbfix < 16; p = e) {
            long v = wcstol(p, &e, 10);
            if (e == p) { e = p + 1; continue; }
            if (v > 0) g_cfg.cbfix[g_cfg.ncbfix++] = (int)v;
        }
    }

    char buf[64];
    {
        char inia[MAX_PATH * 3];
        WideCharToMultiByte(CP_ACP, 0, ini, -1, inia, sizeof inia, NULL, NULL);
        GetPrivateProfileStringA("ui4k", "scale", "2", buf, sizeof buf, inia);
    }
    Ratio r;
    if (ratio_parse(buf, &r)) {
        g_cfg.scale = r;
    } else {
        g_cfg.scale.num = 2; g_cfg.scale.den = 1;
        LOG("config: scale=\"%s\" is invalid (allowed 1..4, e.g. 1.5, 2, 3/2) -> using 2", buf);
    }
}
