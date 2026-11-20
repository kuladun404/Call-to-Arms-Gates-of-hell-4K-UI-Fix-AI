/* Мир без HDR (world.c) через собранную DLL на WARP.
 *   test_world.exe <папка с d3d11.dll>      (ui4k.ini тест пишет сам: нужен VS интерфейса — его контрольная сумма)
 * Кадр 1: «мир» (красный, тест глубины) рисуется прямо в back buffer — мод должен заметить (HDR выключен).
 * Кадр 2: мир снова в back buffer -> должен уйти в свою цель мира; первое рисование интерфейса (зелёный квадрат
 * слева сверху, VS интерфейса, без глубины) — мир растягивается на весь экран, интерфейс поверх.
 * Проверки: лог (свой мир нужного размера, переход на интерфейсе), пиксели back buffer (угол справа снизу красный,
 * квадрат зелёный). Последняя строка PASS/FAIL.
 * Варианты (второй аргумент): hdr0 — HDR выключен в настройках игры; msaa — к тому же в игре включён MSAA x4 (экран и
 * глубина многовыборочные): своя цель мира — с тем же числом выборок, мир остаётся в размере строки (П7), глубина
 * работает; msaa_live — MSAA включают на ходу, как игра: новый swapchain с выборками через IDXGIFactory (Г1: размер
 * мира при этом не меняется, цель мира пересоздаётся с выборками); msaa_safe — MSAA при msaa_safe=1: безопасный
 * режим, мир в размере интерфейса; nopresets — без строк списка (presets=0, render=50): мир в своей цели в каждом
 * кадре, а не только в первом (С4).
 * resolve — как hdr-вариант по умолчанию, но экран читается через ResolveSubresource (так его читает сама игра).
 * Тест глубины проверяется вторым, дальним треугольником (синий): с глубиной он не виден. */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                     UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                  UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                  D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_log[1 << 21];
static const char *dir;
static void read_log(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\ui4k.log", dir);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(g_log, 1, sizeof g_log - 1, f) : 0;
    if (f) fclose(f);
    g_log[n] = 0;
}

static PFN_COMPILE comp;
static ID3DBlob *compile(const char *src, const char *entry, const char *prof)
{
    ID3DBlob *b = NULL, *e = NULL;
    if (FAILED(comp(src, strlen(src), "t", NULL, NULL, entry, prof, 0, 0, &b, &e))) {
        printf("compile %s: %s\n", entry, e ? (char *)ID3D10Blob_GetBufferPointer(e) : "?");
        return NULL;
    }
    return b;
}

static const char WORLD_VS[] = "float4 main(uint id : SV_VertexID) : SV_Position {"
                               " float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1); }";
static const char UI_VS[] = "float4 main(uint id : SV_VertexID) : SV_Position {"   /* квадрат: левая верхняя четверть */
                            " float2 q[6] = { float2(-1,1), float2(0,1), float2(-1,0), float2(0,1), float2(0,0), float2(-1,0) };"
                            " return float4(q[id], 0, 1); }";
static const char FAR_VS[] = "float4 main(uint id : SV_VertexID) : SV_Position {"   /* тот же мир, но дальше */
                             " float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0.9, 1); }";
static const char BLUE[] = "float4 main() : SV_Target { return float4(0, 0, 1, 1); }";
static const char RED[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
static const char GREEN[] = "float4 main() : SV_Target { return float4(0, 1, 0, 1); }";

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage\nFAIL\n"); return 2; }
    dir = argv[1];
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    comp = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!comp) { printf("no d3dcompiler\nFAIL\n"); return 1; }
    ID3DBlob *bvw = compile(WORLD_VS, "main", "vs_5_0"), *bvu = compile(UI_VS, "main", "vs_5_0");
    ID3DBlob *bpr = compile(RED, "main", "ps_5_0"), *bpg = compile(GREEN, "main", "ps_5_0");
    ID3DBlob *bvf = compile(FAR_VS, "main", "vs_5_0"), *bpb = compile(BLUE, "main", "ps_5_0");
    if (!bvw || !bvu || !bpr || !bpg || !bvf || !bpb) { printf("FAIL\n"); return 1; }
    const BYTE *h = (const BYTE *)ID3D10Blob_GetBufferPointer(bvu) + 4;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\ui4k.ini", dir);
    FILE *ini = fopen(path, "wb");
    int safe = argc > 2 && !strcmp(argv[2], "msaa_safe");
    int live = argc > 2 && !strcmp(argv[2], "msaa_live");        /* MSAA включают на ходу: второй проход с новым swapchain */
    int msaa = safe || live || (argc > 2 && !strcmp(argv[2], "msaa"));
    int hdr0 = msaa || (argc > 2 && !strcmp(argv[2], "hdr0"));   /* HDR выключен в настройках игры: всё с первого кадра */
    int nopresets = argc > 2 && !strcmp(argv[2], "nopresets");   /* С4: мир задан ключом render, строк списка нет */
    int rsv = msaa || (argc > 2 && !strcmp(argv[2], "resolve"));  /* экран читается сведением выборок, как в игре */
    int perf = argc > 2 && !strcmp(argv[2], "perf");   /* Э1: режим замеров; в кадре 1 интерфейс берёт кэш 16x16 (x1) */
    /* вердикт 19.11, п. 6: presets=0 и мир = экран — линии всё равно толстые (метка VS интерфейса без строк списка) */
    int nosplit = argc > 2 && !strcmp(argv[2], "lines_nopre");
    int lines = argc > 2 && (!strcmp(argv[2], "lines") || nosplit);   /* Э5: линия интерфейса под x1.5 — три рисования */
    int lines_off = argc > 2 && !strcmp(argv[2], "lines_off");  /* Э5: thick_lines=0 — одно */
    ID3D11Texture2D *small = NULL;           /* Э1: кэш окна 16x16 и его вид */
    ID3D11ShaderResourceView *small_v = NULL;
    ID3D11Query *pq = NULL;                  /* Э5: счётчик примитивов линии */
    ID3D11SamplerState *point = NULL;
    fprintf(ini, "[ui4k]\ngame_hdr=%d\ngame_aa=%d\nmsaa_safe=%d\nenabled=1\nall_monitors=0\nscale=1.5\nlog=2\nui_vs=%02x%02x%02x%02x%02x%02x%02x%02x\n%s",
            hdr0 ? 0 : 1, msaa && !live ? 4 : 0, safe ? 1 : 0, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
            nopresets ? "presets=0\nfit_native=0\nrender=50\n" : nosplit ? "presets=0\nfit_native=0\n" : perf ? "perf=1\nperf_every=2\n" : lines_off ? "thick_lines=0\n" : "");
    fclose(ini);

    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary\nFAIL\n"); return 1; }
    read_log();
    char want[96];
    unsigned mw = 0, mh = 0;
    const char *s = strstr(g_log, "presets: monitor ");
    if (s) sscanf(s, "presets: monitor %ux%u", &mw, &mh);
    unsigned lw = 0, lh = 0, rw = 0, rh = 0, kn, kd;
    for (s = g_log; (s = strstr(s, "  preset ")); s++) {
        unsigned a, b, c, d;
        if (sscanf(s, "  preset %ux%u: interface %u/%u world %ux%u", &a, &b, &kn, &kd, &c, &d) == 6 && c != mw && c != a) {
            lw = a; lh = b; rw = c; rh = d;
            break;
        }
    }
    if (nopresets) { lw = 1280; lh = 720; rw = 960; rh = 540; }   /* x1.5, мир 50%: 1920x1080 -> 960x540 */
    if (nosplit) { lw = 1280; lh = 720; rw = 1280; rh = 720; }    /* x1.5, мир = экран: своей цели мира нет */
    CHECK(lw && rw, "a preset with a smaller world (monitor %ux%u)", mw, mh);
    if (fails) { printf("FAIL %d\n", fails); return 1; }
    printf("preset %ux%u, world %ux%u, monitor %ux%u\n", lw, lh, rw, rh, mw, mh);

    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k world", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    UINT ns = msaa && !live ? 4 : 1;
    DXGI_SWAP_CHAIN_DESC sd = {{lw, lh, {0, 0}, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0}, {ns, 0},
                               DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    if (FAILED(cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        printf("device\nFAIL\n");
        return 1;
    }
    ID3D11VertexShader *vsw = NULL, *vsu = NULL, *vsf = NULL;
    ID3D11PixelShader *psr = NULL, *psg = NULL, *psb = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bvf), ID3D10Blob_GetBufferSize(bvf), NULL, &vsf);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bpb), ID3D10Blob_GetBufferSize(bpb), NULL, &psb);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bvw), ID3D10Blob_GetBufferSize(bvw), NULL, &vsw);
    /* С2: игра пересоздаёт шейдеры (новый бой — новые объекты того же шейдера). Узнавание шейдера интерфейса не
     * должно зависеть от того, сколько таких объектов уже было: у мода была таблица на 64 адреса. */
    static ID3D11VertexShader *many[70];
    for (int i = 0; i < 70; i++)
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bvu), ID3D10Blob_GetBufferSize(bvu), NULL, &many[i]);
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bvu), ID3D10Blob_GetBufferSize(bvu), NULL, &vsu);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bpr), ID3D10Blob_GetBufferSize(bpr), NULL, &psr);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bpg), ID3D10Blob_GetBufferSize(bpg), NULL, &psg);
    ID3D11Texture2D *bb = NULL, *dt = NULL, *glass = NULL, *flat = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    D3D11_DEPTH_STENCIL_DESC zd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0, 0, {0}, {0}};
    ID3D11DepthStencilState *zon = NULL, *zoff = NULL;
    ID3D11Device_CreateDepthStencilState(dev, &zd, &zon);
    zd.DepthEnable = FALSE;
    ID3D11Device_CreateDepthStencilState(dev, &zd, &zoff);
    D3D11_VIEWPORT vp = {0, 0, (float)lw, (float)lh, 0, 1};
    const float black[4] = {0, 0, 0, 1};

  for (int pass = 0; pass < (live ? 2 : 1); pass++) {
    if (pass == 1) {
        /* Г1: игрок включил MSAA в бою. Игра бросает прежний экран и создаёт новый swapchain с выборками через
         * IDXGIFactory::CreateSwapChain; строка списка (логический размер) та же. */
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11DepthStencilView_Release(dsv); ID3D11Texture2D_Release(dt);
        ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(bb);
        ID3D11Texture2D_Release(glass);
        IDXGISwapChain_Release(sc);
        dsv = NULL; dt = NULL; rtv = NULL; bb = NULL; glass = NULL; sc = NULL;
        IDXGIFactory *fa = NULL;
        CreateDXGIFactory(&IID_IDXGIFactory, (void **)&fa);   /* как игра: через импорт, который мод перехватывает */
        ns = 4;
        sd.SampleDesc.Count = ns;
        HRESULT hr = fa ? IDXGIFactory_CreateSwapChain(fa, (IUnknown *)dev, &sd, &sc) : E_FAIL;
        CHECK(SUCCEEDED(hr) && sc, "new multisampled swap chain through the factory: 0x%08lx", (unsigned long)hr);
        if (fa) IDXGIFactory_Release(fa);
        if (!sc) { printf("FAIL %d\n", fails); return 1; }
    }
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    D3D11_TEXTURE2D_DESC dd = {lw, lh, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, {ns, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_DEPTH_STENCIL, 0, 0};
    ID3D11Device_CreateTexture2D(dev, &dd, NULL, &dt);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dt, NULL, &dsv);
    const int ms = ns > 1;   /* экран этого прохода многовыборочный */

    /* копия экрана (как под размытие стеклянных панелей) — последнее чтение экрана перед интерфейсом */
    D3D11_TEXTURE2D_DESC gd = {lw, lh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Device_CreateTexture2D(dev, &gd, NULL, &glass);
    if (ms) ID3D11Device_CreateTexture2D(dev, &gd, NULL, &flat);   /* сведённый экран — читать точки */
    /* Э1: квадратный кэш окна 16x16 — мод его не увеличивает; перепись должна найти его в цели xk */
    if (perf && !point) {   /* кэш окна выводится точечной выборкой */
        D3D11_SAMPLER_DESC pd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
        ID3D11Device_CreateSamplerState(dev, &pd, &point);
    }
    if (perf && !small) {
        D3D11_TEXTURE2D_DESC sd = {16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Device_CreateTexture2D(dev, &sd, NULL, &small);
        if (small) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)small, NULL, &small_v);
        CHECK(small_v, "16x16 cache texture");
    }
/* копия экрана: с MSAA игра сводит выборки, без него копирует */
#define SCREEN_TO(t) do { if (rsv || ms) ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *)(t), 0, (ID3D11Resource *)bb, 0, \
                                                                         DXGI_FORMAT_B8G8R8A8_UNORM); \
                          else ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)(t), (ID3D11Resource *)bb); } while (0)
    /* 3: меню без мира, а выученная копия экрана из боя не читается; 4: снова бой — мир опять в своей цели */
    for (int frame = 0; frame < 5; frame++) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, zon, 0);
        ID3D11DeviceContext_VSSetShader(ctx, vsw, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, psr, NULL, 0);
        if (frame != 3) {
            ID3D11DeviceContext_Draw(ctx, 3, 0);   /* мир */
            /* дальний треугольник: с тестом глубины не виден (и с MSAA: глубина игры подходит к цели мира) */
            ID3D11DeviceContext_VSSetShader(ctx, vsf, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, psb, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        if (frame == 1 || frame == 2 || frame == 4) {
            /* «метка на земле» тем же VS интерфейса до эффектов, потом копия экрана — мир ещё не кончился */
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, zoff, 0);
            ID3D11DeviceContext_VSSetShader(ctx, vsu, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, psr, NULL, 0);
            if (small_v && frame == 1) { ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &small_v); ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &point); }
            ID3D11DeviceContext_Draw(ctx, 6, 0);
            if (small_v && frame == 1) { ID3D11ShaderResourceView *nv = NULL; ID3D11SamplerState *ns = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &nv); ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &ns); }
            SCREEN_TO(glass);
        }
        if (frame >= 2) {
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, zoff, 0);
            ID3D11DeviceContext_VSSetShader(ctx, vsu, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, psg, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 6, 0);   /* интерфейс */
            if ((lines || lines_off) && frame == 2 && pass == 0) {   /* Э5: рамка миникарты — LineStrip интерфейса */
                D3D11_QUERY_DESC qd = {D3D11_QUERY_PIPELINE_STATISTICS, 0};
                if (!pq) ID3D11Device_CreateQuery(dev, &qd, &pq);
                ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
                ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)pq);
                ID3D11DeviceContext_Draw(ctx, 2, 3);   /* вертикальная линия по x = 0 (граница четверти) */
                ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)pq);
                ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                D3D11_VIEWPORT gv;
                UINT gn = 1;
                ID3D11DeviceContext_RSGetViewports(ctx, &gn, &gv);
                CHECK(gn == 1 && gv.Width == vp.Width && gv.TopLeftX == vp.TopLeftX, "viewport the game set is back after the line");
            }
            /* плашка: копия экрана уже с интерфейсом в картинку размера мира — растянуть весь экран, не угол */
            if (frame == 2 || frame == 4) SCREEN_TO(glass);
            if (ms) SCREEN_TO(flat);
            /* пиксели back buffer: копия логического пикселя (через мод — box в физику) в маленькую staging */
            D3D11_TEXTURE2D_DESC td = {8, 8, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                       D3D11_CPU_ACCESS_READ, 0};
            const unsigned px[5][2] = {{lw - 3, lh - 3}, {lw / 2 + 8, lh / 2 + 8}, {lw / 8, lh / 8},
                                       {lw / 2 + 8, lh / 2 + 8}, {lw / 8, lh / 8}};
            const DWORD want[5] = {frame == 3 ? 0xff000000 : 0xffff0000, frame == 3 ? 0xff000000 : 0xffff0000, 0xff00ff00,
                                   0xffff0000, 0xff00ff00};
            const char *name[5] = {"bottom-right: world", "center: world", "top-left quarter: interface",
                                   "screen copy over the interface, center: world (whole screen stretched, not its corner)",
                                   "screen copy over the interface, top-left: interface"};
            for (int i = 0; i < (frame != 3 ? 5 : 3); i++) {
                ID3D11Resource *from = i < 3 ? (ms ? (ID3D11Resource *)flat : (ID3D11Resource *)bb) : (ID3D11Resource *)glass;
                ID3D11Texture2D *st = NULL;
                ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
                D3D11_BOX b = {px[i][0], px[i][1], 0, px[i][0] + 1, px[i][1] + 1, 1};
                ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, from, 0, &b);
                D3D11_MAPPED_SUBRESOURCE mp;
                DWORD v = 0;
                if (st && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
                    v = *(DWORD *)mp.pData;
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
                }
                CHECK(v == want[i], "pass %d frame %d %s: pixel %08lx, want %08lx", pass, frame, name[i], v, want[i]);
                if (st) ID3D11Texture2D_Release(st);
            }
        }
        IDXGISwapChain_Present(sc, 0, 0);
    }
  }
    if (lines || lines_off) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS ps = {0};
        HRESULT qh = S_FALSE;
        for (int i = 0; pq && i < 1000 && qh == S_FALSE; i++) qh = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)pq, &ps, sizeof ps, 0);
        printf("line draw: %llu primitive(s)\n", (unsigned long long)ps.IAPrimitives);
        CHECK(qh == S_OK && ps.IAPrimitives == (lines ? 3u : 1u), "interface line at x1.5: %llu primitives, want %u",
              (unsigned long long)ps.IAPrimitives, lines ? 3u : 1u);
    }
    read_log();
    if (perf) {
        /* Э1: строки «perf» каждые 2 кадра; метки видеокарты; перепись кадра 1 нашла кэш 16x16 в цели xk */
        CHECK(strstr(g_log, "perf: measurement mode on"), "perf mode announced");
        CHECK(strstr(g_log, "perf: GPU timestamps on"), "GPU timestamps created");
        int lines = 0, gpu = 0, ui = 0, copy = 0;
        for (const char *p = g_log; (p = strstr(p, "] perf ")) != NULL;) {
            const char *e = strchr(p, '\n');
            char line[1024];
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, p, n);
            line[n] = 0;
            lines++;
            gpu |= strstr(line, "gpu frame (n 0)") == NULL;
            ui |= strstr(line, "interface part (n 0)") == NULL;
            copy |= strstr(line, "world copy (n 0)") == NULL;
            p += n;
        }
        CHECK(lines >= 2, "perf lines every 2 frames: %d", lines);
        CHECK(gpu, "GPU frame times measured");
        CHECK(ui, "GPU time of the interface part measured");
        CHECK(copy, "GPU time of the own world copy measured");
        CHECK(strstr(g_log, "x1 caches into xk targets 1.0 per frame (16x16)"), "census found the 16x16 cache drawn into an xk target");
        if (small_v) ID3D11ShaderResourceView_Release(small_v);
        if (small) ID3D11Texture2D_Release(small);
    }
    if (safe) {
        /* msaa_safe=1: MSAA без HDR на строке «мир меньше экрана» — мир в размере интерфейса, своей цели мира нет */
        CHECK(!strstr(g_log, "world: own target"), "safe mode: no own world target");
        CHECK(strstr(g_log, "MSAA x4 is on in the game settings"), "MSAA taken from the game settings");
        CHECK(strstr(g_log, "safe mode"), "safe mode announced in the log");
        snprintf(want, sizeof want, "tex2D screen: %ux%u -> %ux%u fmt=87", lw, lh, mw, mh);
        CHECK(strstr(g_log, want), "safe mode: screen copy at interface size (%s)", want);
        snprintf(want, sizeof want, " -> %ux%u fmt=", rw, rh);
        CHECK(!strstr(g_log, want), "safe mode: nothing is created at world size (%ux%u)", rw, rh);
        if (flat) ID3D11Texture2D_Release(flat);
        ID3D11RenderTargetView_Release(rtv);
        ID3D11Texture2D_Release(bb);
        ID3D11DeviceContext_Release(ctx);
        IDXGISwapChain_Release(sc);
        ID3D11Device_Release(dev);
        DestroyWindow(wnd);
        printf(fails ? "FAIL %d\n" : "PASS\n", fails);
        return fails != 0;
    }
    if (hdr0) {
        CHECK(strstr(g_log, "HDR is off in the game settings"), "HDR off taken from the game settings");
        /* экранная картинка формата экрана (копия для воды и т.п.) — сразу в размере мира */
        D3D11_TEXTURE2D_DESC cd = {lw, lh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D *ct = NULL;
        ID3D11Device_CreateTexture2D(dev, &cd, NULL, &ct);
        if (ct) ID3D11Texture2D_Release(ct);
        read_log();
        snprintf(want, sizeof want, "tex2D scene: %ux%u -> %ux%u fmt=87", lw, lh, rw, rh);
        CHECK(strstr(g_log, want), "screen-format copy texture at world size (%s)", want);
    } else
        CHECK(strstr(g_log, "straight into the back buffer"), "noticed the world in the back buffer (HDR off)");

    snprintf(want, sizeof want, "world: own target %ux%u", rw, rh);
    if (!nosplit) CHECK(strstr(g_log, want), "own world target of world size (%s)", want);
    else CHECK(!strstr(g_log, "world: own target"), "world = screen: no own world target");
    if (msaa) {
        /* П7: с MSAA мир остаётся в размере строки — своя цель мира с теми же выборками, что экран и глубина игры */
        CHECK(!strstr(g_log, "safe mode"), "no safe mode: the world keeps the size of the entry with MSAA");
        snprintf(want, sizeof want, "world: own target %ux%u, samples 4", rw, rh);
        CHECK(strstr(g_log, want), "own world target is multisampled like the screen (%s)", want);
        CHECK(!strstr(g_log, "skipped") && !strstr(g_log, "cannot"), "nothing skipped or cropped");
        /* глубина игры (размер мира, 4 выборки) подходит к цели мира: её отвязывают только от экрана, для интерфейса */
        snprintf(want, sizeof want, "depth detached - real size or sample count differs from the render target (target %ux%u x4", rw, rh);
        CHECK(!strstr(g_log, want), "world depth stays bound with the own world target");
        /* ничего экранного в размере монитора, кроме самого экрана: копии и глубина — в размере мира */
        snprintf(want, sizeof want, "tex2D scene: %ux%u -> %ux%u fmt=45 ms=4", lw, lh, rw, rh);
        CHECK(strstr(g_log, want), "multisampled depth at world size (%s)", want);
        snprintf(want, sizeof want, "-> %ux%u fmt=", mw, mh);
        CHECK(!strstr(g_log, want), "no screen-size texture at monitor size (%ux%u): world size did not change", mw, mh);
    }
    if (live) {
        CHECK(strstr(g_log, "MSAA x4 is now on (swap chain)"), "MSAA switched on at run time is noticed");
        snprintf(want, sizeof want, "world: own target %ux%u, samples 1", rw, rh);
        CHECK(strstr(g_log, want), "before the switch: plain own world target (%s)", want);
    } else if (msaa)
        CHECK(strstr(g_log, "MSAA x4 is on in the game settings"), "MSAA taken from the game settings");
    if (flat) ID3D11Texture2D_Release(flat);
    CHECK(strstr(g_log, "world: interface vertex shader"), "interface VS recognised by checksum");
    if (!nosplit) {   /* мир = экран: переходить к интерфейсу не из чего */
        CHECK(strstr(g_log, "interface after the last screen read"), "switch to the interface only after the last screen read");
        CHECK(strstr(g_log, "interface, no 3D world in this frame"), "menu frame: interface straight away despite the learned screen read");
    }
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(bb);
    ID3D11DeviceContext_Release(ctx);
    IDXGISwapChain_Release(sc);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
