/* F03: виртуальные режимы экрана. Игрок ставит в игре родное разрешение монитора (верхнюю строку списка), а мод сам
 * делит его на k: игра видит монитор и список режимов логическими (физическое / k).
 *  - k подгоняется под родной экран (ratio_fit), чтобы родной режим делился без остатка: 1440p x1.5 -> 160/107.
 *  - В список попадают только режимы, которые делятся точно: выбор любого из них даёт настоящий режим монитора.
 *  - Источники списка: EnumDisplaySettingsW(i) (через input.c) и DXGI IDXGIOutput::GetDisplayModeList(1).
 *    DXGI меняем только для вызовов из exe игры: сама DXGI и оверлей Steam видят настоящие режимы.
 *  - DesktopCoordinates выхода не трогаем: процесс понимает DPI (раньше — из-за галочки совместимости у exe,
 *    с ревизии В1 мод объявляет это сам, dpi.c), координаты настоящие. */
#include <stdio.h>
#include <string.h>
#include "common.h"

static BYTE *g_exe_base;
static SIZE_T g_exe_size;

int from_exe(void *ret)
{
    if (!g_exe_base) {
        BYTE *b = (BYTE *)GetModuleHandleW(NULL);
        g_exe_size = ((IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew))->OptionalHeader.SizeOfImage;
        g_exe_base = b;
    }
    return (BYTE *)ret >= g_exe_base && (BYTE *)ret < g_exe_base + g_exe_size;
}

/* список режимов Windows. Тесты (test_dsr): последним идёт ещё один режим больше монитора — как при NVIDIA DSR/DLDSR
 * или AMD VSR */
static BOOL enum_modes(LPCWSTR dev, DWORD i, DEVMODEW *dm)
{
    if (EnumDisplaySettingsW(dev, i, dm)) return TRUE;
    if (g_cfg.test_dsr_w <= 0 || i == 0 || !EnumDisplaySettingsW(dev, i - 1, dm)) return FALSE;
    dm->dmPelsWidth = (DWORD)g_cfg.test_dsr_w;
    dm->dmPelsHeight = (DWORD)g_cfg.test_dsr_h;
    return TRUE;
}

/* С5: родное разрешение = разрешение рабочего стола. Самый большой режим из списка Windows — не всегда монитор:
 * с NVIDIA DSR/DLDSR или AMD VSR в списке есть режимы больше панели (5K, 8K), и мод принимал их за монитор.
 * Ключ monitor=WxH в ui4k.ini задаёт родное вручную. */
static void native(DWORD *pw, DWORD *ph)
{
    static volatile LONG said;
    DEVMODEW dm;
    DWORD mw = 0, mh = 0;
    *pw = *ph = 0;
    if (g_cfg.test_mw > 0 && g_cfg.test_mh > 0) { *pw = (DWORD)g_cfg.test_mw; *ph = (DWORD)g_cfg.test_mh; return; }
    for (DWORD i = 0;; i++) {   /* наибольший режим — только для лога и на случай, если рабочий стол спросить не удалось */
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!enum_modes(NULL, i, &dm)) break;
        if ((ULONGLONG)dm.dmPelsWidth * dm.dmPelsHeight > (ULONGLONG)mw * mh) { mw = dm.dmPelsWidth; mh = dm.dmPelsHeight; }
    }
    static const DWORD which[2] = {ENUM_REGISTRY_SETTINGS, ENUM_CURRENT_SETTINGS};
    for (int k = 0; k < 2 && !*pw; k++) {
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, which[k], &dm) && dm.dmPelsWidth && dm.dmPelsHeight) { *pw = dm.dmPelsWidth; *ph = dm.dmPelsHeight; }
    }
    if (!*pw) { *pw = mw; *ph = mh; }
    if (!InterlockedExchange(&said, 1))
        LOG("modes: native = desktop mode %lux%lu; largest mode in the Windows list %lux%lu%s", *pw, *ph, mw, mh,
            (ULONGLONG)mw * mh > (ULONGLONG)*pw * *ph ? " (larger than the desktop: DSR / VSR - not the monitor)" : "");
}

long modes_native_width(void)
{
    DWORD pw, ph;
    native(&pw, &ph);
    return (long)pw;
}

void modes_fit(void)
{
    DWORD pw = 0, ph = 0;
    native(&pw, &ph);
    if (!pw) { LOG("modes: no display modes, scale stays %d/%d", g_cfg.scale.num, g_cfg.scale.den); return; }
    Ratio k = g_cfg.scale, f = ratio_fit(k, (long)pw, (long)ph);
    long lw, lh;
    if (mode_to_log(f, (long)pw, (long)ph, &lw, &lh)) {
        g_cfg.scale = f;
        LOG("modes: native %lux%lu, scale %d/%d -> %d/%d (%.4f), game sees %ldx%ld", pw, ph, k.num, k.den, f.num, f.den,
            (double)f.num / f.den, lw, lh);
    } else {
        LOG("modes: native %lux%lu does not divide by %d/%d exactly; game sees %lldx%lld, edges may be off by 1 px", pw, ph,
            k.num, k.den, size_to_log(k, pw), size_to_log(k, ph));
    }
}

/* ---- L8: строки «мир + масштаб» в списке разрешений игры ----
 * Каждая строка — свой логический размер L (под него игра раскладывает интерфейс), точно делящий родной M:
 * L = M·t/g (g = НОД сторон M), масштаб k = g/t. Для одного масштаба строки разных миров различаются на 1 шаг t
 * (k отличается < 2%, на глаз не видно). Подпись строки (loc.c) — «<мир> ×<масштаб>». Игра применяет строку через
 * ResizeTarget/ResizeBuffers/создание swapchain — там preset_apply() переключает масштаб и мир. */
typedef struct { UINT lw, lh; Ratio k, scene; UINT rw, rh; double lab; int mon, fhd, door; } Preset;   /* lab — масштаб в подписи;
                                                                                         door — строка-переход на другой монитор */
static Preset g_ps[96];
static int g_nps;

/* Второй монитор (17.11). Игра спрашивает список разрешений один раз при запуске и только у основного монитора, а
 * полный экран включает на мониторе своего окна (разведка в игре). У каждой строки свой монитор; применённая строка
 * переносит окно игры на свой монитор (swapchain.c). Мониторы одного размера дают одни и те же строки: окно на любом из
 * них строке подходит, переносить не нужно. Какие строки в списке — presets_build. */
typedef struct { WCHAR dev[32]; LONG x, y; DWORD pw, ph; } Mon;
#define MAXMON 4
static Mon g_mon[MAXMON];
static int g_nmon, g_curmon;
static HWND g_gamewnd;      /* окно игры (swapchain) */
static HMONITOR g_gamemon;  /* его монитор (для лога) */

static long gcd_l(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a; }

int presets_on(void) { return g_nps > 0; }

/* Безопасный режим (msaa_safe=1, запасной выключатель): пока MSAA включён, мир рисуется в размере интерфейса — как на
 * строках «мир = экран»: нет своей цели мира и нет картинок двух разных размеров. По умолчанию выключен (П7): мир
 * остаётся в размере строки и с MSAA, своя цель мира многовыборочная (world.c). Размер мира, меняющийся на ходу,
 * давал двоение затенения, когда MSAA включали в бою (Г1: игра пересоздаёт не все свои картинки). */
int msaa_safe(void) { return g_cfg.msaa_safe && g_cfg.game_aa > 0; }

/* Включён ли MSAA сейчас. Источники: число выборок последнего swapchain, созданного сменой настроек (с MSAA игра
 * делает многовыборочный swapchain — проверка 17.11), значение из профиля (игра пишет его перед сменой) и, если ни
 * того ни другого нет, многовыборочная экранная цель. */
static LONG g_aa_sc = -1, g_aa_prof = -1;
static Ratio g_scene_base = {0, 1};

void scene_set_base(Ratio r) { g_scene_base = r; }

static void aa_update(const char *why)
{
    LONG v = g_aa_sc > 1 ? g_aa_sc : g_aa_prof > 0 ? g_aa_prof : 0;
    LONG was = g_cfg.game_aa;
    if ((v > 0) == (was > 0)) { if (v > 0) InterlockedExchange(&g_cfg.game_aa, v); return; }
    InterlockedExchange(&g_cfg.game_aa, v);
    if (v > 0)
        LOG("MSAA x%ld is now on (%s)%s", (long)v, why, g_cfg.msaa_safe ? " - safe mode: world at interface size" : "");
    else
        LOG("MSAA is now off (%s)%s", why, g_cfg.msaa_safe ? " - safe mode ends: world at the size of the entry" : "");
    preset_refresh();
}

void msaa_swapchain(UINT samples, int fresh)
{
    if (fresh) g_aa_sc = (LONG)samples;
    else if (samples > 1) g_aa_sc = (LONG)samples;   /* первый swapchain игра создаёт без MSAA, даже если он включён */
    else return;
    aa_update("swap chain");
}

void msaa_profile(int aa)
{
    g_aa_prof = aa;
    aa_update("game profile");
}

void msaa_seen(UINT samples)
{
    if (samples < 2 || g_cfg.game_aa > 0) return;
    InterlockedExchange(&g_cfg.game_aa, (LONG)samples);
    LOG("MSAA x%u noticed at run time (the game created a multisampled screen target)%s", samples,
        g_cfg.msaa_safe ? " - safe mode: world at interface size" : "");
    preset_refresh();
}

static const Preset *preset_find(UINT lw, UINT lh)
{
    for (int i = 0; i < g_nps; i++)
        if (g_ps[i].lw == lw && g_ps[i].lh == lh) return &g_ps[i];
    return NULL;
}

int preset_apply(UINT lw, UINT lh)
{
    const Preset *p = preset_find(lw, lh);
    if (!p) return 0;
    Ratio was = g_cfg.scale;
    if (!ratio_eq(p->k, g_cfg.scale) || !ratio_eq(p->scene, g_cfg.scene))
        LOG("preset %ux%u: interface %d/%d (%.3f), world %ux%u", lw, lh, p->k.num, p->k.den, (double)p->k.num / p->k.den,
            p->rw, p->rh);
    if (p->mon != g_curmon)
        LOG("preset %ux%u: entry of monitor %lux%lu%s", lw, lh, g_mon[p->mon].pw, g_mon[p->mon].ph, p->mon ? " (not the main display)" : "");
    g_curmon = p->mon;
    g_cfg.scale = p->k;
    g_cfg.scene = p->scene;
    if (msaa_safe() && !ratio_eq(p->scene, p->k)) {
        g_cfg.scene = p->k;
        LOG("preset %ux%u: safe mode (MSAA x%ld) - world at interface size instead of %ux%u", lw, lh, (long)g_cfg.game_aa, p->rw, p->rh);
    }
    views_scale_changed(was, g_cfg.scale);   /* Э9: на ходу — виды 3D игры и иконки предметов заново, в новом масштабе */
    return 1;
}

/* Реверс 19.11 (eDeviceScreen.cpp): 0x4d1de0 строит список разрешений игры — режимы не меньше 1360x768, по возрастанию;
 * 0x4cfaa0 («screen:create», сразу после создания устройства) берёт его ПЕРВУЮ строку — «OnTargetSize», «SetResolution:
 * RESET». Строку профиля (options.set {mode}) игра применяет позже (ResizeTarget, ~3 с). Без мода первая строка — 1360x768
 * (окно запуска из реверса), с нашими строками — наименьшая строка (на 4K 1856x1044, x2.069). За эти секунды игра делает
 * виды 3D (иконки инвентаря, 512 и 1024), атлас букв, кэши — и они оставались в масштабе этой строки до конца сессии.
 * Поэтому первый swap chain процесса размером наименьшей строки берёт масштаб и мир строки профиля, если она есть в
 * списке. 1 — так и сделано. */
int preset_startup(UINT lw, UINT lh)
{
    static volatile LONG done;
    if (InterlockedExchange(&done, 1) || !presets_on() || !g_nps) return 0;
    UINT pw, ph;
    if (!config_profile_mode(&pw, &ph) || (pw == lw && ph == lh)) return 0;
    int least = 0;   /* наименьшая строка — как сортирует игра (по площади); строки-переходы на другие мониторы тоже в списке */
    for (int i = 1; i < g_nps; i++)
        if ((ULONGLONG)g_ps[i].lw * g_ps[i].lh < (ULONGLONG)g_ps[least].lw * g_ps[least].lh) least = i;
    if (g_ps[least].lw != lw || g_ps[least].lh != lh || !preset_find(pw, ph)) return 0;
    LOG("preset %ux%u at start: the game opens on the first entry of its list (eDeviceScreen) - interface and world of the "
        "profile entry %ux%u, which it applies next", lw, lh, pw, ph);
    return preset_apply(pw, ph);
}

/* состояние MSAA сменилось — мир текущей строки заново (со строками списка или с ключом render) */
void preset_refresh(void)
{
    Screen s = screen_get();
    if (presets_on()) { if (s.lw) preset_apply((UINT)s.lw, (UINT)s.lh); return; }
    if (g_scene_base.num) g_cfg.scene = msaa_safe() ? g_cfg.scale : g_scene_base;
}

static void fmt_k(char *b, size_t n, double k)
{
    snprintf(b, n, "%.2f", k);
    size_t l = strlen(b);
    while (l && b[l - 1] == '0') b[--l] = 0;
    if (l && b[l - 1] == '.') b[--l] = 0;
}

/* мониторы рабочего стола: основной первым (родное — по native(): рабочий стол, monitor=, DSR), за ним остальные с
 * другими размерами. Тесты: test_monitor= — настоящие не смотрим, test_monitor2= — второй справа от основного. */
static void monitors_find(void)
{
    DWORD pw, ph;
    g_nmon = 0;
    native(&pw, &ph);
    if (!pw || !ph) return;
    Mon *m = &g_mon[g_nmon++];
    ZeroMemory(m, sizeof *m);
    m->pw = pw; m->ph = ph;
    DISPLAY_DEVICEW dd;
    for (DWORD i = 0;; i++) {
        ZeroMemory(&dd, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { lstrcpynW(m->dev, dd.DeviceName, 32); break; }
    }
    if (g_cfg.test_mon) {
        if (g_cfg.test_m2w > 0 && g_cfg.test_m2h > 0) {
            Mon *t = &g_mon[g_nmon++];
            ZeroMemory(t, sizeof *t);
            lstrcpyW(t->dev, L"\\\\.\\UI4KTEST2");
            t->x = (LONG)pw;
            t->pw = (DWORD)g_cfg.test_m2w; t->ph = (DWORD)g_cfg.test_m2h;
        }
        return;
    }
    if (!g_cfg.all_monitors) return;
    for (DWORD i = 0; g_nmon < MAXMON; i++) {
        ZeroMemory(&dd, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm) || !dm.dmPelsWidth || !dm.dmPelsHeight) continue;
        int same = 0;
        for (int j = 0; j < g_nmon; j++) same |= g_mon[j].pw == dm.dmPelsWidth && g_mon[j].ph == dm.dmPelsHeight;
        LOG("modes: another monitor %ls %lux%lu at %ld,%ld%s", dd.DeviceName, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmPosition.x,
            dm.dmPosition.y, same ? " - same size as one already listed, same entries" : "");
        if (same) continue;
        Mon *o = &g_mon[g_nmon++];
        ZeroMemory(o, sizeof *o);
        lstrcpynW(o->dev, dd.DeviceName, 32);
        o->x = dm.dmPosition.x; o->y = dm.dmPosition.y;
        o->pw = dm.dmPelsWidth; o->ph = dm.dmPelsHeight;
    }
}

/* строки монитора mi -> out (без подписей). *first — строка до первого выбора игры; fhd — «как на Full HD» */
static int rows_of(int mi, Preset *out, int cap, int *first, int *pnw, int *pnk)
{
    DWORD pw = g_mon[mi].pw, ph = g_mon[mi].ph;
    long g = gcd_l((long)pw, (long)ph);
    int n = 0;
    *first = -1;
    /* миры: родной и до двух стандартных ниже (1440 / 1080 / 720 по высоте, та же пропорция) */
    UINT ww[3], wh[3];
    int nw = 0;
    ww[nw] = pw; wh[nw++] = ph;
    static const UINT hs[] = {1440, 1080, 720};
    for (int i = 0; i < 3 && nw < 3; i++)
        if (hs[i] < ph) { wh[nw] = hs[i]; ww[nw] = (UINT)(((ULONGLONG)pw * hs[i] / ph + 1) & ~1ull); nw++; }
    /* масштабы: ×1..×2 и «как на Full HD» (высота интерфейса 1080) */
    double ks[8] = {1, 1.25, 1.5, 1.75, 2};
    int nk = 5, fhd = -1;
    double kf = ph / 1080.0;
    if (kf > 1.01) {
        for (int i = 0; i < nk; i++) if (ks[i] > kf - 0.01 && ks[i] < kf + 0.01) fhd = i;
        if (fhd < 0) {
            int j = nk;
            while (j > 0 && ks[j - 1] > kf) { ks[j] = ks[j - 1]; j--; }
            ks[j] = kf; fhd = j; nk++;
        }
    }
    *pnw = nw; *pnk = nk;
    for (int ki = 0; ki < nk; ki++) {
        /* шаги t, при которых обе стороны L делятся на 4: у игры половины и четверти экрана целые (при нечётных
         * 535.5 / 267.75 её вспомогательные картинки на строку меньше — видна полоса). Ближайшие к g/k — родному миру. */
        double tr = g / ks[ki];
        long ts[3];
        int nt = 0;
        for (long d = 0; nt < nw && d < g; d++)
            for (int sgn = 0; sgn < 2 && nt < nw; sgn++) {
                long t = sgn ? (long)(tr + 0.5) + d : (long)(tr + 0.5) - d;
                if (sgn && d == 0) continue;
                if (t < 1 || t > g || ((long)pw / g * t) % 4 || ((long)ph / g * t) % 4) continue;
                int dup = 0;
                for (int q = 0; q < nt; q++) dup |= ts[q] == t;
                if (!dup) ts[nt++] = t;
            }
        for (int wi = 0; wi < nw && wi < nt; wi++) {
            long t = ts[wi];
            if (t < 1 || n >= cap) continue;
            Preset p;
            ZeroMemory(&p, sizeof p);
            p.lw = (UINT)((long)pw / g * t); p.lh = (UINT)((long)ph / g * t);
            /* Реверс 20.11 (Ghidra, Render::eDx11Device слот 12, 0x50ae80): игра берёт в свой список только режимы шириной
             * >= 1360 И высотой >= 768 — меньшую строку она выбросит. Такой строки не делаем: иначе её подпись лишняя, а
             * «наименьшая строка» preset_startup не совпала бы с первой строкой игры (1080p / 1440p: 1280x720) */
            if (p.lw < 1360 || p.lh < 768) continue;
            int dup = 0;
            for (int q = 0; q < n; q++) dup |= out[q].lw == p.lw && out[q].lh == p.lh;
            if (dup) continue;
            double dev = (double)g / t / ks[ki] - 1;
            if (dev > 0.06 || dev < -0.06) continue;   /* грубый шаг (малый монитор, ультраширокий) — строка не та, что в подписи */
            long kg = gcd_l(g, t);
            p.k.num = (int)(g / kg); p.k.den = (int)(t / kg);
            long sg = gcd_l((long)ww[wi], (long)p.lw);
            p.scene.num = (int)(ww[wi] / sg); p.scene.den = (int)(p.lw / sg);
            p.rw = ww[wi]; p.rh = wh[wi]; p.lab = ks[ki]; p.mon = mi; p.fhd = ki == fhd;
            out[n++] = p;
            if (*first < 0 || (wi == 0 && ks[ki] > (double)g_cfg.scale.num / g_cfg.scale.den - 0.01 &&
                               ks[ki] < (double)g_cfg.scale.num / g_cfg.scale.den + 0.01))
                *first = n - 1;
        }
    }
    return n;
}

/* Широкоформатные и прочие мониторы (17.11, просьба пользователя): в списке — только строки монитора игры, той же
 * пропорции, что он сам. Список игра строит один раз при запуске, поэтому монитор игры решается при запуске: тот, чья
 * строка выбрана в профиле игры (там игра была в прошлый раз), иначе основной. Чтобы перейти на другой монитор из
 * игры — по одной строке на каждый другой монитор с подписью «— другой монитор» (его «как на Full HD», иначе родной
 * ×1): она переносит игру туда, а со следующего запуска список уже для того монитора. */
static int g_gmon;   /* монитор игры: его строки в списке */

void presets_build(void)
{
    monitors_find();
    if (!g_nmon) { LOG("presets: no display modes"); return; }
    static Preset tmp[48];
    int f, nw, nk, G = 0;
    UINT pmw = 0, pmh = 0;
    const char *why = "the main display";
    int found = 0;
    if (g_nmon > 1 && config_profile_mode(&pmw, &pmh))
        for (int mi = 0; mi < g_nmon && !found; mi++) {
            int n = rows_of(mi, tmp, 48, &f, &nw, &nk);
            for (int i = 0; i < n && !found; i++)
                if (tmp[i].lw == pmw && tmp[i].lh == pmh) {
                    found = 1;
                    G = mi;
                    why = mi ? "the entry in the game profile is for it" : "the main display, its entry is in the game profile";
                }
        }
    g_gmon = g_curmon = G;
    int first = -1;
    g_nps = rows_of(G, g_ps, (int)(sizeof g_ps / sizeof *g_ps), &first, &nw, &nk);
    int nG = g_nps;
    for (int mi = 0; mi < g_nmon; mi++) {   /* строки-переходы на другие мониторы */
        if (mi == G || g_nps >= (int)(sizeof g_ps / sizeof *g_ps)) continue;
        int n = rows_of(mi, tmp, 48, &f, &nw, &nk), pick = -1;
        for (int pass = 0; pass < 3 && pick < 0; pass++)
            for (int i = 0; i < n && pick < 0; i++) {
                int ok = pass == 0 ? tmp[i].fhd && tmp[i].rw == g_mon[mi].pw
                       : pass == 1 ? tmp[i].lab == 1 && tmp[i].rw == g_mon[mi].pw : 1;
                if (ok && !preset_find(tmp[i].lw, tmp[i].lh)) pick = i;
            }
        if (pick < 0) continue;
        tmp[pick].door = 1;
        g_ps[g_nps++] = tmp[pick];
    }
    for (int i = 0; i < g_nps; i++) {   /* подписи (loc.c) */
        const Preset *p = &g_ps[i];
        char mode[24], kt[16], ru[160], en[128];
        snprintf(mode, sizeof mode, "%ux%u", p->lw, p->lh);
        fmt_k(kt, sizeof kt, p->lab);
        const char *x = "\xC3\x97";   /* × */
        const char *rs = p->door ? "  \xE2\x80\x94 \xD0\xB4\xD1\x80\xD1\x83\xD0\xB3\xD0\xBE\xD0\xB9 "
                                   "\xD0\xBC\xD0\xBE\xD0\xBD\xD0\xB8\xD1\x82\xD0\xBE\xD1\x80"   /* — другой монитор */
                         : p->fhd ? "  \xE2\x80\x94 \xD0\xB8\xD0\xBD\xD1\x82\xD0\xB5\xD1\x80\xD1\x84\xD0\xB5\xD0\xB9\xD1\x81 "
                                    "\xD0\xBA\xD0\xB0\xD0\xBA \xD0\xBD\xD0\xB0 Full HD"   /* — интерфейс как на Full HD */
                         : "";
        snprintf(ru, sizeof ru, "%u%s%u  %s%s%s", p->rw, x, p->rh, x, kt, rs);
        snprintf(en, sizeof en, "%u%s%u  %s%s%s", p->rw, x, p->rh, x, kt,
                 p->door ? "  \xE2\x80\x94 other monitor" : p->fhd ? "  \xE2\x80\x94 UI like Full HD" : "");
        loc_add(mode, ru, en);
    }
    /* по возрастанию размера, как список режимов DXGI: игра берёт последнюю строку как наибольшую.
     * (Свой порядок не поможет: игра сама сортирует список по размеру — проверено 16.11.) */
    for (int i = 1; i < g_nps; i++)
        for (int j = i; j > 0 && (ULONGLONG)g_ps[j].lw * g_ps[j].lh < (ULONGLONG)g_ps[j - 1].lw * g_ps[j - 1].lh; j--) {
            Preset t = g_ps[j]; g_ps[j] = g_ps[j - 1]; g_ps[j - 1] = t;
            if (first == j) first = j - 1; else if (first == j - 1) first = j;
        }
    rows_of(G, tmp, 48, &f, &nw, &nk);   /* nw, nk — монитора игры (для строки лога) */
    LOG("presets: monitor %lux%lu, %d entries (%d worlds x %d scales) in the game resolution list%s%ls (%s)", g_mon[G].pw,
        g_mon[G].ph, nG, nw, nk, G ? " - not the main display, " : "", G ? g_mon[G].dev : L"", why);
    for (int i = 0; i < g_nps; i++)
        if (g_ps[i].door)
            LOG("presets: + entry %ux%u to switch to monitor %lux%lu (%ls at %ld,%ld) - applying it moves the game there, its own "
                "entries come with the next start", g_ps[i].lw, g_ps[i].lh, g_mon[g_ps[i].mon].pw, g_mon[g_ps[i].mon].ph,
                g_mon[g_ps[i].mon].dev, g_mon[g_ps[i].mon].x, g_mon[g_ps[i].mon].y);
    for (int i = 0; i < g_nps; i++)
        LOGV("  preset %ux%u: interface %d/%d world %ux%u label %.4f monitor %lux%lu", g_ps[i].lw, g_ps[i].lh, g_ps[i].k.num,
             g_ps[i].k.den, g_ps[i].rw, g_ps[i].rh, g_ps[i].lab, g_mon[g_ps[i].mon].pw, g_mon[g_ps[i].mon].ph);
    /* до первой строки игры — строка профиля (её игра и выберет; см. preset_startup), иначе родная */
    UINT qw, qh;
    const Preset *pp = config_profile_mode(&qw, &qh) ? preset_find(qw, qh) : NULL;
    if (pp) { g_cfg.scale = pp->k; g_cfg.scene = pp->scene; }
    else if (first >= 0) { g_cfg.scale = g_ps[first].k; g_cfg.scene = g_ps[first].scene; }
}

/* монитор по имени устройства (\\.\DISPLAY2); NULL — основной; -1 — монитора без строк */
static int mon_by_dev(const WCHAR *dev)
{
    if (!dev || !dev[0]) return g_nmon ? 0 : -1;
    for (int i = 0; i < g_nmon; i++)
        if (!lstrcmpiW(g_mon[i].dev, dev)) return i;
    return -1;
}

/* режим (w, h) — родной режим монитора mi: на его месте в списке — все строки */
static int preset_slot(int mi, DWORD w, DWORD h) { return mi >= 0 && w == g_mon[mi].pw && h == g_mon[mi].ph; }

/* второй монитор: текущая строка — монитора другого размера, чем тот, где окно игры? 1 — *to: левый верх монитора
 * строки. Монитор окна — тот из списка, где центр окна; вне списка — по размеру монитора окна */
int mon_misplaced(HWND *h, POINT *to)
{
    if (!g_gamewnd || !presets_on() || g_curmon < 0 || g_curmon >= g_nmon || IsIconic(g_gamewnd)) return 0;
    RECT r;
    if (!GetWindowRect(g_gamewnd, &r)) return 0;
    LONG cx = r.left + (r.right - r.left) / 2, cy = r.top + (r.bottom - r.top) / 2;
    const Mon *m = &g_mon[g_curmon];
    DWORD ww = 0, wh = 0;
    for (int i = 0; i < g_nmon && !ww; i++)
        if (cx >= g_mon[i].x && cx < g_mon[i].x + (LONG)g_mon[i].pw && cy >= g_mon[i].y && cy < g_mon[i].y + (LONG)g_mon[i].ph) {
            ww = g_mon[i].pw; wh = g_mon[i].ph;
        }
    if (!ww) {
        MONITORINFOEXW mi;
        mi.cbSize = sizeof mi;
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof dm);
        dm.dmSize = sizeof dm;
        HMONITOR hm = MonitorFromWindow(g_gamewnd, MONITOR_DEFAULTTONEAREST);
        if (hm && GetMonitorInfoW(hm, (MONITORINFO *)&mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            ww = dm.dmPelsWidth; wh = dm.dmPelsHeight;
        }
    }
    if (ww == m->pw && wh == m->ph) return 0;
    *h = g_gamewnd;
    to->x = m->x; to->y = m->y;
    return 1;
}

/* i-й режим, который делится точно; dm — буфер игры (её dmSize сохраняется) */
BOOL modes_enum(LPCWSTR dev, DWORD mode, DEVMODEW *dm)
{
    static volatile LONG logged, said;
    DWORD n = 0;
    if (mode == 0 && InterlockedIncrement(&said) <= 8) LOG("modes: EnumDisplaySettingsW(%ls) list asked", dev ? dev : L"main display");
    int mi = mon_by_dev(dev);
    if (presets_on() && mi >= 0) {   /* L8: родные режимы монитора (по частотам) x строки всех мониторов */
        for (DWORD i = 0;; i++) {
            if (!enum_modes(dev, i, dm)) {
                /* С5: у этого монитора нет режима, равного родному (второй монитор другого размера) — строк для него
                 * нет; не оставлять игру с пустым списком, отдать обычный логический */
                if (n == 0) break;
                if (mode == 0 || !InterlockedExchange(&logged, 1)) LOG("presets: EnumDisplaySettingsW list: %lu entries", n);
                return FALSE;
            }
            if (!preset_slot(mi, dm->dmPelsWidth, dm->dmPelsHeight)) continue;
            if (mode < n + (DWORD)g_nps) {
                dm->dmPelsWidth = g_ps[mode - n].lw;
                dm->dmPelsHeight = g_ps[mode - n].lh;
                return TRUE;
            }
            n += (DWORD)g_nps;
        }
    }
    for (DWORD i = 0;; i++) {
        if (!enum_modes(dev, i, dm)) {
            if (mode == 0 || !InterlockedExchange(&logged, 1)) LOG("modes: EnumDisplaySettingsW list: %lu logical modes", n);
            return FALSE;
        }
        long lw, lh;
        if (!mode_to_log(g_cfg.scale, (long)dm->dmPelsWidth, (long)dm->dmPelsHeight, &lw, &lh)) continue;
        if (n++ == mode) {
            dm->dmPelsWidth = (DWORD)lw;
            dm->dmPelsHeight = (DWORD)lh;
            return TRUE;
        }
    }
}

/* ---- монитор окна игры (второй монитор, 17.11) ----
 * Разведка: на каком мониторе окно игры и когда оно переходит на другой; какие выходы DXGI спрашивает игра. */
static void out_name(IDXGIOutput *o, WCHAR *n, size_t cap, RECT *rc)
{
    DXGI_OUTPUT_DESC d;
    n[0] = 0;
    if (o && SUCCEEDED(IDXGIOutput_GetDesc(o, &d))) {
        lstrcpynW(n, d.DeviceName, (int)cap);
        if (rc) *rc = d.DesktopCoordinates;
    } else lstrcpynW(n, L"-", (int)cap);
}

void mon_track(HWND h, const char *why)
{
    if (h) g_gamewnd = h;
    if (!g_gamewnd) return;
    HMONITOR m = MonitorFromWindow(g_gamewnd, MONITOR_DEFAULTTONEAREST);
    if (!m || m == g_gamemon) return;
    g_gamemon = m;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof dm);
    dm.dmSize = sizeof dm;
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return;
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    LOG("monitor: game window on %ls %lux%lu at %ld,%ld%s (%s)", mi.szDevice, dm.dmPelsWidth, dm.dmPelsHeight, mi.rcMonitor.left,
        mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " - main display" : " - not the main display", why);
}

void mon_track_msg(HWND h)
{
    if (h && h == g_gamewnd) mon_track(NULL, "window moved");
}

/* ---- DXGI: фабрика -> адаптер -> выход ---- */
static VtHook h_ea, h_ea1, h_eo, h_dml, h_dml1, h_fcm;
typedef HRESULT(STDMETHODCALLTYPE *PFN_EA)(IDXGIFactory *, UINT, IDXGIAdapter **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_EA1)(IDXGIFactory1 *, UINT, IDXGIAdapter1 **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_EO)(IDXGIAdapter *, UINT, IDXGIOutput **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_DML)(IDXGIOutput *, DXGI_FORMAT, UINT, UINT *, void *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_FCM)(IDXGIOutput *, const DXGI_MODE_DESC *, DXGI_MODE_DESC *, IUnknown *);

/* общий для DXGI_MODE_DESC и DXGI_MODE_DESC1: оба начинаются с UINT Width, Height */
static HRESULT mode_list(IDXGIOutput *o, PFN_DML real, size_t es, DXGI_FORMAT fmt, UINT fl, UINT *num, void *out,
                         const char *name)
{
    UINT n = 0;
    HRESULT hr = real(o, fmt, fl, &n, NULL);
    if (FAILED(hr) || !num) return hr;
    BYTE *all = n ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, n * es) : NULL;
    if (n && !all) return E_OUTOFMEMORY;
    if (n && FAILED(hr = real(o, fmt, fl, &n, all))) { HeapFree(GetProcessHeap(), 0, all); return hr; }
    UINT m = 0, maxw = 0, maxh = 0, nn = 0;
    WCHAR on[32];
    out_name(o, on, 32, NULL);
    int mi = mon_by_dev(on);
    if (presets_on())
        for (UINT i = 0; i < n; i++) { UINT *wh = (UINT *)(all + i * es); nn += preset_slot(mi, wh[0], wh[1]); }
    /* С5: у выхода без режима, равного родному (второй монитор другого размера), строк нет — ему обычный список */
    if (nn) {   /* L8: каждый родной режим (частота, развёртка) -> все строки */
        BYTE *ps = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)nn * g_nps * es);
        for (UINT i = 0; ps && i < n; i++) {
            UINT *wh = (UINT *)(all + i * es);
            if (!preset_slot(mi, wh[0], wh[1])) continue;
            for (int j = 0; j < g_nps; j++, m++) {
                memcpy(ps + (size_t)m * es, wh, es);
                UINT *o2 = (UINT *)(ps + (size_t)m * es);
                o2[0] = g_ps[j].lw; o2[1] = g_ps[j].lh;
            }
        }
        if (all) HeapFree(GetProcessHeap(), 0, all);
        all = ps;
        for (int j = 0; m && j < g_nps; j++)
            if ((ULONGLONG)g_ps[j].lw * g_ps[j].lh > (ULONGLONG)maxw * maxh) { maxw = g_ps[j].lw; maxh = g_ps[j].lh; }
    } else
    for (UINT i = 0; i < n; i++) {   /* сжимаем на месте: остаются только точные, уже логические */
        UINT *wh = (UINT *)(all + i * es);
        long lw, lh;
        if (!mode_to_log(g_cfg.scale, (long)wh[0], (long)wh[1], &lw, &lh)) continue;
        if (m != i) memcpy(all + m * es, all + i * es, es);
        wh = (UINT *)(all + m * es);
        wh[0] = (UINT)lw; wh[1] = (UINT)lh;
        if ((ULONGLONG)lw * lh > (ULONGLONG)maxw * maxh) { maxw = (UINT)lw; maxh = (UINT)lh; }
        m++;
    }
    hr = S_OK;
    if (!out) {
        *num = m;
    } else {
        UINT c = *num < m ? *num : m;
        if (c) memcpy(out, all, c * es);
        if (*num < m) hr = DXGI_ERROR_MORE_DATA;
        *num = c;
    }
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 24) {   /* второй монитор: какой выход и когда спрашивает игра */
        LOG("modes: %s(fmt=%d) on %ls %s: %u of %u modes, max %ux%u", name, fmt, on, out ? "list" : "count", m, n, maxw, maxh);
    }
    if (all) HeapFree(GetProcessHeap(), 0, all);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_GetDisplayModeList(IDXGIOutput *o, DXGI_FORMAT fmt, UINT fl, UINT *num, DXGI_MODE_DESC *d)
{
    PFN_DML real = VT_ORIG(o, &h_dml, PFN_DML);
    if (!from_exe(__builtin_return_address(0))) return real(o, fmt, fl, num, d);
    return mode_list(o, real, sizeof(DXGI_MODE_DESC), fmt, fl, num, d, "GetDisplayModeList");
}

static HRESULT STDMETHODCALLTYPE hk_GetDisplayModeList1(IDXGIOutput1 *o, DXGI_FORMAT fmt, UINT fl, UINT *num,
                                                        DXGI_MODE_DESC1 *d)
{
    PFN_DML real = VT_ORIG(o, &h_dml1, PFN_DML);
    if (!from_exe(__builtin_return_address(0))) return real((IDXGIOutput *)o, fmt, fl, num, d);
    return mode_list((IDXGIOutput *)o, real, sizeof(DXGI_MODE_DESC1), fmt, fl, num, d, "GetDisplayModeList1");
}

static HRESULT STDMETHODCALLTYPE hk_FindClosestMatchingMode(IDXGIOutput *o, const DXGI_MODE_DESC *want,
                                                            DXGI_MODE_DESC *got, IUnknown *dev)
{
    PFN_FCM real = VT_ORIG(o, &h_fcm, PFN_FCM);
    if (!want || !from_exe(__builtin_return_address(0))) return real(o, want, got, dev);
    DXGI_MODE_DESC w = *want;
    const Preset *pr = presets_on() ? preset_find(w.Width, w.Height) : NULL;
    if (pr) {   /* L8: строка списка -> родной режим её монитора, игре — её размер */
        w.Width = g_mon[pr->mon].pw; w.Height = g_mon[pr->mon].ph;
        HRESULT hr = real(o, &w, got, dev);
        if (SUCCEEDED(hr) && got) { got->Width = want->Width; got->Height = want->Height; }
        return hr;
    }
    if (w.Width && w.Height) { w.Width = (UINT)to_phys(g_cfg.scale, w.Width); w.Height = (UINT)to_phys(g_cfg.scale, w.Height); }
    HRESULT hr = real(o, &w, got, dev);
    if (SUCCEEDED(hr) && got) {
        long lw, lh;
        if (!mode_to_log(g_cfg.scale, (long)got->Width, (long)got->Height, &lw, &lh)) {
            lw = (long)size_to_log(g_cfg.scale, got->Width);
            lh = (long)size_to_log(g_cfg.scale, got->Height);
        }
        LOG("modes: FindClosestMatchingMode(%ux%u) -> %ux%u, game sees %ldx%ld", want->Width, want->Height, got->Width,
            got->Height, lw, lh);
        got->Width = (UINT)lw; got->Height = (UINT)lh;
    }
    return hr;
}

static void hook_output(IDXGIOutput *o)
{
    VT_PATCH_OBJ(o, IDXGIOutputVtbl, GetDisplayModeList, hk_GetDisplayModeList, &h_dml);
    VT_PATCH_OBJ(o, IDXGIOutputVtbl, FindClosestMatchingMode, hk_FindClosestMatchingMode, &h_fcm);
    IDXGIOutput1 *o1 = NULL;
    if (SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput1, (void **)&o1)) && o1) {
        VT_PATCH_OBJ(o1, IDXGIOutput1Vtbl, GetDisplayModeList1, hk_GetDisplayModeList1, &h_dml1);
        IDXGIOutput1_Release(o1);
    }
}

static HRESULT STDMETHODCALLTYPE hk_EnumOutputs(IDXGIAdapter *a, UINT i, IDXGIOutput **out)
{
    HRESULT hr = VT_ORIG(a, &h_eo, PFN_EO)(a, i, out);
    if (SUCCEEDED(hr) && out && *out) hook_output(*out);
    static volatile LONG said;
    if (InterlockedIncrement(&said) <= 16) {
        WCHAR on[32];
        RECT rc = {0, 0, 0, 0};
        out_name(SUCCEEDED(hr) && out ? *out : NULL, on, 32, &rc);
        LOG("dxgi: EnumOutputs(%u) -> 0x%08lx %ls (%ld,%ld %ldx%ld)%s", i, (unsigned long)hr, on, rc.left, rc.top,
            rc.right - rc.left, rc.bottom - rc.top, from_exe(__builtin_return_address(0)) ? " - asked by the game" : "");
    }
    return hr;
}

static void hook_adapter(IDXGIAdapter *a) { VT_PATCH_OBJ(a, IDXGIAdapterVtbl, EnumOutputs, hk_EnumOutputs, &h_eo); }

static HRESULT STDMETHODCALLTYPE hk_EnumAdapters(IDXGIFactory *f, UINT i, IDXGIAdapter **out)
{
    HRESULT hr = VT_ORIG(f, &h_ea, PFN_EA)(f, i, out);
    if (SUCCEEDED(hr) && out && *out) hook_adapter(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_EnumAdapters1(IDXGIFactory1 *f, UINT i, IDXGIAdapter1 **out)
{
    HRESULT hr = VT_ORIG(f, &h_ea1, PFN_EA1)(f, i, out);
    if (SUCCEEDED(hr) && out && *out) hook_adapter((IDXGIAdapter *)*out);
    return hr;
}

void modes_hook_factory(IDXGIFactory *f)
{
    VT_PATCH(f, IDXGIFactoryVtbl, EnumAdapters, hk_EnumAdapters, &h_ea);
    IDXGIFactory1 *f1 = NULL;
    if (SUCCEEDED(IDXGIFactory_QueryInterface(f, &IID_IDXGIFactory1, (void **)&f1)) && f1) {
        VT_PATCH(f1, IDXGIFactory1Vtbl, EnumAdapters1, hk_EnumAdapters1, &h_ea1);
        IDXGIFactory1_Release(f1);
    }
}
