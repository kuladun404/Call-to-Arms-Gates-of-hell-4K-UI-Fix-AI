/* Э9: классы игры по именам RTTI MSVC и места в коде (src/rtti.c) — на искусственном образе: описатель типа, локатор,
 * таблица, объект в данных, обработчик с проверкой байта-запрета. Подделки (чужое смещение, локатор без ссылки на себя,
 * имя-продолжение, два одинаковых места) не должны находиться. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "../src/rtti.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char img[0x6000];
static void w32(unsigned o, unsigned v) { memcpy(img + o, &v, 4); }
static void w64(unsigned o, unsigned long long v) { memcpy(img + o, &v, 8); }
static void col(unsigned o, unsigned off, unsigned td, unsigned self) { w32(o, 1); w32(o + 4, off); w32(o + 8, 0); w32(o + 12, td); w32(o + 16, 0x1f00); w32(o + 20, self); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const unsigned long long base = 0x140000000ULL;
    ImgSec s[3] = {{0x1000, 0x1000, 0, 0}, {0x2000, 0x2000, 1, 0}, {0x4000, 0x1000, 0, 1}};   /* rdata, data, text */
    const char *tm = ".?AVeTargetManager@Render@@", *vm = ".?AVeViewManager@Stuff@@";
    /* описатели типов в данных: имя на +16 */
    memcpy(img + 0x2110, tm, strlen(tm) + 1);
    memcpy(img + 0x2210, vm, strlen(vm) + 1);
    memcpy(img + 0x2310, ".?AVeTargetManager@Render@@X", 29);   /* имя-продолжение — не наше */
    col(0x1200, 0, 0x2100, 0x1200);   /* главный локатор eTargetManager */
    col(0x1240, 8, 0x2100, 0x1240);   /* второй базы (смещение 8) — не главная таблица */
    col(0x1280, 0, 0x2100, 0x1290);   /* без ссылки на себя — подделка */
    col(0x12c0, 0, 0x2200, 0x12c0);   /* eViewManager */
    w64(0x1400, base + 0x1200);       /* таблица eTargetManager: 0x1408 */
    w64(0x1500, base + 0x12c0);       /* таблица eViewManager: 0x1508 */
    w64(0x1600, base + 0x1240);       /* таблица второй базы */
    w64(0x3000, base + 0x1408);       /* объект eTargetManager в данных */
    w64(0x3100, base + 0x1508);       /* объект eViewManager */
    w64(0x3108, base + 0x1508);       /* и ещё ячейка с тем же числом */

    unsigned v1 = rtti_vtable(img, base, s, 3, tm), v2 = rtti_vtable(img, base, s, 3, vm);
    printf("vtables: eTargetManager 0x%x, eViewManager 0x%x\n", v1, v2);
    CHECK(v1 == 0x1408, "eTargetManager vtable 0x%x, expected 0x1408", v1);
    CHECK(v2 == 0x1508, "eViewManager vtable 0x%x, expected 0x1508", v2);
    CHECK(!rtti_vtable(img, base, s, 3, ".?AVeTarget@Render@@"), "a class without a type descriptor found");
    unsigned at[4];
    int n = img_find_qword(img, s, 3, base + v1, at, 4);
    CHECK(n == 1 && at[0] == 0x3000, "eTargetManager objects %d (0x%x)", n, n ? at[0] : 0);
    n = img_find_qword(img, s, 3, base + v2, at, 4);
    CHECK(n == 2 && at[0] == 0x3100 && at[1] == 0x3108, "eViewManager cells %d", n);
    /* два описателя с одним именем — неоднозначно */
    memcpy(img + 0x2410, vm, strlen(vm) + 1);
    CHECK(!rtti_vtable(img, base, s, 3, vm), "two type descriptors with one name accepted");
    memset(img + 0x2410, 0, 32);
    /* две таблицы с одним локатором — неоднозначно */
    w64(0x1700, base + 0x1200);
    CHECK(!rtti_vtable(img, base, s, 3, tm), "two vtables of one locator accepted");
    w64(0x1700, 0);

    /* обработчик: sub ecx,0x40f ... cmp byte ptr [rip+rel32],0 — байт-запрет в данных 0x2500 */
    static const unsigned char sub40f[] = {0x81, 0xE9, 0x0F, 0x04, 0x00, 0x00};
    memcpy(img + 0x4000, "\x48\x89\x5c\x24\x08", 5);
    memcpy(img + 0x400e, sub40f, sizeof sub40f);
    img[0x4040] = 0x80; img[0x4041] = 0x3D; w32(0x4042, (unsigned)(0x2500 - (0x4040 + 7))); img[0x4046] = 0;
    CHECK(code_find(img + 0x4000, 0x40, sub40f, sizeof sub40f) == 0xe, "sub ecx,0x40f not found at +0xe");
    unsigned g = code_rip_cmp0(img + 0x4000, 0x100, 0x4000);
    printf("gate byte: 0x%x\n", g);
    CHECK(g == 0x2500, "gate byte 0x%x, expected 0x2500", g);
    img[0x4080] = 0x80; img[0x4081] = 0x3D; w32(0x4082, 0x100); img[0x4086] = 0;
    CHECK(!code_rip_cmp0(img + 0x4000, 0x100, 0x4000), "two byte checks accepted");
    CHECK(code_find(img + 0x4000, 4, sub40f, sizeof sub40f) == -1, "pattern longer than the code found");

    /* секции настоящего образа (этой программы) */
    ImgSec own[32];
    int ns = img_sections((const unsigned char *)GetModuleHandleW(NULL), own, 32), ex = 0, wr = 0;
    for (int i = 0; i < ns; i++) { ex |= own[i].exec; wr |= own[i].write; }
    printf("own image: %d sections\n", ns);
    CHECK(ns >= 2 && ex && wr, "own image sections %d (exec %d, write %d)", ns, ex, wr);
    CHECK(!img_sections(img, own, 32), "not a PE image accepted");
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
