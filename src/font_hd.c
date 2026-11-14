/* Чёткие шрифты. Игра грузит страницы атласов BMFont (TGA) так: создаёт пустую текстуру
 * B8G8R8A8 без мипмапов (без привязки или как SRV) и заливает её UpdateSubresource.
 * Такие текстуры размеров страниц шрифтов создаём сразу ×scale (игре GetDesc показывает
 * прежний размер, UV нормализованы). При заливке:
 *   - есть ui4k_fonts\<hash>.raw (пиксели ×scale в том же формате) — заливаем его: глифы
 *     той же раскладки, но в scale раз детальнее;
 *   - нет — увеличиваем присланную картинку билинейно (выглядит как раньше).
 * font_filter: sharp (по умолчанию) — бикубика с восстановлением резкого края, nearest — без
 * фильтрации (пиксели блоками), bilinear — сглаженное.
 * font_dump=1 пишет присланные картинки в ui4k_dump\ для подготовки замен.
 * font_big=1 (по умолчанию) включает и страницы 2048x1024 (Arial Unicode «main», 32 МБ видеопамяти
 * каждая): для них ui4k_fonts\<hash>.pat — только чёткие глифы поверх увеличенной страницы. */
#include "common.h"
#include <stdio.h>
#include <string.h>

static const GUID MARK_UP = {0x7c0f1d2e, 0x3a44, 0x4b9e, {0x81, 0x22, 0x5e, 0x10, 0x9a, 0xcd, 0x01, 0x02}};

static int page_dims(UINT w, UINT h)
{
    return (w == 256 && h == 256) || (w == 512 && h == 256) || (w == 512 && h == 512) ||
           (g_font_big && w == 2048 && h == 1024);
}

static int fmt_ok(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM;
}

int fonthd_create(ID3D11Device *dev, PFN_CT2D_ orig, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                  ID3D11Texture2D **out, HRESULT *phr)
{
    if (!g_font_hd || !d || init || !out) return 0;
    if (d->MipLevels != 1 || d->ArraySize != 1 || d->SampleDesc.Count != 1 || d->Usage != D3D11_USAGE_DEFAULT ||
        d->CPUAccessFlags || d->MiscFlags || !fmt_ok(d->Format) || !page_dims(d->Width, d->Height) ||
        (d->BindFlags != 0 && d->BindFlags != D3D11_BIND_SHADER_RESOURCE))
        return 0;
    D3D11_TEXTURE2D_DESC n = *d;
    n.Width *= g_scale;
    n.Height *= g_scale;
    HRESULT hr = orig(dev, &n, NULL, out);
    if (FAILED(hr) || !*out) return 0;
    d3d_mark_scaled(*out);
    DWORD one = 1;
    ID3D11Texture2D_SetPrivateData(*out, &MARK_UP, sizeof one, &one);
    *phr = hr;
    return 1;
}

static int is_up(ID3D11Resource *r)
{
    UINT sz = sizeof(DWORD);
    DWORD v = 0;
    return r && SUCCEEDED(ID3D11Resource_GetPrivateData(r, &MARK_UP, &sz, &v)) && v == 1;
}

static void path_in(WCHAR *out, const WCHAR *dir, const char *file)
{
    lstrcpyW(out, g_dir);
    lstrcatW(out, dir);
    CreateDirectoryW(out, NULL);
    WCHAR wf[96];
    MultiByteToWideChar(CP_ACP, 0, file, -1, wf, 96);
    lstrcatW(out, L"\\");
    lstrcatW(out, wf);
}

/* Catmull-Rom */
static float cr(float a, float b, float c, float d, float t)
{
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

#define SHARPEN 1.9f   /* крутизна края: (v - 0.5) * SHARPEN + 0.5 */
#define THIN_LO 0.45f  /* штрихи с пиком ниже не усиливаем (тонкие и бледные не пропадут) */
#define THIN_HI 0.80f  /* с пиком выше — усиливаем полностью */

/* ×s бикубикой, затем край снова ~1 пиксель, как у родной отрисовки ×s: контраст вокруг 0.5,
 * взвешенный по локальному максимуму исходника 3×3 (тонкие бледные штрихи не трогаем).
 * Форма, толщина и положение букв — исходные. Каждый байт BGRA обрабатывается отдельно. */
static void sharp_up(const BYTE *src, UINT sp, UINT w, UINT h, BYTE *dst, UINT s)
{
    UINT W = w * s;
#define PX(x, y, c) ((float)src[(size_t)((y) < 0 ? 0 : (y) >= (int)h ? (int)h - 1 : (y)) * sp +                                  (size_t)((x) < 0 ? 0 : (x) >= (int)w ? (int)w - 1 : (x)) * 4 + (c)] * (1.0f / 255))
    for (UINT Y = 0; Y < h * s; Y++) {
        float fy = (Y + 0.5f) / s - 0.5f;
        int y1 = (int)(fy < 0 ? fy - 1 : fy);
        float ty = fy - y1;
        int cy = (int)(Y / s);
        BYTE *o = dst + (size_t)Y * W * 4;
        for (UINT X = 0; X < W; X++) {
            float fx = (X + 0.5f) / s - 0.5f;
            int x1 = (int)(fx < 0 ? fx - 1 : fx);
            float tx = fx - x1;
            int cx = (int)(X / s);
            for (int c = 0; c < 4; c++) {
                float r[4];
                for (int j = 0; j < 4; j++) {
                    int yy = y1 - 1 + j;
                    r[j] = cr(PX(x1 - 1, yy, c), PX(x1, yy, c), PX(x1 + 1, yy, c), PX(x1 + 2, yy, c), tx);
                }
                float v = cr(r[0], r[1], r[2], r[3], ty);
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                float pk = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        float q = PX(cx + dx, cy + dy, c);
                        if (q > pk) pk = q;
                    }
                float k = (pk - THIN_LO) / (THIN_HI - THIN_LO);
                if (k < 0) k = 0;
                if (k > 1) k = 1;
                float hard = (v - 0.5f) * SHARPEN + 0.5f;
                if (hard < 0) hard = 0;
                if (hard > 1) hard = 1;
                v += (hard - v) * k;
                o[X * 4 + c] = (BYTE)(v * 255 + 0.5f);
            }
        }
    }
#undef PX
}

/* увеличение ×s, 4 байта на пиксель: font_filter=nearest — каждый пиксель блоком s×s (текст ровно как
 * в 1080p, без мыла: квадраты букв стоят на целых пикселях и рисуются ровно ×s), bilinear — как раньше */
static void upscale(const BYTE *src, UINT sp, UINT w, UINT h, BYTE *dst, UINT s)
{
    UINT W = w * s, H = h * s;
    if (g_font_filter == FONT_SHARP) {
        sharp_up(src, sp, w, h, dst, s);
        return;
    }
    if (g_font_filter == FONT_NEAREST) {
        for (UINT Y = 0; Y < H; Y++) {
            const UINT *r = (const UINT *)(src + (size_t)(Y / s) * sp);
            UINT *o = (UINT *)(dst + (size_t)Y * W * 4);
            for (UINT X = 0; X < W; X++) o[X] = r[X / s];
        }
        return;
    }
    for (UINT Y = 0; Y < H; Y++) {
        float fy = (Y + 0.5f) / s - 0.5f;
        int y0 = (int)(fy < 0 ? -1 : fy);
        float ty = fy - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > (int)h - 1) y1 = h - 1;
        if (y0 > (int)h - 1) y0 = h - 1;
        const BYTE *r0 = src + (size_t)y0 * sp, *r1 = src + (size_t)y1 * sp;
        BYTE *o = dst + (size_t)Y * W * 4;
        for (UINT X = 0; X < W; X++) {
            float fx = (X + 0.5f) / s - 0.5f;
            int x0 = (int)(fx < 0 ? -1 : fx);
            float tx = fx - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > (int)w - 1) x1 = w - 1;
            if (x0 > (int)w - 1) x0 = w - 1;
            for (int c = 0; c < 4; c++) {
                float a = r0[x0 * 4 + c] + (r0[x1 * 4 + c] - r0[x0 * 4 + c]) * tx;
                float b = r1[x0 * 4 + c] + (r1[x1 * 4 + c] - r1[x0 * 4 + c]) * tx;
                o[X * 4 + c] = (BYTE)(a + (b - a) * ty + 0.5f);
            }
        }
    }
}

/* Заплатка для огромных страниц (main, 2048x1024): поверх увеличенной страницы кладём только
 * чёткие глифы. Формат: "UI4P", u32 n; n раз: u16 x,y,w,h (×scale), u8 маска байтов BGRA, 3 байта,
 * затем w*h байт (в каждый байт маски) или w*h*4 (маска 0xF). */
static void apply_patch(BYTE *buf, UINT W, UINT H, const char *file)
{
    if (!file[0]) return;
    WCHAR path[MAX_PATH];
    path_in(path, L"ui4k_fonts", file);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LOG("font_hd: %ux%u atlas %.16s has no replacement (upscaled)", W / g_scale, H / g_scale, file);
        return;
    }
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE *p = size != INVALID_FILE_SIZE ? (BYTE *)HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    int ok = p && ReadFile(f, p, size, &got, NULL) && got == size && size >= 8 && !memcmp(p, "UI4P", 4);
    CloseHandle(f);
    UINT n = ok ? *(UINT *)(p + 4) : 0, done = 0;
    size_t off = 8;
    for (; ok && done < n; done++) {
        if (off + 12 > size) break;
        const WORD *hd = (const WORD *)(p + off);
        UINT x = hd[0], y = hd[1], w = hd[2], h = hd[3], mask = p[off + 8];
        size_t bpp = mask == 0xF ? 4 : 1, len = (size_t)w * h * bpp;
        off += 12;
        if (off + len > size || x + w > W || y + h > H) break;
        const BYTE *src = p + off;
        for (UINT yy = 0; yy < h; yy++) {
            BYTE *o = buf + ((size_t)(y + yy) * W + x) * 4;
            for (UINT xx = 0; xx < w; xx++, o += 4) {
                if (bpp == 4) { memcpy(o, src, 4); src += 4; continue; }
                BYTE v = *src++;
                for (int c = 0; c < 4; c++)
                    if (mask & (1u << c)) o[c] = v;
            }
        }
        off += len;
    }
    if (ok) LOG("font_hd: %ux%u atlas patched by %s (%u of %u glyphs)", W, H, file, done, n);
    else LOG("font_hd: bad patch %s", file);
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

/* 1 — заливка выполнена здесь */
int fonthd_update(ID3D11DeviceContext *real, PFN_UPD_ orig, ID3D11Resource *r, UINT sub, const D3D11_BOX *box,
                  const void *data, UINT rp)
{
    if (!g_font_hd || !data || sub != 0 || !is_up(r)) return 0;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d); /* логический размер */
    UINT x0 = 0, y0 = 0, w = d.Width, h = d.Height;
    if (box) { x0 = box->left; y0 = box->top; w = box->right - box->left; h = box->bottom - box->top; }
    if (!w || !h) return 0;
    UINT s = g_scale, W = w * s, H = h * s;
    size_t need = (size_t)W * H * 4;
    BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, need);
    if (!buf) return 0;
    int replaced = 0;
    char patch_name[64] = "";

    if (!box) {
        ULONG64 x = 1469598103934665603ull;
        const BYTE *p = (const BYTE *)data;
        for (UINT y = 0; y < h; y++, p += rp)
            for (UINT i = 0; i < w * 4; i++) { x ^= p[i]; x *= 1099511628211ull; }
        char name[64];
        WCHAR path[MAX_PATH];
        if (g_font_dump) {
            sprintf(name, "%016llx_%ux%u_f%d.raw", x, w, h, d.Format);
            path_in(path, L"ui4k_dump", name);
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD wr;
                p = (const BYTE *)data;
                for (UINT y = 0; y < h; y++, p += rp) WriteFile(f, p, w * 4, &wr, NULL);
                CloseHandle(f);
            }
        }
        sprintf(patch_name, "%016llx.pat", x);
        sprintf(name, "%016llx.raw", x);
        path_in(path, L"ui4k_fonts", name);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            if (ReadFile(f, buf, (DWORD)need, &got, NULL) && got == need) replaced = 1;
            else LOG("font_hd: bad replacement %s (%lu of %zu bytes)", name, got, need);
            CloseHandle(f);
        }
        if (replaced) {
            static LONG cnt;
            LOG("font_hd: %ux%u atlas %s replaced (#%ld)", w, h, name, InterlockedIncrement(&cnt));
        }
    }
    if (!replaced) {
        upscale((const BYTE *)data, rp, w, h, buf, s);
        if (!box) apply_patch(buf, W, H, patch_name);
    }
    D3D11_BOX b = {x0 * s, y0 * s, 0, (x0 + w) * s, (y0 + h) * s, 1};
    orig(real, r, 0, box ? &b : NULL, buf, W * 4, 0);
    HeapFree(GetProcessHeap(), 0, buf);
    return 1;
}
