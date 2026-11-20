/* Э9: поиск в загруженном образе exe — классы по именам RTTI MSVC, их объекты, места в коде. Чистые функции над памятью
 * образа (RVA = смещение от начала) — тестируются отдельно (tests/test_rtti.c). */
#pragma once
#include <stddef.h>

typedef struct { unsigned rva, size; int write, exec; } ImgSec;

/* секции загруженного образа PE (img — его начало); сколько записано */
int img_sections(const unsigned char *img, ImgSec *out, int max);

/* RVA главной таблицы виртуальных функций класса по имени типа RTTI (".?AVeViewManager@Stuff@@"): описатель типа ->
 * локатор объекта (подпись 1, смещение 0, ссылка на себя) -> таблица (перед ней — адрес локатора). 0 — нет или не одна. */
unsigned rtti_vtable(const unsigned char *img, unsigned long long base, const ImgSec *s, int ns, const char *name);

/* RVA 8-байтных ячеек в записываемых секциях, равных va (объекты класса: в начале — адрес таблицы); сколько найдено */
int img_find_qword(const unsigned char *img, const ImgSec *s, int ns, unsigned long long va, unsigned *out, int max);

/* смещение образца в code[0..n); -1 — нет */
long code_find(const unsigned char *code, size_t n, const unsigned char *pat, size_t pn);

/* единственное «cmp byte ptr [rip+rel32], 0» (80 3D rel32 00) в code[0..n): RVA байта, который оно читает
 * (code_rva — RVA начала code). 0 — нет или больше одного. */
unsigned code_rip_cmp0(const unsigned char *code, size_t n, unsigned code_rva);
