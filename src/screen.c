/* Логический (видит игра) и физический (рисуем) размер экрана. Меняются вместе под одной блокировкой. */
#include "common.h"

static Screen g_scr;
static SRWLOCK g_lock = SRWLOCK_INIT;

Screen screen_get(void)
{
    AcquireSRWLockShared(&g_lock);
    Screen s = g_scr;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

static void set(LONG lw, LONG lh, LONG pw, LONG ph)
{
    AcquireSRWLockExclusive(&g_lock);
    int changed = g_scr.lw != lw || g_scr.lh != lh || g_scr.pw != pw || g_scr.ph != ph;
    g_scr.lw = lw; g_scr.lh = lh; g_scr.pw = pw; g_scr.ph = ph;
    ReleaseSRWLockExclusive(&g_lock);
    if (changed) LOG("screen: logical %ldx%ld, physical %ldx%ld (scale %d/%d)", lw, lh, pw, ph, g_cfg.scale.num, g_cfg.scale.den);
}

void screen_set_logical(UINT lw, UINT lh)
{
    if (!lw || !lh) return;
    set((LONG)lw, (LONG)lh, (LONG)to_phys(g_cfg.scale, lw), (LONG)to_phys(g_cfg.scale, lh));
}

void screen_set_physical(UINT pw, UINT ph)
{
    if (!pw || !ph) return;
    Screen s = screen_get();
    /* тот же экран, что уже знаем (логическое задано игрой) — не пересчитываем: обратное округление неоднозначно */
    if (s.pw == (LONG)pw && s.ph == (LONG)ph && s.lw) return;
    set((LONG)size_to_log(g_cfg.scale, pw), (LONG)size_to_log(g_cfg.scale, ph), (LONG)pw, (LONG)ph);
}

/* F14: семейство формата (typeless / unorm / srgb одного вида) */
static int family(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 87;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return 88;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 28;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return 24;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 10;
    default: return (int)f;
    }
}

static volatile LONG g_bbfmt = DXGI_FORMAT_B8G8R8A8_UNORM;

void screen_set_format(DXGI_FORMAT f)
{
    if (f) InterlockedExchange(&g_bbfmt, (LONG)f);
}

int screen_is_output_format(DXGI_FORMAT f)
{
    return family(f) == family((DXGI_FORMAT)g_bbfmt);
}
