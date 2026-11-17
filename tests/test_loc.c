/* L8: подмешивание пакета подписей (loc.c).
 *   test_loc.exe <папка с d3d11.dll и ui4k.ini>
 * После загрузки DLL перебор <папка>\localizations\*.* должен дать настоящий a.pak и в конце ui4k.pak;
 * открытие ui4k.pak — наш zip с ru/interface/text/ui4k.po и подписями строк (presets, нужен fit_native=1). Последняя строка PASS/FAIL. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage\nFAIL\n"); return 2; }
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\d3d11.dll", argv[1]);
    if (!LoadLibraryA(p)) { printf("LoadLibrary\nFAIL\n"); return 1; }
    WCHAR dir[MAX_PATH], w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
    swprintf(w, MAX_PATH, L"%ls\\localizations", dir);
    CreateDirectoryW(w, NULL);
    swprintf(w, MAX_PATH, L"%ls\\localizations\\a.pak", dir);
    HANDLE f = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CloseHandle(f);

    /* игра пишет пути через '/' */
    swprintf(w, MAX_PATH, L"%ls/localizations/*.*", dir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(w, &d);
    int real = 0, ours = 0, after = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(d.cFileName, L"a.pak")) { real++; if (ours) after++; }
            if (!wcscmp(d.cFileName, L"ui4k.pak")) { ours++; CHECK(d.nFileSizeLow > 100, "size of ui4k.pak %lu", d.nFileSizeLow); }
        } while (FindNextFileW(h, &d));
        CHECK(GetLastError() == ERROR_NO_MORE_FILES, "scan ends with ERROR_NO_MORE_FILES (%lu)", GetLastError());
        FindClose(h);
    }
    CHECK(real == 1 && ours == 1 && !after, "scan: a.pak %d, ui4k.pak %d (last)", real, ours);
    /* второй перебор — снова ровно один */
    h = FindFirstFileW(w, &d);
    ours = 0;
    if (h != INVALID_HANDLE_VALUE) { do ours += !wcscmp(d.cFileName, L"ui4k.pak"); while (FindNextFileW(h, &d)); FindClose(h); }
    CHECK(ours == 1, "second scan: ui4k.pak %d", ours);

    swprintf(w, MAX_PATH, L"%ls/localizations/ui4k.pak", dir);
    CHECK(GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES, "attributes of virtual ui4k.pak");
    f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE, "open virtual ui4k.pak");
    static char buf[65536];
    DWORD n = 0;
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, buf, sizeof buf - 1, &n, NULL); CloseHandle(f); }
    buf[n] = 0;
    CHECK(n > 22 && !memcmp(buf, "PK\x03\x04", 4) && !memcmp(buf + n - 22, "PK\x05\x06", 4), "zip signatures");
    int has_name = 0, has_key = 0, has_text = 0;
    for (DWORD i = 0; i + 40 < n; i++) {
        if (!memcmp(buf + i, "ru/interface/text/ui4k.po", 25)) has_name = 1;
        if (!memcmp(buf + i, "options/video/resolution#/", 26)) has_key = 1;
        if (!memcmp(buf + i, "\xC3\x97", 2)) has_text = 1;
    }
    CHECK(has_name && has_key && has_text, "po inside: name %d key %d text %d", has_name, has_key, has_text);
    /* настоящий файл с тем же именем в другой папке не трогаем */
    swprintf(w, MAX_PATH, L"%ls/other/ui4k.pak", dir);
    CHECK(GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES, "only localizations/ui4k.pak is virtual");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
