/* Чтение отражения шейдера DXBC (часть RDEF). Чистые функции — тестируются отдельно (tests/test_dxbc.c). */
#pragma once
#include <stddef.h>

typedef struct {
    unsigned slot;       /* регистр буфера констант: bN */
    unsigned cb_bytes;   /* размер буфера, как его объявил шейдер (не больше настоящего буфера игры) */
    unsigned offset;     /* смещение переменной в буфере, байты */
    unsigned size;       /* размер переменной, байты */
} DxbcVar;

/* Переменная буфера констант по имени. 0 — у шейдера нет отражения, нет такой переменной или байткод битый. */
int dxbc_find_var(const void *code, size_t len, const char *name, DxbcVar *out);

/* Привязка ресурса по имени (текстура, UAV, буфер...): регистр; -1 — нет такой или нет отражения. */
int dxbc_find_binding(const void *code, size_t len, const char *name);

/* Каждая привязка ресурса a (имя, тип, регистр) есть и в b. 1 — да (b может иметь и свои). Для пересобранных шейдеров:
 * игра привязывает ресурсы по регистрам — пересборка не должна их сдвинуть. */
int dxbc_same_bindings(const void *a, size_t an, const void *b, size_t bn);
