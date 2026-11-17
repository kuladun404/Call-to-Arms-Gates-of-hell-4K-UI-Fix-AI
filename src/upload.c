/* В0: игра загружает пиксели процессором (UpdateSubresource) в текстуру, которую мод увеличил.
 * Игра считает текстуру логической и даёт картинку логического размера; настоящая текстура больше, и драйвер читал бы
 * строки настоящей длины — за концом памяти игры (падение 16.11: nvwgf2umx, строка 15 360 байт вместо 7680).
 * Мод пересчитывает сам: картинка игры растягивается во временную память настоящего размера области, в D3D уходит она.
 * Из памяти игры читается ровно то, что игра дала: ширина x высота области, по шагу строки игры.
 * Чего пересчитать нельзя (сжатые форматы, глубина, MSAA) — не передаём вовсе: вызов с несходящимися размерами
 * в D3D без отладочного слоя не проверяется. */
#include "common.h"
#include "fmt.h"

static void said(volatile LONG *n, const char *what, ID3D11Resource *r, UINT sub, const D3D11_TEXTURE2D_DESC *d,
                 UINT sw, UINT sh, UINT dw, UINT dh)
{
    LONG k = InterlockedIncrement(n);
    if (k > 8) return;
    LOG("upload: %s - texture %p sub %u fmt=%d samples=%u bind=0x%x, game %ux%u, real %ux%u%s", what, r, sub, d->Format,
        d->SampleDesc.Count, d->BindFlags, sw, sh, dw, dh, k == 8 ? " (further lines of this kind are not logged)" : "");
}

/* Ревизия кода 17.11, п. 2: если драйвер не умеет свои командные списки (DriverCommandLists = 0: WARP, часть AMD / Intel),
 * D3D для UpdateSubresource с областью в отложенном контексте прибавляет к указателю данных смещение области; Microsoft
 * велит приложению заранее его вычесть. Значит, данные игры начинаются на это смещение (по её области и шагу строки)
 * дальше указателя, а свой указатель мод должен сдвинуть назад на смещение своей области. Без этого D3D читал мимо
 * буфера мода — падение (test_boundary deferred на сборке до правки). 1 — так и есть для этого вызова. */
int upload_quirk(ID3D11DeviceContext *c, ID3D11Resource *r)
{
    if (CTXW(c)->type != D3D11_DEVICE_CONTEXT_DEFERRED || !r) return 0;
    ID3D11Device *d = NULL;
    ID3D11Resource_GetDevice(r, &d);
    if (!d) return 0;
    static ID3D11Device *volatile last;
    static volatile LONG lastq;
    int q;
    if (d == last) q = (int)lastq;
    else {
        D3D11_FEATURE_DATA_THREADING t = {0};
        q = SUCCEEDED(ID3D11Device_CheckFeatureSupport(d, D3D11_FEATURE_THREADING, &t, sizeof t)) && !t.DriverCommandLists;
        lastq = q;
        last = d;
        LOG("upload: device %p %s its own command lists%s", (void *)d, q ? "does not support" : "supports",
            q ? " - region uploads in deferred contexts use the pointer offset D3D expects" : "");
    }
    ID3D11Device_Release(d);
    return q;
}

int upload_prepare(ID3D11Resource *r, UINT sub, const D3D11_BOX **box, const void **data, UINT *rp, UINT *dp, D3D11_BOX *pb,
                   void **tmp, int quirk)
{
    static volatile LONG n_ok, n_skip, n_bad;
    ScaledInfo si;
    *tmp = NULL;
    if (!res_scaled(r, &si)) return 0;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);   /* размеры логические, остальное настоящее */
    UINT mips = d.MipLevels ? d.MipLevels : 1, mip = sub % mips, lw, lh, pw, ph;
    res_mip_size(&si, mip, &lw, &lh, &pw, &ph);
    if (lw == pw && lh == ph) return 0;   /* этот уровень не отличается — как есть */

    const D3D11_BOX *b = *box;
    UINT l = b ? b->left : 0, t = b ? b->top : 0, rt = b ? b->right : lw, bt = b ? b->bottom : lh;
    if (rt <= l || bt <= t) return -1;                      /* пустая область: D3D ничего не делает */
    if (rt > lw || bt > lh || (b && (b->front != 0 || b->back < 1))) {   /* ошибка самой игры — в D3D не передаём */
        said(&n_bad, "box outside the texture, skipped", r, sub, &d, rt - l, bt - t, pw, ph);
        return -1;
    }
    UINT bpp = fmt_bytes(d.Format);
    if (!bpp || d.SampleDesc.Count > 1 || (d.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
        said(&n_skip, "cannot convert, skipped", r, sub, &d, rt - l, bt - t, pw, ph);
        return -1;
    }
    UINT sw = rt - l, sh = bt - t;
    const BYTE *game = (const BYTE *)*data;
    if (quirk && b) game += (size_t)b->front * *dp + (size_t)b->top * *rp + (size_t)b->left * bpp;   /* сдвиг игры — обратно */
    UINT X0 = res_edge(&si, mip, 0, l), X1 = res_edge(&si, mip, 0, rt), Y0 = res_edge(&si, mip, 1, t), Y1 = res_edge(&si, mip, 1, bt);
    if (X1 <= X0) { if (X0 >= pw) X0 = pw - 1; X1 = X0 + 1; }   /* мир меньше логического: область не схлопывать */
    if (Y1 <= Y0) { if (Y0 >= ph) Y0 = ph - 1; Y1 = Y0 + 1; }
    UINT dw = X1 - X0, dh = Y1 - Y0;
    UINT srp = *rp >= sw * bpp ? *rp : sw * bpp;   /* шаг строки игры; 0 или меньше ширины — строки подряд */
    BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)dw * dh * bpp);
    if (!buf) {
        said(&n_skip, "out of memory, skipped", r, sub, &d, sw, sh, dw, dh);
        return -1;
    }
    px_resample(game, sw, sh, srp, buf, dw, dh, dw * bpp, bpp, fmt_lerp8(d.Format));
    said(&n_ok, "game pixels stretched for an enlarged texture", r, sub, &d, sw, sh, dw, dh);
    pb->left = X0; pb->top = Y0; pb->front = 0; pb->right = X1; pb->bottom = Y1; pb->back = 1;
    *box = pb;
    /* свой сдвиг — D3D прибавит его сам; указатель вне буфера не разыменовывается */
    *data = quirk ? (const void *)((ULONG_PTR)buf - ((ULONG_PTR)Y0 * dw * bpp + (ULONG_PTR)X0 * bpp)) : (const void *)buf;
    *rp = dw * bpp;
    *dp = dw * dh * bpp;
    *tmp = buf;
    return 1;
}

void upload_done(void *tmp)
{
    if (tmp) HeapFree(GetProcessHeap(), 0, tmp);
}
