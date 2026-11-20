/* Хуки устройства: контексты, которые устройство отдаёт игре, заменяются обёртками (F06).
 * Сюда же позже: CreateTexture2D (F05), Create*View, swapchain через фабрику (F04). */
#include "common.h"

static VtHook h_cdc, h_cdc1, h_cdc2, h_cdc3, h_gic, h_gic1, h_gic2, h_gic3;
typedef HRESULT(STDMETHODCALLTYPE *PFN_CDC)(void *, UINT, void **);
typedef void(STDMETHODCALLTYPE *PFN_GIC)(void *, void **);

#define DEF_CDC(name, hk)                                                                \
    static HRESULT STDMETHODCALLTYPE name(void *dev, UINT f, void **out)                 \
    {                                                                                    \
        HRESULT hr = VT_ORIG(dev, &hk, PFN_CDC)(dev, f, out);                            \
        if (SUCCEEDED(hr) && out && *out) *out = ctx_wrap((ID3D11DeviceContext *)*out);  \
        return hr;                                                                       \
    }
DEF_CDC(hk_CreateDeferredContext, h_cdc)
DEF_CDC(hk_CreateDeferredContext1, h_cdc1)
DEF_CDC(hk_CreateDeferredContext2, h_cdc2)
DEF_CDC(hk_CreateDeferredContext3, h_cdc3)

#define DEF_GIC(name, hk)                                                                \
    static void STDMETHODCALLTYPE name(void *dev, void **out)                            \
    {                                                                                    \
        VT_ORIG(dev, &hk, PFN_GIC)(dev, out);                                            \
        if (out && *out) *out = ctx_wrap((ID3D11DeviceContext *)*out);                   \
    }
DEF_GIC(hk_GetImmediateContext, h_gic)
DEF_GIC(hk_GetImmediateContext1, h_gic1)
DEF_GIC(hk_GetImmediateContext2, h_gic2)
DEF_GIC(hk_GetImmediateContext3, h_gic3)

void device_hook(ID3D11Device *dev)
{
    res_hook_device(dev);
    res_origin_init();                         /* Э2: «t:create» в exe — один раз */
    fontpages_hook_device(dev);                /* Э6: виды рабочих страниц шрифтов */
    anchors_hook_device(dev);                  /* буферы констант библиотеки HBAO+ — их размер */
    lum_hook_device(dev);
    world_hook_device(dev);                    /* раньше diag: diag видит шейдер игры, а не подмену */
    dof_hook_device(dev);                      /* Г4: шейдер размытия глубины резкости — по имени переменной */
    box_hook_device(dev);                      /* Э8: шейдер копирования с шагом one_pixel — по именам переменных */
    if (g_cfg.diag) diag_hook_device(dev);
    VT_PATCH(dev, ID3D11DeviceVtbl, CreateDeferredContext, hk_CreateDeferredContext, &h_cdc);
    VT_PATCH(dev, ID3D11DeviceVtbl, GetImmediateContext, hk_GetImmediateContext, &h_gic);
    void *d;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, &d))) {
        VT_PATCH(d, ID3D11Device1Vtbl, GetImmediateContext1, hk_GetImmediateContext1, &h_gic1);
        VT_PATCH(d, ID3D11Device1Vtbl, CreateDeferredContext1, hk_CreateDeferredContext1, &h_cdc1);
        IUnknown_Release((IUnknown *)d);
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device2, &d))) {
        VT_PATCH(d, ID3D11Device2Vtbl, GetImmediateContext2, hk_GetImmediateContext2, &h_gic2);
        VT_PATCH(d, ID3D11Device2Vtbl, CreateDeferredContext2, hk_CreateDeferredContext2, &h_cdc2);
        IUnknown_Release((IUnknown *)d);
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device3, &d))) {
        VT_PATCH(d, ID3D11Device3Vtbl, GetImmediateContext3, hk_GetImmediateContext3, &h_gic3);
        VT_PATCH(d, ID3D11Device3Vtbl, CreateDeferredContext3, hk_CreateDeferredContext3, &h_cdc3);
        IUnknown_Release((IUnknown *)d);
    }
    LOG("device %p hooked", dev);
}
