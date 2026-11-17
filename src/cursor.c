/* F11: аппаратный курсор и GDI-заставка правильного размера.
 * Курсор: игра делает его из картинки для логического экрана (CreateIconIndirect / LoadImageW) — на физическом он
 * в k раз меньше остального UI. Увеличиваем картинку и горячую точку:
 *   целый k — повтор пикселей (чётко, как у v1 без сети); дробный — билинейно с учётом альфы
 *   (как игра сама растягивает UI). Сеть для курсора — L4, после основы.
 * Заставка (класс *SPLASH*) рисует себя GDI в размере из GetClientRect (логическом). Окно создано ×k (F10), на его DC
 * ставится мировое преобразование ×k — нарисованное занимает окно целиком. */
#include "common.h"

#define K (g_cfg.scale)

static int up(int v) { return (int)to_phys(K, v); }

/* 32 бита сверху вниз -> W x H */
static void resample32(const DWORD *s, int w, int h, DWORD *d, int W, int H, Ratio k)
{
    if (k.den == 1) {   /* целый — повтор пикселей */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) d[(SIZE_T)y * W + x] = s[(SIZE_T)(y / k.num) * w + x / k.num];
        return;
    }
    for (int y = 0; y < H; y++) {
        float fy = (y + 0.5f) * h / H - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
        float wy = fy - y0; if (wy < 0) wy = 0;
        for (int x = 0; x < W; x++) {
            float fx = (x + 0.5f) * w / W - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            float wx = fx - x0; if (wx < 0) wx = 0;
            DWORD p[4] = {s[y0 * w + x0], s[y0 * w + x1], s[y1 * w + x0], s[y1 * w + x1]};
            float wt[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
            float a = 0, c[3] = {0, 0, 0};
            for (int i = 0; i < 4; i++) {
                float pa = (float)(p[i] >> 24) / 255.0f;
                a += wt[i] * pa;
                for (int ch = 0; ch < 3; ch++) c[ch] += wt[i] * pa * (float)((p[i] >> (8 * ch)) & 255);
            }
            DWORD o = 0;
            int anyalpha = (p[0] | p[1] | p[2] | p[3]) >> 24;
            for (int ch = 0; ch < 3; ch++) {
                float v;
                if (anyalpha) v = a > 0 ? c[ch] / a : 0;
                else { v = 0; for (int i = 0; i < 4; i++) v += wt[i] * (float)((p[i] >> (8 * ch)) & 255); }
                o |= (DWORD)(v + 0.5f > 255 ? 255 : v + 0.5f) << (8 * ch);
            }
            o |= (DWORD)(a * 255.0f + 0.5f) << 24;
            d[(SIZE_T)y * W + x] = o;
        }
    }
}

HBITMAP cursor_bmp_scale(HBITMAP src, Ratio k)
{
    BITMAP bm;
    if (!src || !GetObjectW(src, sizeof bm, &bm)) return NULL;
    int w = bm.bmWidth, h = bm.bmHeight, W = (int)to_phys(k, w), H = (int)to_phys(k, h);
    HDC dc = GetDC(NULL);
    HBITMAP out = NULL;
    if (bm.bmBitsPixel == 1) {   /* маска: ближайший сосед */
        out = CreateBitmap(W, H, 1, 1, NULL);
        HDC a = CreateCompatibleDC(dc), b = CreateCompatibleDC(dc);
        HGDIOBJ oa = SelectObject(a, src), ob = SelectObject(b, out);
        SetStretchBltMode(b, COLORONCOLOR);
        StretchBlt(b, 0, 0, W, H, a, 0, 0, w, h, SRCCOPY);
        SelectObject(a, oa); SelectObject(b, ob);
        DeleteDC(a); DeleteDC(b);
    } else {
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        DWORD *px = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
        if (px && GetDIBits(dc, src, 0, h, px, &bi, DIB_RGB_COLORS) == h) {
            bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
            DWORD *dst = NULL;
            out = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
            if (out && dst) resample32(px, w, h, dst, W, H, k);
        }
        if (px) HeapFree(GetProcessHeap(), 0, px);
    }
    ReleaseDC(NULL, dc);
    return out;
}

static int upk(Ratio k, int v) { return (int)to_phys(k, v); }

/* картинка ii, увеличенная в k; NULL — не вышло */
static HICON make_icon(const ICONINFO *ii, Ratio k)
{
    ICONINFO c = *ii;
    c.hbmColor = NULL;
    /* монохромный курсор: маска AND+XOR одна над другой — высота строго 2 x увеличенной половины */
    if (!ii->hbmColor) {
        BITMAP bm;
        GetObjectW(ii->hbmMask, sizeof bm, &bm);
        HBITMAP t = cursor_bmp_scale(ii->hbmMask, k);
        BITMAP tb;
        int W = upk(k, bm.bmWidth), H2 = 2 * upk(k, bm.bmHeight / 2);
        if (t && GetObjectW(t, sizeof tb, &tb) && tb.bmHeight != H2) {
            /* нечётное округление: перерисовать в точный размер */
            HDC dc = GetDC(NULL), a = CreateCompatibleDC(dc), b = CreateCompatibleDC(dc);
            HBITMAP o = CreateBitmap(W, H2, 1, 1, NULL);
            HGDIOBJ oa = SelectObject(a, ii->hbmMask), ob = SelectObject(b, o);
            SetStretchBltMode(b, COLORONCOLOR);
            StretchBlt(b, 0, 0, W, H2, a, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(a, oa); SelectObject(b, ob);
            DeleteDC(a); DeleteDC(b); ReleaseDC(NULL, dc);
            DeleteObject(t);
            t = o;
        }
        c.hbmMask = t;
    } else {
        c.hbmMask = cursor_bmp_scale(ii->hbmMask, k);
        c.hbmColor = cursor_bmp_scale(ii->hbmColor, k);
    }
    HICON r = NULL;
    if (c.hbmMask && (c.hbmColor || !ii->hbmColor)) {
        c.xHotspot = (DWORD)upk(k, (int)ii->xHotspot);
        c.yHotspot = (DWORD)upk(k, (int)ii->yHotspot);
        r = CreateIconIndirect(&c);
    }
    if (c.hbmMask) DeleteObject(c.hbmMask);
    if (c.hbmColor) DeleteObject(c.hbmColor);
    return r;
}

/* L8: строка списка разрешений меняет масштаб на ходу, а игра в меню делает курсор один раз. Запоминаем исходную
 * картинку каждого курсора; SetCursor с курсором прежнего масштаба получает копию нынешнего. */
typedef struct { HICON h, alt; Ratio k, altk; ICONINFO src; } CurRec;
#define NCUR 64
static CurRec g_cur[NCUR];
static SRWLOCK g_curlk = SRWLOCK_INIT;
static HCURSOR g_shown;   /* что игра поставила последним (её handle) и в каком масштабе он сейчас на экране */
static Ratio g_shownk;

static void rec_free(CurRec *r)
{
    if (r->alt) DestroyIcon(r->alt);
    if (r->src.hbmMask) DeleteObject(r->src.hbmMask);
    if (r->src.hbmColor) DeleteObject(r->src.hbmColor);
    ZeroMemory(r, sizeof *r);
}

static void rec_add(HICON h, const ICONINFO *ii, Ratio k)
{
    ICONINFO s = *ii;
    s.hbmMask = (HBITMAP)CopyImage(ii->hbmMask, IMAGE_BITMAP, 0, 0, 0);
    s.hbmColor = ii->hbmColor ? (HBITMAP)CopyImage(ii->hbmColor, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION) : NULL;
    if (!s.hbmMask || (ii->hbmColor && !s.hbmColor)) {
        if (s.hbmMask) DeleteObject(s.hbmMask);
        if (s.hbmColor) DeleteObject(s.hbmColor);
        return;
    }
    AcquireSRWLockExclusive(&g_curlk);
    static int next;
    CurRec *r = NULL;
    for (int i = 0; i < NCUR && !r; i++) if (!g_cur[i].h) r = &g_cur[i];
    if (!r) { r = &g_cur[next]; next = (next + 1) % NCUR; rec_free(r); }   /* полная таблица: вытеснить старейший */
    r->h = h; r->k = k; r->src = s;
    ReleaseSRWLockExclusive(&g_curlk);
}

static BOOL WINAPI hk_DestroyIcon(HICON h)
{
    if (h && h == g_shown) g_shown = NULL;
    if (h) {
        AcquireSRWLockExclusive(&g_curlk);
        for (int i = 0; i < NCUR; i++)
            if (g_cur[i].h == h) { rec_free(&g_cur[i]); break; }
        ReleaseSRWLockExclusive(&g_curlk);
    }
    return DestroyIcon(h);
}

static HCURSOR set_cursor(HCURSOR h);
static HCURSOR WINAPI hk_SetCursor(HCURSOR h)
{
    g_shown = h;
    g_shownk = K;
    return set_cursor(h);
}

/* окно игры (input.c, поток окна): масштаб сменился, а игра курсор заново не ставит — поставить его копию самим */
void cursor_refresh(void)
{
    if (g_shown && !ratio_eq(g_shownk, K)) hk_SetCursor(g_shown);
}

static HCURSOR set_cursor(HCURSOR h)
{
    HCURSOR use = h;
    if (h) {
        Ratio k = K;
        AcquireSRWLockExclusive(&g_curlk);
        for (int i = 0; i < NCUR; i++) {
            CurRec *r = &g_cur[i];
            if (!r->h || (r->h != h && r->alt != h)) continue;
            if (ratio_eq(r->k, k)) { use = r->h; break; }
            if (!r->alt || !ratio_eq(r->altk, k)) {
                HICON n = make_icon(&r->src, k);
                if (!n) break;
                HICON old = r->alt;
                r->alt = n; r->altk = k;
                ReleaseSRWLockExclusive(&g_curlk);
                HCURSOR prev = SetCursor(n);
                if (old) DestroyIcon(old);   /* уже не на экране */
                static Ratio said;
                if (!ratio_eq(said, k)) { said = k; LOG("cursor: re-made at the current interface scale %d/%d", k.num, k.den); }
                return prev;
            }
            use = r->alt;
            break;
        }
        ReleaseSRWLockExclusive(&g_curlk);
    }
    return SetCursor(use);
}

static HICON WINAPI hk_CreateIconIndirect(ICONINFO *ii)
{
    if (!ii) return CreateIconIndirect(ii);
    Ratio k = K;
    HICON r = make_icon(ii, k);
    if (!r) r = CreateIconIndirect(ii);
    else if (presets_on() && !ii->fIcon) rec_add(r, ii, k);
    static volatile LONG logged;
    static Ratio lastk;   /* L8: после смены строки — первый курсор в новом масштабе тоже в лог */
    int newk = !ratio_eq(lastk, k);
    lastk = k;
    if (InterlockedIncrement(&logged) <= 20 || newk) {
        BITMAP bm = {0};
        GetObjectW(ii->hbmColor ? ii->hbmColor : ii->hbmMask, sizeof bm, &bm);
        LOG("CreateIconIndirect(icon=%d %ldx%ld bpp=%d hot=%lu,%lu) -> scaled %d/%d %p", ii->fIcon, bm.bmWidth,
            bm.bmHeight, bm.bmBitsPixel, ii->xHotspot, ii->yHotspot, k.num, k.den, r);
    }
    return r;
}

static HANDLE WINAPI hk_LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT f)
{
    if (type == IMAGE_CURSOR) {
        if (!cx && !cy && !(f & LR_DEFAULTSIZE)) {   /* «размер из файла»: узнаём его */
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
        if (cx || cy) {
            cx = up(cx); cy = up(cy);
            f &= ~(UINT)LR_DEFAULTSIZE;
            /* общий кэш мог бы отдать курсор прежнего размера — но системные курсоры (inst=NULL) без LR_SHARED
             * не грузятся вовсе; снимаем только для файлов и ресурсов игры */
            if ((f & LR_LOADFROMFILE) || inst) f &= ~(UINT)LR_SHARED;
        }
        LOG("LoadImageW(cursor %ls) -> %dx%d", HIWORD((ULONG_PTR)name) ? name : L"#", cx, cy);
        HANDLE r = LoadImageW(inst, name, type, cx, cy, f);
        if (r && (f & LR_SHARED) && (cx || cy)) {
            /* общий системный курсор мог прийти из кэша прежнего размера — копия нужного размера из ресурса */
            ICONINFO ii;
            BITMAP bm = {0};
            if (GetIconInfo((HICON)r, &ii)) {
                GetObjectW(ii.hbmMask, sizeof bm, &bm);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
            }
            if (bm.bmWidth && bm.bmWidth != cx) {
                HANDLE c = CopyImage(r, IMAGE_CURSOR, cx, cy, LR_COPYFROMRESOURCE);
                if (c) return c;
            }
        }
        return r;
    }
    return LoadImageW(inst, name, type, cx, cy, f);
}

/* ---------------- заставка ---------------- */
static int is_splash(HWND h)
{
    WCHAR c[64];
    return h && GetClassNameW(h, c, 64) && wcsstr(c, L"SPLASH");
}

static void gdi_up(HDC dc)
{
    if (!dc) return;
    float s = to_phys_f(K, 1.0f);
    XFORM x = {s, 0, 0, s, 0, 0};
    SetGraphicsMode(dc, GM_ADVANCED);
    SetWorldTransform(dc, &x);
}

static void gdi_reset(HDC dc)
{
    ModifyWorldTransform(dc, NULL, MWT_IDENTITY);
    SetGraphicsMode(dc, GM_COMPATIBLE);
}

static HDC WINAPI hk_BeginPaint(HWND h, PAINTSTRUCT *ps)
{
    HDC dc = BeginPaint(h, ps);
    if (dc && ps && is_splash(h)) {
        gdi_up(dc);
        RECT *r = &ps->rcPaint;
        r->left = (LONG)size_to_log(K, r->left); r->top = (LONG)size_to_log(K, r->top);
        r->right = (LONG)size_to_log(K, r->right) + 1; r->bottom = (LONG)size_to_log(K, r->bottom) + 1;   /* край не терять */
    }
    return dc;
}

static BOOL WINAPI hk_EndPaint(HWND h, const PAINTSTRUCT *ps)
{
    if (ps && is_splash(h)) gdi_reset(ps->hdc);
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
    if (dc && is_splash(h)) gdi_reset(dc);
    return ReleaseDC(h, dc);
}

static BOOL WINAPI hk_InvalidateRect(HWND h, const RECT *r, BOOL erase)
{
    if (!r || !is_splash(h)) return InvalidateRect(h, r, erase);
    RECT c = *r;
    rect_to_phys(K, &c.left, &c.top, &c.right, &c.bottom);
    return InvalidateRect(h, &c, erase);
}

void cursor_install(void)
{
    if (ratio_is_one(K) && !presets_on()) return;
    HMODULE exe = GetModuleHandleW(NULL);
    static const struct { const char *n; void *h; } U[] = {
        {"CreateIconIndirect", hk_CreateIconIndirect}, {"LoadImageW", hk_LoadImageW},
        {"BeginPaint", hk_BeginPaint}, {"EndPaint", hk_EndPaint}, {"GetDC", hk_GetDC}, {"ReleaseDC", hk_ReleaseDC},
        {"InvalidateRect", hk_InvalidateRect}, {"SetCursor", hk_SetCursor}, {"DestroyIcon", hk_DestroyIcon},
    };
    int ok = 0, n = (int)(sizeof U / sizeof U[0]);
    for (int i = 0; i < n; i++) ok += iat_patch(exe, "user32.dll", U[i].n, U[i].h, NULL);
    LOG("IAT: cursor/splash user32 %d/%d hooked", ok, n);
}
