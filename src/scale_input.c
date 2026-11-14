/* Ввод/окна: игра видит все экранные координаты User32 делёнными на S.
 * IAT-хуки только в call_to_arms.exe + обёртка оконной процедуры. */
#include "common.h"
#include <windowsx.h>

#define S (g_scale)

/* ---------------- IAT ---------------- */
static int iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn)) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            if (orig) *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* ---------------- оконные процедуры ---------------- */
typedef struct { HWND h; WNDPROC orig; } HwndProc;
typedef struct { ATOM a; WNDPROC orig; } ClassProc;
static HwndProc g_hp[64];
static ClassProc g_cp[32];
static SRWLOCK g_wlock = SRWLOCK_INIT;

static LRESULT CALLBACK wrap_proc(HWND, UINT, WPARAM, LPARAM);

static WNDPROC orig_for(HWND h)
{
    WNDPROC r = NULL;
    AcquireSRWLockShared(&g_wlock);
    for (int i = 0; i < 64 && !r; i++)
        if (g_hp[i].h == h) r = g_hp[i].orig;
    if (!r) {
        ATOM a = (ATOM)GetClassWord(h, GCW_ATOM);
        for (int i = 0; i < 32 && !r; i++)
            if (g_cp[i].a && g_cp[i].a == a) r = g_cp[i].orig;
    }
    ReleaseSRWLockShared(&g_wlock);
    return r ? r : DefWindowProcW;
}

static void set_hwnd_orig(HWND h, WNDPROC p)
{
    AcquireSRWLockExclusive(&g_wlock);
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (g_hp[i].h == h) { slot = i; break; }
        if (slot < 0 && (!g_hp[i].h || !IsWindow(g_hp[i].h))) slot = i;
    }
    if (slot >= 0) { g_hp[slot].h = h; g_hp[slot].orig = p; }
    ReleaseSRWLockExclusive(&g_wlock);
}

static int pt_msg(UINT m)
{
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_NCHITTEST ||
           (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK) || m == WM_MOVE;
}

static LPARAM down_lparam(UINT m, LPARAM l)
{
    if (pt_msg(m))
        return MAKELPARAM((short)(GET_X_LPARAM(l) / S), (short)(GET_Y_LPARAM(l) / S));
    if (m == WM_CONTEXTMENU && l != -1)
        return MAKELPARAM((short)(GET_X_LPARAM(l) / S), (short)(GET_Y_LPARAM(l) / S));
    if (m == WM_SIZE || m == WM_DISPLAYCHANGE)
        return MAKELPARAM(LOWORD(l) / S, HIWORD(l) / S);
    return l;
}

/* текущее транслируемое сообщение — чтобы DefWindowProcW получил исходный lParam */
typedef struct { HWND h; UINT m; WPARAM w; LPARAM nl, l; } CurMsg;
static __thread CurMsg t_cur;

static LRESULT CALLBACK wrap_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = orig_for(h);
    LPARAM nl = down_lparam(m, l);
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
    if (!h || !g_enabled) return;
    WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur == wrap_proc) return;
    set_hwnd_orig(h, cur);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)wrap_proc);
    LOG("window %p subclassed (orig %p)", h, cur);
}

static ATOM WINAPI hk_RegisterClassExW(const WNDCLASSEXW *wc)
{
    WNDCLASSEXW c = *wc;
    c.lpfnWndProc = wrap_proc;
    ATOM a = RegisterClassExW(&c);
    if (a) {
        AcquireSRWLockExclusive(&g_wlock);
        for (int i = 0; i < 32; i++)
            if (!g_cp[i].a) { g_cp[i].a = a; g_cp[i].orig = wc->lpfnWndProc; break; }
        ReleaseSRWLockExclusive(&g_wlock);
        LOG("RegisterClassExW '%ls' atom=%u wrapped", HIWORD((ULONG_PTR)wc->lpszClassName) ? wc->lpszClassName : L"#", a);
    }
    return a;
}

static LONG_PTR WINAPI hk_SetWindowLongPtrW(HWND h, int i, LONG_PTR v)
{
    if (i == GWLP_WNDPROC && (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) == wrap_proc) {
        WNDPROC prev = orig_for(h);
        set_hwnd_orig(h, (WNDPROC)v);
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
static void rect_down(RECT *r) { r->left /= S; r->top /= S; r->right /= S; r->bottom /= S; }
static void rect_up(RECT *r) { r->left *= S; r->top *= S; r->right *= S; r->bottom *= S; }

static BOOL WINAPI hk_GetCursorPos(POINT *p)
{
    BOOL r = GetCursorPos(p);
    if (r && p) { p->x /= S; p->y /= S; }
    return r;
}
static BOOL WINAPI hk_SetCursorPos(int x, int y) { return SetCursorPos(x * S, y * S); }
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
    p->x *= S; p->y *= S;
    BOOL r = ScreenToClient(h, p);
    p->x /= S; p->y /= S;
    return r;
}
static BOOL WINAPI hk_ClientToScreen(HWND h, POINT *p)
{
    if (!p) return ClientToScreen(h, p);
    p->x *= S; p->y *= S;
    BOOL r = ClientToScreen(h, p);
    p->x /= S; p->y /= S;
    return r;
}
static int WINAPI hk_MapWindowPoints(HWND from, HWND to, POINT *p, UINT n)
{
    for (UINT i = 0; i < n; i++) { p[i].x *= S; p[i].y *= S; }
    int r = MapWindowPoints(from, to, p, n);
    for (UINT i = 0; i < n; i++) { p[i].x /= S; p[i].y /= S; }
    return MAKELONG((short)LOWORD(r) / S, (short)HIWORD(r) / S);
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
        return v / S;
    }
    return v;
}
static HWND WINAPI hk_WindowFromPoint(POINT p)
{
    p.x *= S; p.y *= S;
    return WindowFromPoint(p);
}
static BOOL WINAPI hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    LOG("SetWindowPos(%p, %d,%d %dx%d, 0x%x)", h, x, y, cx, cy, f);
    return SetWindowPos(h, after, x * S, y * S, cx * S, cy * S, f);
}
static HWND WINAPI hk_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st, int x, int y, int w, int h,
                                      HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    LOG("CreateWindowExW(%d,%d %dx%d style=0x%lx)", x, y, w, h, st);
    /* все окна ×S, включая GDI-заставку: её рисование увеличивает мировое преобразование (gdi_up) */
    if (x != CW_USEDEFAULT) { x *= S; y *= S; }
    if (w != CW_USEDEFAULT) { w *= S; h *= S; }
    return CreateWindowExW(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
}

/* ---------------- GDI-заставка ----------------
 * Заставка (класс *SPLASH*) рисует себя GDI (BitBlt/StretchBlt/FillRect/TextOutW) в размере из GetClientRect,
 * который игра видит делённым на S. Окно создано ×S, а на его DC ставится мировое преобразование ×S —
 * всё нарисованное игрой занимает окно целиком, как на 1080p. */
static int is_splash(HWND h)
{
    WCHAR c[64];
    return h && GetClassNameW(h, c, 64) && wcsstr(c, L"SPLASH");
}

static void gdi_up(HDC dc)
{
    if (!dc) return;
    XFORM x = {(FLOAT)S, 0, 0, (FLOAT)S, 0, 0};
    SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &x);
}

static HDC WINAPI hk_BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    HDC dc = BeginPaint(h, ps);
    if (dc && is_splash(h)) {
        gdi_up(dc);
        rect_down(&ps->rcPaint);
        ps->rcPaint.right += 1; ps->rcPaint.bottom += 1;  /* нечётный край не теряем */
    }
    return dc;
}

static BOOL WINAPI hk_EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    if (is_splash(h)) {
        HDC dc = ps->hdc;
        ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
        SetGraphicsMode(dc, GM_COMPATIBLE);
    }
    return EndPaint(h, ps);
}

static HDC WINAPI hk_GetDC(HWND h)
{
    HDC dc = GetDC(h);
    if (dc && is_splash(h)) gdi_up(dc);
    return dc;
}

static int WINAPI hk_ReleaseDC(HWND h, HDC dc)
{
    if (dc && is_splash(h)) {
        ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
        SetGraphicsMode(dc, GM_COMPATIBLE);
    }
    return ReleaseDC(h, dc);
}

static BOOL WINAPI hk_InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    if (!r || !is_splash(h)) return InvalidateRect(h, r, erase);
    RECT c = *r;
    rect_up(&c);
    return InvalidateRect(h, &c, erase);
}
static BOOL WINAPI hk_AdjustWindowRect(RECT *r, DWORD st, BOOL menu)
{
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
    RECT c = *r;
    rect_up(&c);
    return MonitorFromRect(&c, f);
}
static BOOL WINAPI hk_EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW *dm)
{
    BOOL b = EnumDisplaySettingsW(dev, mode, dm);
    if (b && dm && mode == ENUM_CURRENT_SETTINGS) {
        LOG("EnumDisplaySettingsW(current) %lux%lu -> /%d", dm->dmPelsWidth, dm->dmPelsHeight, S);
        dm->dmPelsWidth /= S; dm->dmPelsHeight /= S;
    }
    return b;
}

/* ---------------- курсор ----------------
 * Игра делает аппаратный курсор из картинки для 1080p (CreateIconIndirect / LoadImageW). На экране ×S он
 * вдвое меньше остального UI — увеличиваем картинку ×S (ближайший сосед: пиксели курсора чёткие) и горячую точку. */
static HBITMAP bmp_up(HBITMAP src)
{
    BITMAP bm;
    if (!src || !GetObjectW(src, sizeof bm, &bm)) return NULL;
    int w = bm.bmWidth, h = bm.bmHeight, W = w * S, H = h * S;
    HDC dc = GetDC(NULL);
    HBITMAP out = NULL;
    if (bm.bmBitsPixel == 1) {  /* маска */
        out = CreateBitmap(W, H, 1, 1, NULL);
        HDC a = CreateCompatibleDC(dc), b = CreateCompatibleDC(dc);
        HGDIOBJ oa = SelectObject(a, src), ob = SelectObject(b, out);
        SetStretchBltMode(b, COLORONCOLOR);
        StretchBlt(b, 0, 0, W, H, a, 0, 0, w, h, SRCCOPY);
        SelectObject(a, oa); SelectObject(b, ob);
        DeleteDC(a); DeleteDC(b);
    } else {  /* цвет с альфой: читаем как 32 бита сверху вниз и повторяем пиксели */
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
        if (px && GetDIBits(dc, src, 0, h, px, &bi, DIB_RGB_COLORS) == h) {
            bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
            DWORD *dst = NULL;
            out = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
            if (out && dst)
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) dst[(SIZE_T)y * W + x] = px[(SIZE_T)(y / S) * w + x / S];
        }
        if (px) HeapFree(GetProcessHeap(), 0, px);
    }
    ReleaseDC(NULL, dc);
    return out;
}

static HICON WINAPI hk_CreateIconIndirect(ICONINFO *ii)
{
    if (!ii) return CreateIconIndirect(ii);
    ICONINFO c = *ii;
    c.hbmMask = bmp_up(ii->hbmMask);
    c.hbmColor = ii->hbmColor ? bmp_up(ii->hbmColor) : NULL;
    HICON r;
    if (c.hbmMask && (c.hbmColor || !ii->hbmColor)) {
        c.xHotspot = ii->xHotspot * S; c.yHotspot = ii->yHotspot * S;
        r = CreateIconIndirect(&c);
    } else {
        r = CreateIconIndirect(ii);
    }
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 40) {
        BITMAP bm = {0};
        GetObjectW(ii->hbmColor ? ii->hbmColor : ii->hbmMask, sizeof bm, &bm);
        LOG("CreateIconIndirect(icon=%d %ldx%ld bpp=%d hot=%lu,%lu) -> x%d %p", ii->fIcon, bm.bmWidth, bm.bmHeight,
            bm.bmBitsPixel, ii->xHotspot, ii->yHotspot, S, r);
    }
    if (c.hbmMask) DeleteObject(c.hbmMask);
    if (c.hbmColor) DeleteObject(c.hbmColor);
    return r;
}

static HCURSOR WINAPI hk_SetCursor(HCURSOR c)
{
    static HCURSOR seen[16];
    static LONG n;
    int known = 0;
    for (int i = 0; i < 16; i++) if (seen[i] == c) known = 1;
    if (!known && n < 16) {
        seen[InterlockedIncrement(&n) - 1] = c;
        ICONINFO ii;
        BITMAP bm = {0};
        if (c && GetIconInfo(c, &ii)) {
            GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
        }
        LOG("SetCursor(%p) %ldx%ld", c, bm.bmWidth, bm.bmHeight);
    }
    return SetCursor(c);
}

static HANDLE WINAPI hk_LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT f)
{
    if (type == IMAGE_CURSOR) {
        if (!cx && !cy && !(f & LR_DEFAULTSIZE)) {  /* «размер из файла» — узнаём его и берём ×S */
            HANDLE t = LoadImageW(inst, name, type, 0, 0, f);
            ICONINFO ii;
            if (t && GetIconInfo((HICON)t, &ii)) {
                BITMAP bm;
                if (GetObjectW(ii.hbmMask, sizeof bm, &bm)) {
                    cx = bm.bmWidth;
                    cy = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
                }
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
            if (t && !(f & LR_SHARED)) DestroyCursor((HCURSOR)t);
        }
        if (cx || cy) { cx *= S; cy *= S; f &= ~LR_DEFAULTSIZE; }
        LOG("LoadImageW(cursor %ls) -> %dx%d", HIWORD((ULONG_PTR)name) ? name : L"#", cx, cy);
    }
    return LoadImageW(inst, name, type, cx, cy, f);
}

/* ---------------- dxgi ---------------- */
typedef HRESULT(WINAPI *PFN_CF)(REFIID, void **);
static PFN_CF real_CreateDXGIFactory;
static HRESULT WINAPI hk_CreateDXGIFactory(REFIID riid, void **out)
{
    HRESULT hr = real_CreateDXGIFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        IDXGIFactory *f = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_IDXGIFactory, (void **)&f)) && f) {
            d3d_hook_factory(f);
            IDXGIFactory_Release(f);
        }
    }
    LOG("CreateDXGIFactory -> 0x%08lx", hr);
    return hr;
}

void input_install_iat(void)
{
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
        {"BeginPaint", hk_BeginPaint}, {"EndPaint", hk_EndPaint}, {"GetDC", hk_GetDC},
        {"ReleaseDC", hk_ReleaseDC}, {"InvalidateRect", hk_InvalidateRect},
        {"CreateIconIndirect", hk_CreateIconIndirect}, {"LoadImageW", hk_LoadImageW}, {"SetCursor", hk_SetCursor},
    };
    int ok = 0, n = (int)(sizeof U / sizeof U[0]);
    if (g_scale > 1)
        for (int i = 0; i < n; i++) {
            int r = iat_patch(exe, "user32.dll", U[i].n, U[i].h, NULL);
            ok += r;
            if (!r) LOG("IAT: user32!%s not found", U[i].n);
        }
    int f = iat_patch(exe, "dxgi.dll", "CreateDXGIFactory", hk_CreateDXGIFactory, (void **)&real_CreateDXGIFactory);
    LOG("IAT hooks: user32 %d/%d, dxgi %d", ok, g_scale > 1 ? n : 0, f);
}
