/* Э2 (план v3): кэши окон по происхождению. Чистые функции — тестируются отдельно (tests/test_origin.c). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ORIGIN_MAX 4
typedef struct { DWORD begin, end; } OriginRange;   /* функция exe: [begin, end) в RVA */

/* Образ exe в памяти (секции по своим RVA). Строка label (целиком, с нулём, с начала строки) в секциях данных ->
 * код с LEA/MOV по RIP на неё -> функция по таблице раскрутки (.pdata, с цепочками). Возвращает число разных функций
 * (не больше max); str_rva — где первая найденная строка (0 — нет), nrefs — сколько ссылок на неё в коде. */
int origin_find(const BYTE *img, SIZE_T img_size, const char *label, OriginRange *out, int max, DWORD *str_rva, int *nrefs);

/* Стек текущего вызова проходит через одну из функций образа img: смотрим depth возвратов (без своего кадра). */
int origin_on_stack(const BYTE *img, const OriginRange *r, int n, int depth);

/* Все части функции f (по .pdata: её запись и записи-продолжения с UNW_FLAG_CHAININFO, которые ведут к ней). MSVC выносит
 * тело большой функции в такие части — origin_find отдаёт только основную (19.11: 0x6de9f0 «video/water_quality» —
 * пролог 0x6de9f0..0x6dea05, вызов отражения в 0x6dea05..0x6debfd). Добавляет в out[*n..max), возвращает сколько добавлено. */
int origin_parts(const BYTE *img, SIZE_T img_size, const OriginRange *f, OriginRange *out, int *n, int max);

/* Возвраты fr[0..n) (от внутреннего к внешнему): 0 — ни одного в функциях a; 1 — есть; 2 — есть, и не дальше within
 * возвратов выше него — функция из b (кто позвал a). Чистая — для тестов. */
int origin_chain(void *const *fr, int n, const BYTE *img, const OriginRange *a, int na, const OriginRange *b, int nb, int within);

/* то же для стека текущего вызова (depth возвратов без своего кадра) */
int origin_stack(const BYTE *img, const OriginRange *a, int na, const OriginRange *b, int nb, int depth, int within);
