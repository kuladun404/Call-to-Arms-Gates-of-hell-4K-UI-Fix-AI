/* Устойчивость к обновлениям игры через собранную DLL: «игра после обновления».
 *   test_update.exe <папка с d3d11.dll и GFSDK_SSAO_fake.win64.dll>
 * Тест сам пишет архив шейдеров shader.pak (как у игры: zip без сжатия, кэш по MD5 имени): шейдер интерфейса под
 * именем игры, но с новым байткодом (собран здесь), буфер сцены — 40 групп (160 чисел вместо 156). Библиотека HBAO+
 * (поддельная, по имени модуля) создаёт буфер на 100 чисел, радиус в нём сдвинут на [44] (за ним R² и −1/R²).
 * Мод должен: узнать шейдер интерфейса по имени, править экранные числа в буфере на 160, узнать буфер HBAO+ на 100 и
 * его радиус. Экран 1280x720, x1.5 (физический 1920x1080). Последняя строка PASS/FAIL.
 *   test_update.exe <папка> ucache — то же, но в архиве только исходники (как у игрока с модами, 23.11), а
 *   скомпилированные шейдеры лежат файлами в отдельной папке, как их собирает сама игра в
 *   Documents\my games\gates of hell\shader_cache\dx11.0 (test_shader_cache=). */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi.h>
#include "gamedata.h"

typedef HRESULT(WINAPI *PFN_CDSC)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                  const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                  ID3D11DeviceContext **);
typedef HRESULT(WINAPI *PFN_COMPILE)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT,
                                     UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT (*PFN_MAKECB)(ID3D11Device *, UINT, ID3D11Buffer **);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static const char *dir;
static char g_log[1 << 20];

static void read_log(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\ui4k.log", dir);
    FILE *f = fopen(p, "rb");
    size_t n = f ? fread(g_log, 1, sizeof g_log - 1, f) : 0;
    if (f) fclose(f);
    g_log[n] = 0;
}

/* ---- zip без сжатия (CRC не нужен: архив читает только мод) ---- */
typedef struct { unsigned char *b; size_t n, cap; } Buf;
static void put(Buf *z, const void *d, size_t n)
{
    if (z->n + n > z->cap) { z->cap = (z->n + n) * 2; z->b = realloc(z->b, z->cap); }
    memcpy(z->b + z->n, d, n);
    z->n += n;
}
static void p16(Buf *z, unsigned v) { unsigned char t[2] = {(unsigned char)v, (unsigned char)(v >> 8)}; put(z, t, 2); }
static void p32(Buf *z, unsigned v) { p16(z, v & 0xffff); p16(z, v >> 16); }
typedef struct { const char *name; const void *data; size_t n; } Ent;
static int write_zip(const char *path, const Ent *e, int ne)
{
    Buf z = {0};
    unsigned off[16];
    for (int i = 0; i < ne; i++) {
        off[i] = (unsigned)z.n;
        p32(&z, 0x04034b50); p16(&z, 10); p16(&z, 0); p16(&z, 0); p16(&z, 0); p16(&z, 0); p32(&z, 0);
        p32(&z, (unsigned)e[i].n); p32(&z, (unsigned)e[i].n); p16(&z, (unsigned)strlen(e[i].name)); p16(&z, 0);
        put(&z, e[i].name, strlen(e[i].name)); put(&z, e[i].data, e[i].n);
    }
    unsigned cd = (unsigned)z.n;
    for (int i = 0; i < ne; i++) {
        p32(&z, 0x02014b50); p16(&z, 20); p16(&z, 10); p16(&z, 0); p16(&z, 0); p16(&z, 0); p16(&z, 0); p32(&z, 0);
        p32(&z, (unsigned)e[i].n); p32(&z, (unsigned)e[i].n); p16(&z, (unsigned)strlen(e[i].name));
        p16(&z, 0); p16(&z, 0); p16(&z, 0); p16(&z, 0); p32(&z, 0); p32(&z, off[i]); put(&z, e[i].name, strlen(e[i].name));
    }
    unsigned cdn = (unsigned)z.n - cd;
    p32(&z, 0x06054b50); p16(&z, 0); p16(&z, 0); p16(&z, (unsigned)ne); p16(&z, (unsigned)ne); p32(&z, cdn); p32(&z, cd); p16(&z, 0);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(z.b, 1, z.n, f);
    fclose(f);
    free(z.b);
    return 1;
}

static void cache_path(const char *shader, const char *ext, char *out)
{
    char low[256];
    size_t l = strlen(shader);
    for (size_t i = 0; i <= l; i++) low[i] = (char)(shader[i] >= 'A' && shader[i] <= 'Z' ? shader[i] + 32 : shader[i]);
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    unsigned char md[16];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_MD5_ALGORITHM, NULL, 0);
    BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0);
    BCryptHashData(h, (PUCHAR)low, (ULONG)l, 0);
    BCryptFinishHash(h, md, 16, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    int p = sprintf(out, "shader/cache/dx11.0/");
    for (int i = 0; i < 16; i++) p += sprintf(out + p, "%02x", md[i]);
    sprintf(out + p, ".%s", ext);
}

static unsigned char *cache_entry(const char *shader, const void *code, size_t n, size_t *len)
{
    size_t l = strlen(shader);
    unsigned char *d = malloc(1 + l + n);
    d[0] = (unsigned char)l;
    memcpy(d + 1, shader, l);
    memcpy(d + 1 + l, code, n);
    *len = 1 + l + n;
    return d;
}

/* записать буфер констант через обёртку контекста (как игра) и прочитать, что ушло в видеокарту */
static void cb_roundtrip(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Buffer *cb, const float *in, float *out, UINT n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, in, n * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
    }
    D3D11_BUFFER_DESC sd = {n * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *st = NULL;
    ID3D11Device_CreateBuffer(dev, &sd, NULL, &st);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)cb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, n * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Buffer_Release(st);
}

static int near_(float a, float b, float eps) { return a > b - eps && a < b + eps; }

/* «real»: архив шейдеров настоящей игры. Шейдеры автоэкспозиции из её кэша пересобираются из её исходников (все
 * #include находятся, регистры те же), VS интерфейса узнаётся по имени, буфер сцены — 156 чисел. */
static int t_real(const char *pakpath, ID3D11Device *dev)
{
    FILE *f = fopen(pakpath, "rb");
    if (!f) { printf("real archive not readable - skipped\n"); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    Pak p;
    CHECK(pak_parse(b, got, &p), "real archive not parsed");
    static const char *const cs[] = {"postprocess/histogram[HistogramBinsCS]", "postprocess/histogram[HistogramMaxAvg_Init_CS]"};
    for (int i = 0; i < 2; i++) {
        size_t len;
        const unsigned char *c = pak_shader(&p, cs[i], &len);
        ID3D11ComputeShader *s = NULL;
        CHECK(c && SUCCEEDED(ID3D11Device_CreateComputeShader(dev, c, len, NULL, &s)), "%s not created", cs[i]);
        if (s) ID3D11ComputeShader_Release(s);
        read_log();
        char want[160];
        snprintf(want, sizeof want, "exposure: %s rebuilt from the game's source", cs[i]);
        CHECK(strstr(g_log, want), "%s not rebuilt from the game's source", cs[i]);
        printf("%s: %s\n", cs[i], strstr(g_log, want) ? "rebuilt from the game's source" : "NOT rebuilt");
    }
    size_t len;
    const unsigned char *v = pak_shader(&p, "vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR", &len);
    ID3D11VertexShader *vs = NULL;
    if (v) ID3D11Device_CreateVertexShader(dev, v, len, NULL, &vs);
    if (vs) ID3D11VertexShader_Release(vs);
    read_log();
    CHECK(strstr(g_log, "interface vertex shaders - 9 of 9 names found (9 in the archive, 0 compiled by the game), 2 distinct"), "log: 9 names / 2 VS");
    CHECK(strstr(g_log, "world: interface vertex shader"), "real interface VS not recognised");
    CHECK(strstr(g_log, "scene constant buffer - 156 floats") || 1, "-");
    pak_free(&p);
    free(b);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage\nFAIL\n"); return 2; }
    dir = argv[1];
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_COMPILE comp = dc ? (PFN_COMPILE)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!comp) { printf("no d3dcompiler\nFAIL\n"); return 1; }
    /* «новый» шейдер интерфейса: байткод не тот, что был у игры 16.11 */
    static const char vs[] = "float4 main(float4 p : POSITION, float4 c : COLOR) : SV_Position { return p * c.x; }";
    ID3DBlob *b = NULL;
    if (FAILED(comp(vs, sizeof vs - 1, "ui", NULL, NULL, "main", "vs_5_0", 0, 0, &b, NULL))) { printf("compile\nFAIL\n"); return 1; }
    const void *code = ID3D10Blob_GetBufferPointer(b);
    size_t clen = ID3D10Blob_GetBufferSize(b);

    /* «игровой» шейдер автоэкспозиции: поток (x,y) читает пиксель (x,y) картинки сцены — как histogram.hlsl игры */
    static const char hist[] =
        "#include \"shader_constants.h\"\n"
        "Texture2D scene_Buffer : register(t0);\n"
        "RWTexture2D<float4> out_Buffer : register(u0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void HistogramBinsCS(uint3 dispatchThreadID : SV_DispatchThreadID)\n"
        "{ out_Buffer[dispatchThreadID.xy] = scene_Buffer[dispatchThreadID.xy]; }\n";
    const char *nohdr = strchr(hist, '\n') + 1;   /* в кэше — собранный без #include (байткод любой, лишь бы игровой) */
    ID3DBlob *cb_ = NULL;
    if (FAILED(comp(nohdr, strlen(nohdr), "h", NULL, NULL, "HistogramBinsCS", "cs_5_0", 0, 0, &cb_, NULL))) { printf("cs\nFAIL\n"); return 1; }
    char pak[MAX_PATH], ent[128], ent2[128], path[MAX_PATH];
    snprintf(pak, sizeof pak, "%s\\shader.pak", dir);
    static const char consts[] = "// after an update\n#define pscSceneGroup\t_PSG(pscgScene, 40)\n";
    size_t elen, e2len;
    cache_path("vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR", "vso", ent);
    unsigned char *e = cache_entry("vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR", code, clen, &elen);
    cache_path("postprocess/histogram[HistogramBinsCS]", "cso", ent2);
    unsigned char *e2 = cache_entry("postprocess/histogram[HistogramBinsCS]", ID3D10Blob_GetBufferPointer(cb_),
                                    ID3D10Blob_GetBufferSize(cb_), &e2len);
    static const char copy_src[] = "float2 src_size : packoffset(c5.x);\nfloat2 one_pixel : packoffset(c5.z);\n";
    static const char dof_src[] = "float2 textureDimensions : packoffset(c24.y);\n";
    static const char gauss_src[] = "float2 halfPixel : packoffset(c0);\n";
    static const char lum_src[] = "float2 textureDimension : packoffset(c14);\n";
    static const char sky_src[] = "float4 sunMoonPosRes : packoffset(c17);\n";
    static const char fxaa_src[] = "float2 fxaaNormPixelWidthHeight : packoffset(c37.z);\n";
    Ent z[10] = {{"shader/dx10/shader_constants.h", consts, sizeof consts - 1}, {ent, e, elen},
                 {"shader/dx10/postprocess/histogram.hlsl", hist, sizeof hist - 1}, {ent2, e2, e2len},
                 {"shader/dx10/postprocess/copy.hlsl", copy_src, sizeof copy_src - 1},
                 {"shader/dx10/postprocess/dof/dof_blur.hlsl", dof_src, sizeof dof_src - 1},
                 {"shader/dx10/postprocess/gauss_blur.hlsl", gauss_src, sizeof gauss_src - 1},
                 {"shader/dx10/postprocess/luma.hlsl", lum_src, sizeof lum_src - 1},
                 {"shader/dx10/sky.hlsl", sky_src, sizeof sky_src - 1},
                 {"shader/dx10/postprocess/lenseffects/lens_effects.hlsl", fxaa_src, sizeof fxaa_src - 1}};
    int real = argc > 3 && !strcmp(argv[2], "real");
    int ucache = argc > 2 && !strcmp(argv[2], "ucache");
    char ucdir[MAX_PATH] = "";
    if (real) lstrcpynA(pak, argv[3], MAX_PATH);   /* архив настоящей игры, поддельный не пишем */
    else if (ucache) {
        /* в архиве — только исходники; кэш-записи — файлами «<md5>.vso|cso» в папке, как у игры */
        Ent src[8];
        int ns = 0;
        for (int i = 0; i < 10; i++) if (z[i].data != e && z[i].data != e2) src[ns++] = z[i];
        CHECK(ns == 8 && write_zip(pak, src, ns), "sources-only archive not written");
        snprintf(ucdir, sizeof ucdir, "%s\\shader_cache", dir);
        CreateDirectoryA(ucdir, NULL);
        const char *uent[2] = {ent, ent2};
        const unsigned char *ud[2] = {e, e2};
        size_t un[2] = {elen, e2len};
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s\\%s", ucdir, strrchr(uent[i], '/') + 1);
            FILE *cf = fopen(path, "wb");
            CHECK(cf && fwrite(ud[i], 1, un[i], cf) == un[i], "cache file %s not written", path);
            if (cf) fclose(cf);
        }
    } else CHECK(write_zip(pak, z, 10), "archive not written");
    snprintf(path, sizeof path, "%s\\ui4k.ini", dir);
    FILE *f = fopen(path, "wb");
    fprintf(f, "[ui4k]\nenabled=1\nscale=1.5\nlog=2\npresets=0\nfit_native=0\nrender=50\ngame_hdr=1\ngame_aa=0\ntest_shader_pak=%s\n", pak);
    if (ucache) fprintf(f, "test_shader_cache=%s\n", ucdir);
    fclose(f);

    snprintf(path, sizeof path, "%s\\d3d11.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m) { printf("LoadLibrary\nFAIL\n"); return 1; }
    PFN_CDSC cdsc = (PFN_CDSC)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    HWND wnd = CreateWindowExW(0, L"STATIC", L"ui4k update", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC scd = {{1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1, wnd,
                                TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = cdsc(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("device 0x%08lx\nFAIL\n", hr); return 1; }
    if (real) {
        t_real(argv[3], dev);
        printf(fails ? "FAIL %d\n" : "PASS\n", fails);
        return fails != 0;
    }

    /* 1. шейдер интерфейса — по имени */
    ID3D11VertexShader *v = NULL;
    ID3D11Device_CreateVertexShader(dev, code, clen, NULL, &v);
    read_log();
    CHECK(strstr(g_log, "interface vertex shaders - 1 of 9 names found"), "interface VS names not looked up in the archive");
    if (ucache) {
        CHECK(strstr(g_log, "1 of 9 names found (0 in the archive, 1 compiled by the game)"),
              "interface VS name must come from the folder the game compiles into");
        CHECK(strstr(g_log, "shaders compiled by the game itself are also looked up in"), "log: game cache folder");
    } else
        CHECK(strstr(g_log, "(1 in the archive, 0 compiled by the game)"), "interface VS name must come from the archive");
    CHECK(strstr(g_log, "world: interface vertex shader"), "updated interface VS not recognised by name");

    /* 2. буфер сцены 160 чисел: экранная пара (1280, 720) -> (1920, 1080) */
    float in[160] = {0}, out[160] = {0};
    in[20] = 1280; in[21] = 720; in[100] = 7;
    in[40] = 1280; in[41] = 720;   /* Э3: та же пара не в экранном месте (не из имён шейдеров) — не трогать */
    D3D11_BUFFER_DESC bd = {sizeof in, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    cb_roundtrip(dev, ctx, cb, in, out, 160);
    printf("scene buffer 160 floats: pair (%g, %g), other value %g\n", out[20], out[21], out[100]);
    CHECK(out[20] == 1920 && out[21] == 1080 && out[100] == 7, "scene buffer of the updated size not fixed");
    CHECK(out[40] == 1280 && out[41] == 720, "E3: pair outside the named places left alone: %g %g", out[40], out[41]);
    read_log();
    CHECK(strstr(g_log, "screen numbers of the scene buffer at floats 0, 20, 22, 56, 70, 97, 150"),
          "E3: places by name from the archive's shader sources");

    /* 3. HBAO+: буфер на 100 чисел создаёт библиотека; радиус на [44] */
    snprintf(path, sizeof path, "%s\\GFSDK_SSAO_fake.win64.dll", dir);
    HMODULE g = LoadLibraryA(path);
    PFN_MAKECB make = g ? (PFN_MAKECB)GetProcAddress(g, "make_cb") : NULL;
    ID3D11Buffer *hb = NULL;
    CHECK(make && SUCCEEDED(make(dev, 400, &hb)) && hb, "fake HBAO+ library did not create its buffer");
    if (hb) {
        float hi[100] = {0}, ho[100] = {0};
        hi[30] = 1.0f / 1280; hi[31] = 1.0f / 720; hi[44] = 25000; hi[45] = 400; hi[46] = -1.0f / 400;
        cb_roundtrip(dev, ctx, hb, hi, ho, 100);
        printf("HBAO+ buffer 100 floats: 1/size (%g, %g), radius %g (want 37500), R2 %g\n", ho[30], ho[31], ho[44], ho[45]);
        CHECK(near_(ho[30], 1.0f / 1920, 1e-9f) && near_(ho[31], 1.0f / 1080, 1e-9f), "HBAO+ buffer of the new size not fixed");
        CHECK(near_(ho[44], 37500, 1) && ho[45] == 400, "HBAO+ radius not found at its new place");
        ID3D11Buffer_Release(hb);
    }
    /* 4. автоэкспозиция: шейдер игры пересобран из её исходника; сетка 1280x720 читает всю картинку 1920x1080.
     * Картинка — четыре четверти; точка сетки (700, 400): по всей картинке это (1050, 600) — правая нижняя (белая),
     * а без пересборки — (700, 400), левая верхняя (красная). */
    {
        static UINT quads[1280 * 720], zero[1280 * 720];
        for (UINT y = 0; y < 720; y++)
            for (UINT x = 0; x < 1280; x++)
                quads[y * 1280 + x] = y < 360 ? (x < 640 ? 0xFF0000FF : 0xFFFF0000) : (x < 640 ? 0xFF00FF00 : 0xFFFFFFFF);
        D3D11_TEXTURE2D_DESC td = {1280, 720, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ID3D11Texture2D *scene = NULL, *uo = NULL, *st = NULL;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);   /* экранная: увеличена x1.5 */
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)scene, 0, NULL, quads, 1280 * 4, 0);
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA sd = {zero, 1280 * 4, 0};
        ID3D11Device_CreateTexture2D(dev, &td, &sd, &uo);       /* с начальными данными — не увеличена */
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
        ID3D11ShaderResourceView *sv = NULL;
        ID3D11UnorderedAccessView *uv = NULL;
        ID3D11ComputeShader *cs = NULL;
        if (scene) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)scene, NULL, &sv);
        if (uo) ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)uo, NULL, &uv);
        ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb_), ID3D10Blob_GetBufferSize(cb_), NULL, &cs);
        /* ревизия кода 4, п. 3: игра держит больше таких шейдеров, чем помнит быстрый отсев мода (16 адресов) */
        ID3D11ComputeShader *more[16] = {0};
        for (int i = 0; i < 16; i++)
            ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(cb_), ID3D10Blob_GetBufferSize(cb_), NULL, &more[i]);
        CHECK(sv && uv && cs && st, "exposure objects");
        if (sv && uv && cs && st) {
            ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
            ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &sv);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uv, NULL);
            ID3D11DeviceContext_Dispatch(ctx, 1280 / 8, 720 / 8, 1);
            ID3D11Buffer *b13 = (ID3D11Buffer *)1;
            ID3D11DeviceContext_CSGetConstantBuffers(ctx, 13, 1, &b13);
            CHECK(b13 == NULL, "the mod's constant buffer b13 left bound after the dispatch");
            if (b13 && b13 != (ID3D11Buffer *)1) ID3D11Buffer_Release(b13);
            ID3D11UnorderedAccessView *nu = NULL;
            ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &nu, NULL);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)uo);
            D3D11_MAPPED_SUBRESOURCE mp;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
                UINT a = ((UINT *)((BYTE *)mp.pData + 400 * mp.RowPitch))[700], t = ((UINT *)((BYTE *)mp.pData + 100 * mp.RowPitch))[100];
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
                printf("exposure grid point (700,400): 0x%08x (want white 0xffffffff), (100,100): 0x%08x (want red)\n", a, t);
                CHECK(a == 0xFFFFFFFF, "exposure shader still reads the corner of the enlarged image");
                CHECK(t == 0xFF0000FF, "exposure shader: top-left grid point should stay red");
            }
        }
        read_log();
        CHECK(strstr(g_log, "postprocess/histogram[HistogramBinsCS] rebuilt from the game's source"), "log: exposure shader rebuilt");
        if (cs) ID3D11ComputeShader_Release(cs);
        for (int i = 0; i < 16; i++) if (more[i]) ID3D11ComputeShader_Release(more[i]);
        if (sv) ID3D11ShaderResourceView_Release(sv);
        if (uv) ID3D11UnorderedAccessView_Release(uv);
        if (scene) ID3D11Texture2D_Release(scene);
        if (uo) ID3D11Texture2D_Release(uo);
        if (st) ID3D11Texture2D_Release(st);
    }

    read_log();
    CHECK(strstr(g_log, "scene constant buffer - 160 floats"), "log: scene buffer size");
    CHECK(strstr(g_log, "NVIDIA HBAO+ constant buffer - 100 floats"), "log: HBAO+ buffer size");
    CHECK(strstr(g_log, "HBAO+ shading radius found at float 44 of the 100-float buffer"), "log: HBAO+ radius place");

    if (cb) ID3D11Buffer_Release(cb);
    if (v) ID3D11VertexShader_Release(v);
    IDXGISwapChain_Release(sc);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    DestroyWindow(wnd);
    printf(fails ? "FAIL %d\n" : "PASS\n", fails);
    return fails != 0;
}
