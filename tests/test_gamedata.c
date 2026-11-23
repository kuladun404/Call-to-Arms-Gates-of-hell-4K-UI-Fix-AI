/* Устойчивость к обновлениям игры: разбор архива шейдеров (gamedata.c).
 *   test_gamedata.exe <поддельный архив> [<настоящий resource\shader.pak игры>]
 * Поддельный архив пишет build.py: shader_constants.h с pscSceneGroup 40, кэш-записи под именами VS интерфейса с новым
 * байткодом (как после обновления игры), запись с чужим именем внутри (защита), сжатая запись (не читается).
 * С настоящим архивом: размер буфера сцены 39 групп, все 9 имён VS интерфейса — два отпечатка 16.11, шейдеры
 * автоэкспозиции и размытия находятся по именам. Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "gamedata.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)l);
    *n = fread(b, 1, (size_t)l, f);
    fclose(f);
    return b;
}

static const char *const UI[] = {
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage\nFAIL\n"); return 2; }
    size_t n;
    unsigned char *b = load(argv[1], &n);
    Pak p;
    CHECK(b && pak_parse(b, n, &p), "fake archive not parsed");
    if (b) {
        CHECK(pak_scene_groups(&p) == 40, "scene groups %d, want 40", pak_scene_groups(&p));
        /* Э3: после обновления регистры переехали — места считаются заново по именам */
        unsigned sl[16];
        int names = 0, ns = pak_screen_slots(&p, sl, 16, &names, NULL);
        CHECK(ns == 3 && names == 3 && sl[0] == 8 && sl[1] == 22 && sl[2] == 73,
              "screen places by name in the updated archive: %d (%u %u %u), names %d", ns, ns > 0 ? sl[0] : 0,
              ns > 1 ? sl[1] : 0, ns > 2 ? sl[2] : 0, names);
        size_t len = 0;
        const unsigned char *c = pak_shader(&p, UI[0], &len);
        CHECK(c && len == 32 && !memcmp(c + 4, "NEWSHADER-UPDATE", 16), "updated interface VS not found by name");
        c = pak_shader(&p, "VS[VS]$vcolor$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR", &len);
        CHECK(c != NULL, "name case must not matter (the cache key is lower case)");
        CHECK(!pak_shader(&p, UI[2], NULL), "entry with another name inside was accepted");
        CHECK(!pak_shader(&p, UI[3], NULL), "compressed entry was accepted");
        CHECK(!pak_shader(&p, "vs[VS]$NOPE", NULL), "missing name found");
        CHECK(pak_find(&p, "shader/dx10/shader_constants.h") != NULL, "source file not found");
        /* обрезанный архив: каждая длина — без падения */
        for (size_t l = 0; l < n; l += 7) {
            Pak q;
            if (pak_parse(b, l, &q)) { pak_shader(&q, UI[0], NULL); pak_scene_groups(&q); pak_free(&q); }
        }
        pak_free(&p);
        printf("fake archive (game after an update): new interface VS found by name, scene buffer 40 groups\n");
    }
    if (argc > 2) {
        size_t rn;
        unsigned char *r = load(argv[2], &rn);
        Pak g;
        if (!r) { printf("real archive %s not readable - skipped\n", argv[2]); }
        else {
            CHECK(pak_parse(r, rn, &g), "real archive not parsed");
            CHECK(pak_scene_groups(&g) == 39, "real scene groups %d, want 39", pak_scene_groups(&g));
            unsigned rs[16];
            unsigned char rx[16] = {0};
            int rn2 = 0, rns = pak_screen_slots(&g, rs, 16, &rn2, rx);
            static const unsigned want[] = {0, 4, 6, 12, 56, 70, 97, 150};
            int same = rns == 8;
            for (int i = 0; same && i < 8; i++) same = rs[i] == want[i];
            printf("real archive: screen places of the scene buffer %d (names %d):", rns, rn2);
            for (int i = 0; i < rns; i++) printf(" %u", rs[i]);
            printf("\n");
            CHECK(same && rn2 == 11, "real screen places = 0 4 6 12 56 70 97 150 from 11 names (18.11 game)");
            /* 20.11: место 12 (центр экрана; в мировых проходах — позиция камеры) — только точное значение, прочие — с допуском */
            int ex_ok = same;
            for (int i = 0; ex_ok && i < 8; i++) ex_ok = rx[i] == (rs[i] == 12);
            CHECK(ex_ok, "only place 12 (screen centre / camera position) asks for an exact value");
            int found = 0, a = 0, bb = 0;
            for (int i = 0; i < 9; i++) {
                size_t len;
                const unsigned char *c = pak_shader(&g, UI[i], &len);
                if (!c) continue;
                found++;
                static const unsigned char s1[8] = {0xb7, 0xe2, 0x4d, 0x53, 0x5a, 0x3a, 0x5d, 0x52};
                static const unsigned char s2[8] = {0xce, 0x3d, 0x9b, 0xfe, 0xa0, 0xd2, 0xc0, 0x76};
                a += !memcmp(c + 4, s1, 8);
                bb += !memcmp(c + 4, s2, 8);
            }
            printf("real archive: %d files, scene buffer %d groups, interface VS names %d/9 (checksum 1: %d, checksum 2: %d)\n", g.n,
                   pak_scene_groups(&g), found, a, bb);
            CHECK(found == 9 && a + bb == 9 && a && bb, "interface VS names do not give the two checksums of 16.11");
            CHECK(pak_shader(&g, "postprocess/histogram[HistogramBinsCS]", NULL) &&
                  pak_shader(&g, "postprocess/histogram[HistogramMaxAvg_Init_CS]", NULL), "exposure shaders not found by name");
            CHECK(pak_shader(&g, "postprocess/dof/dof_blur[blurPS]", NULL), "DOF blur shader not found by name");
            CHECK(pak_find(&g, "shader/dx10/postprocess/histogram.hlsl") != NULL, "exposure source not found");
            pak_free(&g);
            free(r);
        }
    }
    free(b);

    /* 23.11: кэш, собранный игрой (у игрока с модами в архиве только исходники): папка, файл на шейдер */
    {
        wchar_t dir[MAX_PATH], tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wsprintfW(dir, L"%sui4k_ucache_%lu", tmp, GetCurrentProcessId());
        CreateDirectoryW(dir, NULL);
        char hex[33];
        shader_cache_hex(UI[3], hex);
        CHECK(strlen(hex) == 32, "cache file name");
        /* запись как у игры: байт длины, имя (регистр как есть), DXBC */
        unsigned char rec[512];
        size_t l = strlen(UI[3]);
        rec[0] = (unsigned char)l;
        memcpy(rec + 1, UI[3], l);
        unsigned char dx[32] = "DXBC" "COMPILED-BY-GAME";
        dx[24] = 32;   /* длина байткода */
        memcpy(rec + 1 + l, dx, 32);
        wchar_t f[MAX_PATH];
        wsprintfW(f, L"%s\\%S.vso", dir, hex);
        HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w = 0;
        WriteFile(h, rec, (DWORD)(1 + l + 32), &w, NULL);
        CloseHandle(h);
        /* вычислительный шейдер — для обратного поиска по байткоду */
        const char *cs = "postprocess/histogram[HistogramBinsCS]";
        unsigned char csdx[40] = "DXBC" "COMPUTE-SHADER-X";
        csdx[24] = 40;
        shader_cache_hex(cs, hex);
        wsprintfW(f, L"%s\\%S.cso", dir, hex);
        l = strlen(cs);
        rec[0] = (unsigned char)l;
        memcpy(rec + 1, cs, l);
        memcpy(rec + 1 + l, csdx, 40);
        h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        WriteFile(h, rec, (DWORD)(1 + l + 40), &w, NULL);
        CloseHandle(h);

        size_t len = 0;
        const unsigned char *c = dir_shader(dir, UI[3], &len);
        CHECK(c && len == 32 && !memcmp(c + 4, "COMPILED-BY-GAME", 16), "interface VS not found in the game-compiled cache folder");
        c = dir_shader(dir, "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$use_vcolor", &len);   /* регистр имени не важен */
        CHECK(c != NULL, "name lookup in the cache folder must ignore case");
        CHECK(dir_shader(dir, UI[0], &len) == NULL, "a shader that is not in the folder must not be found");
        char nm[300] = "";
        CHECK(dir_cs_name(dir, csdx, 40, nm, sizeof nm) && !strcmp(nm, cs), "compute shader name by bytecode: '%s'", nm);
        unsigned char other[40];
        memcpy(other, csdx, 40);
        other[10] ^= 1;
        CHECK(!dir_cs_name(dir, other, 40, nm, sizeof nm), "different bytecode must not match");
        CHECK(dir_shader(L"", UI[3], &len) == NULL && dir_shader(NULL, UI[3], &len) == NULL, "empty folder path");

        wchar_t pat[MAX_PATH];
        wsprintfW(pat, L"%s\\*", dir);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(pat, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.') continue;
                wsprintfW(f, L"%s\\%s", dir, fd.cFileName);
                DeleteFileW(f);
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        RemoveDirectoryW(dir);
    }

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
