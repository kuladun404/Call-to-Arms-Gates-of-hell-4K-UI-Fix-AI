#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include "scale.h"

typedef enum { TEX_KEEP = 0, TEX_SCREEN = 1, TEX_UI_RT = 2 } TexClass;

/* lw, lh — логический экран; 0 = ещё неизвестен */
TexClass tex_classify(const D3D11_TEXTURE2D_DESC *d, int has_init, UINT lw, UINT lh, Ratio k);
UINT tex_full_mips(UINT w, UINT h);
D3D11_TEXTURE2D_DESC tex_phys_desc(const D3D11_TEXTURE2D_DESC *d, Ratio k);
