/* F10: ввод и окна. Игра видит экранные координаты User32 в логических единицах.
 * Перехват — только импорт call_to_arms.exe (IAT) и обёртка оконной процедуры.
 * Правила (дробный масштаб, те же функции, что у рендера):
 *   точка физ -> лог: to_log (физический пиксель попадает в тот логический, в котором нарисован);
 *   размер и край прямоугольника физ -> лог: size_to_log (край экрана 3840 при ×1.5 -> ровно 2560);
 *   лог -> физ: to_phys.
 * Курсор-картинка и GDI-заставка — F11 (здесь не масштабируются). */
#include "common.h"
#include <windowsx.h>

#define K (g_cfg.scale)
static LONG pt_down(LONG v) { return (LONG)to_log(K, v); }
static LONG len_down(LONG v) { return (LONG)size_to_log(K, v); }
static LONG up(LONG v) { return (LONG)to_phys(K, v); }
static void rect_down(RECT *r) { r->left = len_down(r->left); r->top = len_down(r->top); r->right = len_down(r->right); r->bottom = len_down(r->bottom); }
static void rect_up(RECT *r) { rect_to_phys(K, &r->left, &r->top, &r->right, &r->bottom); }

/* ---------------- оконные процедуры ----------------
 * Оригинальная процедура окна — в свойстве окна (v1: таблица на 64 окна); классов, зарегистрированных через
 * RegisterClassExW, мало — таблица. */
static const WCHAR PROP[] = L"ui4k.wndproc";
typedef struct { ATOM a; WNDPROC orig; } ClassProc;
static ClassProc g_cp[64];
static SRWLOCK g_lock = SRWLOCK_INIT;

static LRESULT CALLBACK wrap_proc(HWND, UINT, WPARAM, LPARAM);

static WNDPROC orig_for(HWND h)
{
    WNDPROC r = (WNDPROC)GetPropW(h, PROP);
    if (r) return r;
    ATOM a = (ATOM)GetClassWord(h, GCW_ATOM);
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < 64 && !r; i++)
        if (g_cp[i].a && g_cp[i].a == a) r = g_cp[i].orig;
    ReleaseSRWLockShared(&g_lock);
    return r ? r : DefWindowProcW;
}

static int pt_msg(UINT m)
{
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_NCHITTEST || (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) ||
           m == WM_MOVE || (m == WM_CONTEXTMENU);
}

static LPARAM down_lparam(UINT m, LPARAM l)
{
    if (m == WM_CONTEXTMENU && l == -1) return l;
    if (pt_msg(m)) return MAKELPARAM((short)pt_down(GET_X_LPARAM(l)), (short)pt_down(GET_Y_LPARAM(l)));
    if (m == WM_SIZE || m == WM_DISPLAYCHANGE) return MAKELPARAM(len_down(LOWORD(l)), len_down(HIWORD(l)));
    return l;
}

/* текущее пересчитанное сообщение: DefWindowProcW получает исходный lParam (система работает в физике) */
typedef struct { HWND h; UINT m; WPARAM w; LPARAM nl, l; } CurMsg;
static __thread CurMsg t_cur;

static LRESULT CALLBACK wrap_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = orig_for(h);
    if (m == WM_NCDESTROY) {
        LRESULT r = CallWindowProcW(orig, h, m, w, l);
        RemovePropW(h, PROP);
        return r;
    }
    if (m == WM_SETCURSOR || m == WM_MOUSEMOVE || m == WM_SIZE) cursor_refresh();   /* L8: курсор после смены строки */
    if (m == WM_WINDOWPOSCHANGED || m == WM_DISPLAYCHANGE || m == WM_EXITSIZEMOVE) mon_track_msg(h);   /* второй монитор */
    LPARAM nl = down_lparam(m, l);
    if (g_cfg.diag && (m == WM_SIZE || m == WM_DISPLAYCHANGE || m == WM_ACTIVATEAPP || m == WM_EXITSIZEMOVE))  /* L8: разведка */
        LOG("wndmsg %s w=%llu l=%dx%d -> game %dx%d", m == WM_SIZE ? "WM_SIZE" : m == WM_DISPLAYCHANGE ? "WM_DISPLAYCHANGE"
            : m == WM_ACTIVATEAPP ? "WM_ACTIVATEAPP" : "WM_EXITSIZEMOVE", (unsigned long long)w, LOWORD(l), HIWORD(l),
            LOWORD(nl), HIWORD(nl));
    if (nl == l) return CallWindowProcW(orig, h, m, w, l);
    CurMsg save = t_cur;
    t_cur.h = h; t_cur.m = m; t_cur.w = w; t_cur.nl = nl; t_cur.l = l;
    LRESULT r = CallWindowProcW(orig, h, m, w, nl);
    t_cur = save;
    return r;
}

static LRESULT WINAPI hk_DefWindowProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (t_cur.h == h && t_cur.m == m && t_cur.w == w && t_cur.nl == l) l = t_cur.l;
    return DefWindowProcW(h, m, w, l);
}

void input_watch_window(HWND h)
{
    if (!h || !g_cfg.enabled || (ratio_is_one(K) && !presets_on())) return;
    WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur == wrap_proc) return;
    SetPropW(h, PROP, (HANDLE)cur);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)wrap_proc);
    LOG("window %p subclassed (orig %p)", h, cur);
}

static ATOM WINAPI hk_RegisterClassExW(const WNDCLASSEXW *wc)
{
    if (!wc) return RegisterClassExW(wc);
    WNDCLASSEXW c = *wc;
    c.lpfnWndProc = wrap_proc;
    ATOM a = RegisterClassExW(&c);
    if (a) {
        int ok = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < 64; i++)
            if (!g_cp[i].a) { g_cp[i].a = a; g_cp[i].orig = wc->lpfnWndProc; ok = 1; break; }
        ReleaseSRWLockExclusive(&g_lock);
        if (!ok) LOG("RegisterClassExW: class table full");
        LOG("RegisterClassExW '%ls' atom=%u wrapped", HIWORD((ULONG_PTR)wc->lpszClassName) ? wc->lpszClassName : L"#", a);
    }
    return a;
}

/* игра меняет процедуру окна, обёрнутого нами: обёртка остаётся, новая процедура становится «оригиналом» */
static LONG_PTR WINAPI hk_SetWindowLongPtrW(HWND h, int i, LONG_PTR v)
{
    if (i == GWLP_WNDPROC && (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) == wrap_proc) {
        WNDPROC prev = orig_for(h);
        SetPropW(h, PROP, (HANDLE)v);
        return (LONG_PTR)prev;
    }
    return SetWindowLongPtrW(h, i, v);
}

static LONG_PTR WINAPI hk_GetWindowLongPtrW(HWND h, int i)
{
    LONG_PTR r = GetWindowLongPtrW(h, i);
    if (i == GWLP_WNDPROC && (WNDPROC)r == wrap_proc) return (LONG_PTR)orig_for(h);
    return r;
}

/* ---------------- координаты ---------------- */
static BOOL WINAPI hk_GetCursorPos(POINT *p)
{
    BOOL r = GetCursorPos(p);
    if (r && p) { p->x = pt_down(p->x); p->y = pt_down(p->y); }
    return r;
}
static BOOL WINAPI hk_SetCursorPos(int x, int y) { return SetCursorPos(up(x), up(y)); }
static BOOL WINAPI hk_ClipCursor(const RECT *r)
{
    if (!r) return ClipCursor(NULL);
    RECT c = *r;
    rect_up(&c);
    return ClipCursor(&c);
}
static BOOL WINAPI hk_ScreenToClient(HWND h, POINT *p)
{
    if (!p) return ScreenToClient(h, p);
    p->x = up(p->x); p->y = up(p->y);
    BOOL r = ScreenToClient(h, p);
    p->x = pt_down(p->x); p->y = pt_down(p->y);
    return r;
}
static BOOL WINAPI hk_ClientToScreen(HWND h, POINT *p)
{
    if (!p) return ClientToScreen(h, p);
    p->x = up(p->x); p->y = up(p->y);
    BOOL r = ClientToScreen(h, p);
    p->x = pt_down(p->x); p->y = pt_down(p->y);
    return r;
}
static int WINAPI hk_MapWindowPoints(HWND from, HWND to, POINT *p, UINT n)
{
    for (UINT i = 0; i < n && p; i++) { p[i].x = up(p[i].x); p[i].y = up(p[i].y); }
    int r = MapWindowPoints(from, to, p, n);
    for (UINT i = 0; i < n && p; i++) { p[i].x = pt_down(p[i].x); p[i].y = pt_down(p[i].y); }
    return MAKELONG((short)len_down((short)LOWORD(r)), (short)len_down((short)HIWORD(r)));
}
static BOOL WINAPI hk_GetClientRect(HWND h, RECT *r)
{
    BOOL b = GetClientRect(h, r);
    if (b) rect_down(r);
    return b;
}
static BOOL WINAPI hk_GetWindowRect(HWND h, RECT *r)
{
    BOOL b = GetWindowRect(h, r);
    if (b) rect_down(r);
    return b;
}
static int WINAPI hk_GetSystemMetrics(int i)
{
    int v = GetSystemMetrics(i);
    switch (i) {
    case SM_CXSCREEN: case SM_CYSCREEN: case SM_CXFULLSCREEN: case SM_CYFULLSCREEN:
    case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN: case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN:
    case SM_CXMAXIMIZED: case SM_CYMAXIMIZED: case SM_CXMAXTRACK: case SM_CYMAXTRACK:
        return len_down(v);
    }
    return v;
}
static HWND WINAPI hk_WindowFromPoint(POINT p)
{
    p.x = up(p.x); p.y = up(p.y);
    return WindowFromPoint(p);
}
static BOOL WINAPI hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    LOG("SetWindowPos(%p, %d,%d %dx%d, 0x%x)", h, x, y, cx, cy, f);
    return SetWindowPos(h, after, up(x), up(y), up(cx), up(cy), f);
}
static HWND WINAPI hk_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    LOG("CreateWindowExW(%d,%d %dx%d style=0x%lx)", x, y, w, h, st);
    if (x != CW_USEDEFAULT) { x = up(x); y = up(y); }
    if (w != CW_USEDEFAULT) { w = up(w); h = up(h); }
    return CreateWindowExW(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
}
static BOOL WINAPI hk_AdjustWindowRect(RECT *r, DWORD st, BOOL menu)
{
    if (!r) return AdjustWindowRect(r, st, menu);
    rect_up(r);
    BOOL b = AdjustWindowRect(r, st, menu);
    rect_down(r);
    return b;
}
static BOOL WINAPI hk_GetMonitorInfoW(HMONITOR m, MONITORINFO *mi)
{
    BOOL b = GetMonitorInfoW(m, mi);
    if (b && mi) { rect_down(&mi->rcMonitor); rect_down(&mi->rcWork); }
    return b;
}
static HMONITOR WINAPI hk_MonitorFromRect(const RECT *r, DWORD f)
{
    if (!r) return MonitorFromRect(r, f);
    RECT c = *r;
    rect_up(&c);
    return MonitorFromRect(&c, f);
}
static BOOL WINAPI hk_EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW *dm)
{
    if (dm && g_cfg.fit_native && mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS)
        return modes_enum(dev, mode, dm);   /* F03: список режимов — логический */
    BOOL b = EnumDisplaySettingsW(dev, mode, dm);
    if (b && dm && (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)) {
        LOG("EnumDisplaySettingsW(%ls, %s) %lux%lu -> logical", dev ? dev : L"main display",
            mode == ENUM_CURRENT_SETTINGS ? "current" : "registry", dm->dmPelsWidth, dm->dmPelsHeight);
        dm->dmPelsWidth = (DWORD)len_down((LONG)dm->dmPelsWidth);
        dm->dmPelsHeight = (DWORD)len_down((LONG)dm->dmPelsHeight);
    }
    return b;
}

/* Ревизия v3, п. 2: клавиши мода — Ctrl+F6/F7/F9/F10 (сравнение «с правкой / без»). У игры F6–F11 — бойцы отряда, F6/F7 —
 * «атака по земле» / «рукопашная» (keys.set): с Ctrl в бою они переключали бы мод незаметно, а GetAsyncKeyState видит
 * клавиатуру и тогда, когда впереди другая программа. Поэтому — только с keys=1 (диагностика diag=1 — сама по себе
 * включённая разведка: force) и только когда впереди окно этого процесса. */
int ui4k_hotkey(int vk, int force)
{
    if (!force && !g_cfg.keys) return 0;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return 0;
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(vk) & 0x8000);
}

void input_install(void)
{
    if (ratio_is_one(K) && !presets_on()) return;
    HMODULE exe = GetModuleHandleW(NULL);
    static const struct { const char *n; void *h; } U[] = {
        {"GetCursorPos", hk_GetCursorPos}, {"SetCursorPos", hk_SetCursorPos}, {"ClipCursor", hk_ClipCursor},
        {"ScreenToClient", hk_ScreenToClient}, {"ClientToScreen", hk_ClientToScreen},
        {"MapWindowPoints", hk_MapWindowPoints}, {"GetClientRect", hk_GetClientRect},
        {"GetWindowRect", hk_GetWindowRect}, {"GetSystemMetrics", hk_GetSystemMetrics},
        {"WindowFromPoint", hk_WindowFromPoint}, {"SetWindowPos", hk_SetWindowPos},
        {"CreateWindowExW", hk_CreateWindowExW}, {"AdjustWindowRect", hk_AdjustWindowRect},
        {"GetMonitorInfoW", hk_GetMonitorInfoW}, {"MonitorFromRect", hk_MonitorFromRect},
        {"EnumDisplaySettingsW", hk_EnumDisplaySettingsW}, {"RegisterClassExW", hk_RegisterClassExW},
        {"SetWindowLongPtrW", hk_SetWindowLongPtrW}, {"GetWindowLongPtrW", hk_GetWindowLongPtrW},
        {"DefWindowProcW", hk_DefWindowProcW},
    };
    int ok = 0, n = (int)(sizeof U / sizeof U[0]);
    for (int i = 0; i < n; i++) {
        int r = iat_patch(exe, "user32.dll", U[i].n, U[i].h, NULL);
        ok += r;
        if (!r) LOGV("IAT: user32!%s not imported", U[i].n);
    }
    LOG("IAT: user32 %d/%d hooked", ok, n);
}
