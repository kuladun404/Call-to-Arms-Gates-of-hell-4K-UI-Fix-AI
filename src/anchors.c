/* Устойчивость к обновлениям игры: опоры мода берутся у игры при запуске (gamedata.h), а не зашиты отпечатками.
 *  - VS интерфейса (world.c, мир без HDR): по именам шейдеров в кэше игры. Отпечатки 16.11 — только запасной путь.
 *  - Размер буфера сцены (cb.c): pscSceneGroup из shader_constants.h игры.
 *  - Шейдеры автоэкспозиции (lum.c): по именам; замена собирается из текущего исходника игры.
 * Каждая опора пишет в лог, откуда она взята. */
#include "common.h"
#include "gamedata.h"
#include <stdio.h>

WCHAR g_shaderpak[MAX_PATH];   /* путь к архиву шейдеров; пусто — <папка игры>\resource\shader.pak (тесты: test_shader_pak=) */

static Pak g_pak;
static int g_pak_ok;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK open_pak(PINIT_ONCE o, PVOID a, PVOID *c)
{
    (void)o; (void)a; (void)c;
    WCHAR p[MAX_PATH];
    if (g_shaderpak[0]) lstrcpynW(p, g_shaderpak, MAX_PATH);
    else { lstrcpynW(p, g_dir, MAX_PATH - 32); lstrcatW(p, L"..\\..\\resource\\shader.pak"); }
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LOG("gamedata: %ls is not available (%lu) - the mod uses what it knew for game version 16.11", p, GetLastError());
        return TRUE;
    }
    LARGE_INTEGER sz = {0};
    GetFileSizeEx(f, &sz);
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    const BYTE *v = m ? (const BYTE *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (m) CloseHandle(m);   /* вид держит отображение сам */
    if (!v) { LOG("gamedata: %ls is not readable (%lu) - the mod uses what it knew for game version 16.11", p, GetLastError()); return TRUE; }
    g_pak_ok = pak_parse(v, (size_t)sz.QuadPart, &g_pak);
    LOG("gamedata: %ls - %d files%s", p, g_pak.n, g_pak_ok ? "" : " - not an uncompressed zip, not used");
    return TRUE;
}

const Pak *gd_pak(void)
{
    InitOnceExecuteOnce(&g_once, open_pak, NULL, NULL);
    return g_pak_ok ? &g_pak : NULL;
}

/* ---- VS интерфейса ---- */
/* Все имена, под которыми в кэше игры (16.11) лежат два VS интерфейса: общий шейдер vs.hlsl с цветом вершин. */
static const char *const UI_VS_NAMES[] = {
    "vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR",
    "vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR$ALTITUDE_HAZE",
    "vs[VS]$TEXCOORDS#1$VTEXCOORDS#1$USE_VCOLOR$VNORMAL$VCOLOR",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR$ALTITUDE_HAZE",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR$BOUNDARY_HAZE",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR$BOUNDARY_HAZE$ALTITUDE_HAZE",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR$BOUNDARY_HAZE$TEX_BLEND",
    "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR$TEX_BLEND",
};
#define NUI (sizeof UI_VS_NAMES / sizeof *UI_VS_NAMES)
static BYTE g_ui[NUI][8];   /* отпечатки (байты 4..11 DXBC) по именам */
static int g_nui;
static INIT_ONCE g_uionce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK learn_ui(PINIT_ONCE o, PVOID a, PVOID *c)
{
    (void)o; (void)a; (void)c;
    const Pak *p = gd_pak();
    int found = 0;
    for (size_t i = 0; p && i < NUI; i++) {
        size_t n;
        const BYTE *code = pak_shader(p, UI_VS_NAMES[i], &n);
        if (!code) continue;
        found++;
        int dup = 0;
        for (int j = 0; j < g_nui; j++) dup |= !memcmp(g_ui[j], code + 4, 8);
        if (!dup) memcpy(g_ui[g_nui++], code + 4, 8);
    }
    if (p)
        LOG("gamedata: interface vertex shaders - %d of %d names found in the game's shader cache, %d distinct%s", found, (int)NUI,
            g_nui, found ? "" : " - names gone (game update?), using the checksums known on 16.11");
    return TRUE;
}

/* байткод — VS интерфейса по именам из кэша игры */
int anchors_ui_vs(const void *code, SIZE_T n)
{
    InitOnceExecuteOnce(&g_uionce, learn_ui, NULL, NULL);
    if (!code || n < 12) return 0;
    for (int i = 0; i < g_nui; i++)
        if (!memcmp((const BYTE *)code + 4, g_ui[i], 8)) return 1;
    return 0;
}

int anchors_ui_learned(void)
{
    InitOnceExecuteOnce(&g_uionce, learn_ui, NULL, NULL);
    return g_nui;
}

/* ---- буфер сцены ---- */
int anchors_scene_floats(void)
{
    static volatile LONG v = -1;
    if (v < 0) {
        const Pak *p = gd_pak();
        int g = p ? pak_scene_groups(p) : 0;
        v = g > 0 && g < 4096 ? g * 4 : 0;
        if (v) LOG("gamedata: scene constant buffer - %ld floats (pscSceneGroup in the game's shader_constants.h)", (long)v);
        else if (p) LOG("gamedata: scene constant buffer size not found in shader_constants.h - cb_fix sizes from ui4k.ini only");
    }
    return (int)v;
}

/* ---- буферы констант библиотеки NVIDIA HBAO+ ----
 * Её шейдеры без отражения (имён нет), а размер буфера зависит от версии библиотеки. Размеры узнаём при создании: буфер
 * констант, который создаёт сама GFSDK_SSAO*.dll. До первого такого — 84 (версия 16.11). */
static volatile LONG g_hb[4], g_nhb;
static VtHook h_cbuf;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CB)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);

static int from_hbao(void *ra)
{
    HMODULE m = NULL;
    WCHAR f[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ra, &m) ||
        !GetModuleFileNameW(m, f, MAX_PATH))
        return 0;
    CharUpperW(f);
    const WCHAR *b = wcsrchr(f, L'\\');
    return wcsstr(b ? b : f, L"GFSDK_SSAO") != NULL;
}

static HRESULT STDMETHODCALLTYPE hk_CreateBuffer(ID3D11Device *d, const D3D11_BUFFER_DESC *bd, const D3D11_SUBRESOURCE_DATA *init,
                                                 ID3D11Buffer **out)
{
    void *ra = __builtin_return_address(0);
    if (bd && (bd->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd->ByteWidth >= 16 && from_hbao(ra)) {
        LONG n = (LONG)(bd->ByteWidth / 4);
        int have = 0;
        for (LONG i = 0; i < g_nhb && i < 4; i++) have |= g_hb[i] == n;
        if (!have && g_nhb < 4) {
            g_hb[g_nhb] = n;
            InterlockedIncrement(&g_nhb);
            LOG("gamedata: NVIDIA HBAO+ constant buffer - %ld floats (created by the HBAO+ library)", (long)n);
        }
    }
    return VT_ORIG(d, &h_cbuf, PFN_CB)(d, bd, init, out);
}

/* ---- Э3: экранные места буфера сцены — по именам переменных шейдеров игры (gamedata.c) ---- */
static unsigned g_slots[16];
static unsigned char g_exact[16];   /* 20.11: место только с точным значением экрана (центр экрана / позиция камеры) */
static int g_nslots;
static volatile LONG g_slots_done;

static void slots_find(void)
{
    if (InterlockedExchange(&g_slots_done, 1)) return;
    const Pak *p = gd_pak();
    int names = 0, n = p ? pak_screen_slots(p, g_slots, 16, &names, g_exact) : 0;
    char list[160] = "";
    for (int i = 0, k = 0; i < n && k < (int)sizeof list - 8; i++)
        k += snprintf(list + k, sizeof list - k, "%s%u%s", i ? ", " : "", g_slots[i], g_exact[i] ? " (exact)" : "");
    if (n >= 4 && names >= 6) {
        g_nslots = n;
        LOG("gamedata: screen numbers of the scene buffer at floats %s (%d shader variables found by name) - only these "
            "pairs are checked (keys=1: Ctrl+F6 in game - all floats / named places)", list, names);
    } else if (p) {
        LOG("gamedata: screen numbers of the scene buffer by name - only %d variables found (game update?) - every float "
            "is checked as before", names);
    }
}

const unsigned *anchors_screen_slots(int *n)
{
    *n = g_nslots;
    return g_nslots ? g_slots : NULL;
}

const unsigned char *anchors_screen_exact(void) { return g_nslots ? g_exact : NULL; }

void anchors_hook_device(ID3D11Device *dev)
{
    slots_find();   /* до первой записи в буферы — при создании устройства */
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateBuffer, hk_CreateBuffer, &h_cbuf);
}

int anchors_hbao(UINT floats)
{
    if (!g_nhb) return floats == 84;
    for (LONG i = 0; i < g_nhb && i < 4; i++)
        if (g_hb[i] == (LONG)floats) return 1;
    return 0;
}
