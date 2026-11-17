/* Форматы пикселей и пересчёт картинки процессором. Чистые функции — тестируются отдельно (tests/test_fmt.c). */
#pragma once
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

/* байт на точку; 0 — формат сжатый или неизвестный (пересчитать процессором нельзя) */
UINT fmt_bytes(DXGI_FORMAT f);
/* 1 — каждый байт точки есть свой 8-битный нормализованный канал: можно смешивать соседние точки побайтно */
int fmt_lerp8(DXGI_FORMAT f);
/* формат видов (SRV и RTV) для растяжения шейдером из текстуры формата a в текстуру формата b; DXGI_FORMAT_UNKNOWN —
 * нельзя (разные форматы, целочисленные, глубина). Typeless сходится с типизированным своего семейства. */
DXGI_FORMAT fmt_view(DXGI_FORMAT a, DXGI_FORMAT b);

/* src: sw x sh точек по bpp байт, шаг строки spitch — читается ровно sw*bpp байт каждой из sh строк, ни байтом больше;
 * dst: dw x dh, шаг dpitch. lerp8 — плавно (fmt_lerp8), иначе ближайшая точка. */
void px_resample(const BYTE *src, UINT sw, UINT sh, UINT spitch, BYTE *dst, UINT dw, UINT dh, UINT dpitch, UINT bpp,
                 int lerp8);
