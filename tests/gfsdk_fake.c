/* Поддельная библиотека NVIDIA HBAO+ для test_update: буфер констант создаёт она сама — по имени модуля
 * (GFSDK_SSAO*) мод узнаёт размер её буферов. Собирается без оптимизации: вызов CreateBuffer должен идти из этой DLL,
 * а не хвостом из вызывающего. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

__declspec(dllexport) HRESULT make_cb(ID3D11Device *d, UINT bytes, ID3D11Buffer **out)
{
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    volatile HRESULT hr = ID3D11Device_CreateBuffer(d, &bd, NULL, out);
    return hr;
}
