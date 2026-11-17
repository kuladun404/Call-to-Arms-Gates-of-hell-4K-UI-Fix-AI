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

/* ---- архив игры (DLL): открывается один раз, при первой надобности ---- */
const Pak *gd_pak(void);   /* NULL — архива нет или он не разобран */
