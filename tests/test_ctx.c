/* F02/F06: собранная d3d11.dll на WARP-устройстве.
 *   test_ctx.exe <папка с d3d11.dll и ui4k.ini> [--passthrough]
 * Проверяет: экспорты проброшены; контекст из CreateDevice == из GetImmediateContext == из QI (одна обёртка);
 * deferred-контекст через обёртку пишет в текстуру (FinishCommandList -> ExecuteCommandList);
 * второе устройство получает свою обёртку и не ломает первую; счётчики ссылок.
 * С --passthrough (enabled=0) — контекст настоящий, всё работает так же. Последняя строка PASS/FAIL. */
#define COBJMACROS
#include <stdio.h>
#include <windows.h>
#include <d3d11_4.h>

typedef HRESULT(WINAPI *PFN_CD)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static PFN_CD g_cd;
static HMODULE g_mod;

/* наша ли это обёртка: её vtable лежит внутри нашей DLL */
static int is_wrap(void *c)
{
    MEMORY_BASIC_INFORMATION mi;
    void *vt = *(void **)c;
    return VirtualQuery(vt, &mi, sizeof mi) && mi.AllocationBase == (void *)g_mod;
}

/* deferred-контекст очищает RT цветом, immediate исполняет список, читаем пиксель */
static UINT clear_via_deferred(ID3D11Device *dev, ID3D11DeviceContext *imm, const float col[4])
{
    D3D11_TEXTURE2D_DESC d = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *rt = NULL, *st = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext *def = NULL;
    ID3D11CommandList *cl = NULL;
    UINT px = 0xDEADBEEF;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &rt))) return px;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &st);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    HRESULT hr = ID3D11Device_CreateDeferredContext(dev, 0, &def);
    CHECK(SUCCEEDED(hr) && def, "CreateDeferredContext hr=0x%08lx", hr);
    if (def) {
        ID3D11DeviceContext_ClearRenderTargetView(def, rtv, col);
        hr = ID3D11DeviceContext_FinishCommandList(def, FALSE, &cl);
        CHECK(SUCCEEDED(hr) && cl, "FinishCommandList hr=0x%08lx", hr);
        if (cl) ID3D11DeviceContext_ExecuteCommandList(imm, cl, FALSE);
        ID3D11DeviceContext_CopyResource(imm, (ID3D11Resource *)st, (ID3D11Resource *)rt);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(imm, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            px = *(UINT *)m.pData;
            ID3D11DeviceContext_Unmap(imm, (ID3D11Resource *)st, 0);
        }
        if (cl) ID3D11CommandList_Release(cl);
        ULONG r = ID3D11DeviceContext_Release(def);
        CHECK(r == 0, "deferred context release -> %lu", r);
    }
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (st) ID3D11Texture2D_Release(st);
    if (rt) ID3D11Texture2D_Release(rt);
    return px;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_ctx <dir> [--passthrough]\nFAIL\n"); return 2; }
    int passthrough = argc > 2 && !strcmp(argv[2], "--passthrough");
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d11.dll", argv[1]);
    g_mod = LoadLibraryA(path);
    CHECK(g_mod != NULL, "LoadLibrary %s (err %lu)", path, GetLastError());
    if (!g_mod) { printf("FAIL\n"); return 1; }
    g_cd = (PFN_CD)GetProcAddress(g_mod, "D3D11CreateDevice");
    CHECK(g_cd != NULL, "no D3D11CreateDevice export");
    CHECK(GetProcAddress(g_mod, "D3D11CoreCreateDevice") != NULL, "forwarded export D3D11CoreCreateDevice missing");
    CHECK(GetProcAddress(g_mod, "D3D11On12CreateDevice") != NULL, "forwarded export D3D11On12CreateDevice missing");
    if (!g_cd) { printf("FAIL\n"); return 1; }

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = g_cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    CHECK(SUCCEEDED(hr) && dev && ctx, "WARP device hr=0x%08lx", hr);
    if (!dev || !ctx) { printf("FAIL\n"); return 1; }

    CHECK(is_wrap(ctx) == !passthrough, "immediate context wrapped=%d, expected %d", is_wrap(ctx), !passthrough);

    /* идентичность: GetImmediateContext[1] и QI возвращают ту же обёртку */
    ID3D11DeviceContext *c2 = NULL;
    ID3D11Device_GetImmediateContext(dev, &c2);
    CHECK(c2 == ctx, "GetImmediateContext %p != %p", c2, ctx);
    ID3D11Device1 *dev1 = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1))) {
        ID3D11DeviceContext1 *c1 = NULL;
        ID3D11Device1_GetImmediateContext1(dev1, &c1);
        CHECK((void *)c1 == (void *)ctx, "GetImmediateContext1 %p != %p", c1, ctx);
        if (c1) ID3D11DeviceContext1_Release(c1);
        ID3D11Device1_Release(dev1);
    }
    ID3D11DeviceContext4 *c4 = NULL;
    hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext4, (void **)&c4);
    CHECK(SUCCEEDED(hr) && (void *)c4 == (void *)ctx, "QI ctx4 %p != %p", c4, ctx);
    if (c4) ID3D11DeviceContext4_Release(c4);
    ID3D11Device *gd = NULL;
    ID3D11DeviceContext_GetDevice(ctx, &gd);
    CHECK(gd == dev, "GetDevice through thunk %p != %p", gd, dev);
    if (gd) ID3D11Device_Release(gd);
    if (!passthrough) {
        ULONG r = ID3D11DeviceContext_AddRef(ctx);
        CHECK(r == 3, "wrapper refcount after AddRef = %lu, expected 3 (create + GetImmediateContext + AddRef)", r);
        ID3D11DeviceContext_Release(ctx);
    }
    ID3D11DeviceContext_Release(c2);

    /* deferred -> execute через обёртки */
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
    UINT px = clear_via_deferred(dev, ctx, red);
    CHECK(px == 0xFF0000FF, "deferred clear pixel 0x%08X, want 0xFF0000FF", px);

    /* второе устройство: своя обёртка, первая не ломается */
    ID3D11Device *devB = NULL;
    ID3D11DeviceContext *ctxB = NULL;
    hr = g_cd(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &devB, NULL, &ctxB);
    CHECK(SUCCEEDED(hr) && ctxB && ctxB != ctx, "second device ctx %p (first %p)", ctxB, ctx);
    ID3D11DeviceContext *c3 = NULL;
    ID3D11Device_GetImmediateContext(dev, &c3);
    CHECK(c3 == ctx, "first device lost its wrapper after second device: %p != %p", c3, ctx);
    if (c3) ID3D11DeviceContext_Release(c3);
    if (ctxB) {
        px = clear_via_deferred(devB, ctxB, green);
        CHECK(px == 0xFF00FF00, "second device deferred clear 0x%08X", px);
        ID3D11DeviceContext_Release(ctxB);
        ID3D11Device_Release(devB);
    }

    /* обёртку отпустили целиком, потом снова попросили — новая, рабочая */
    ULONG r = ID3D11DeviceContext_Release(ctx);
    CHECK(r == 0, "last release of immediate wrapper -> %lu", r);
    ctx = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    CHECK(ctx && is_wrap(ctx) == !passthrough, "re-acquired immediate context");
    if (ctx) {
        px = clear_via_deferred(dev, ctx, green);
        CHECK(px == 0xFF00FF00, "after re-wrap deferred clear 0x%08X", px);
        ID3D11DeviceContext_Release(ctx);
    }
    ULONG dr = ID3D11Device_Release(dev);
    CHECK(dr == 0, "device release -> %lu (leaked references)", dr);

    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
