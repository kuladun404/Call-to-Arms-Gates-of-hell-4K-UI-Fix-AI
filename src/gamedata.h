/* Что мод знает об устройстве игры — узнаётся при запуске у самой игры, из её архива шейдеров resource\shader.pak
 * (zip без сжатия): исходники (shader/dx10/...) и кэш скомпилированных шейдеров. Имя файла в кэше — MD5 от имени шейдера
 * «файл[функция]$настройки» в нижнем регистре (shader/cache/dx11.0/<md5>.vso|pso|cso|...), внутри — байт длины, то же имя
 * и байткод DXBC. После обновления игры имена те же, байткод новый — мод находит его снова. Только чтение. */
#pragma once
#include <stddef.h>

/* Разбор архива в памяти (чистые функции — тесты). 1 — это zip, записи разобраны. */
typedef struct { const char *name; unsigned nlen; size_t off, size; } PakEnt;
typedef struct { const unsigned char *base; size_t size; PakEnt *e; int n; } Pak;
int  pak_parse(const unsigned char *buf, size_t len, Pak *p);   /* p->e — HeapAlloc, освобождать pak_free */
void pak_free(Pak *p);
const PakEnt *pak_find(const Pak *p, const char *path);          /* точное имя (регистр не важен) */
/* шейдер из кэша по имени: байткод DXBC и его длина; NULL — нет */
const unsigned char *pak_shader(const Pak *p, const char *name, size_t *len);
/* обратный поиск: имя шейдера в кэше по его байткоду (только записи вычислительных шейдеров .cso); 1 — найдено */
int pak_cs_name(const Pak *p, const void *code, size_t n, char *name, size_t cap);
/* число групп буфера сцены пиксельных шейдеров из shader_constants.h («#define pscSceneGroup _PSG(pscgScene, 39)»);
 * 0 — не нашлось */
int pak_scene_groups(const Pak *p);
/* Э3: где в буфере сцены лежат пары чисел, зависящие от размера экрана, — по именам переменных в исходниках шейдеров
 * игры (packoffset; регистр — число cN или имя из shader_constants.h). out — номер первого числа пары, без повторов,
 * по возрастанию; возврат — сколько мест; names — сколько имён из таблицы мода нашлось в исходниках. */
int pak_screen_slots(const Pak *p, unsigned *out, int max, int *names, unsigned char *exact);
/*   exact (может быть NULL) — по месту на каждое out[]: 1 — только точное совпадение со значением экрана (место, где в
 *   других проходах лежат обычные числа, например позиция камеры), 0 — с допуском ±1 пиксель малой картинки */

/* Кэш, который игра собирает сама (Documents\my games\gates of hell\shader_cache\dx11.0, «Реверс»/Шейдеры): чего нет в
 * архиве, игра компилирует при запуске и кладёт сюда — файл на шейдер, имя и содержимое как у записей кэша в архиве.
 * 23.11: у игрока с модами в resource\shader.pak были только исходники (92 файла), весь кэш — здесь. */
void shader_cache_hex(const char *name, char hex[33]);   /* «<md5 имени в нижнем регистре>», "" — не вышло */
/* шейдер из этой папки по имени: байткод (память HeapAlloc, держится до конца работы) и длина; NULL — нет */
const unsigned char *dir_shader(const wchar_t *dir, const char *name, size_t *len);
/* обратный поиск по байткоду среди вычислительных шейдеров (.cso) этой папки; 1 — найдено */
int dir_cs_name(const wchar_t *dir, const void *code, size_t n, char *name, size_t cap);

/* ---- архив игры (DLL): открывается один раз, при первой надобности ---- */
const Pak *gd_pak(void);   /* NULL — архива нет или он не разобран */
/* шейдер по имени: сначала архив, потом кэш, собранный игрой; from — 1 архив, 2 кэш игры */
const unsigned char *gd_shader(const char *name, size_t *len, int *from);
int gd_cs_name(const void *code, size_t n, char *name, size_t cap);   /* то же для обратного поиска (.cso) */
