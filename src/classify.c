/* F05: какие текстуры игры живут в физическом размере. Чистая функция — тестируется отдельно (tests/test_classify.c).
 *  SCREEN: размер = логический экран / 2^n (n = 0..4, округление вниз или вверх), привязка RT/DS/UAV
 *          (кадр, пост-эффекты, глубина);
 *  UI_RT:  прочие RT/DS произвольного размера — кэши виджетов UI, превью юнита 858x348 и его глубина.
 *          Не квадрат и не pow2 x pow2 (так отсекаются тени, отражения, атласы), один слой. Число выборок MSAA
 *          на решение не влияет (В0): многовыборочная цель и обычная текстура, в которую её сводят, должны быть
 *          одного настоящего размера, как и цель с глубиной.
 *  Текстуры с начальными данными не масштабируются никогда: их пиксели — картинка, а не цель рендера.
 *  Текстуры, которые игра читает или пишет процессором (staging, dynamic, любой доступ CPU), тоже никогда (В3): игра
 *  сама ходит по их строкам и ждёт свой размер — иначе скриншот и картинка сохранения выходят «углом». Пересчёт между
 *  ними и увеличенными делает копия (copy.c).
 * Отличие от v1: размер в физике считается to_phys() по каждой стороне, а не делением на целое. */
#include "classify.h"

static int pow2(UINT v) { return v && !(v & (v - 1)); }

static int screen_match(UINT w, UINT h, UINT lw, UINT lh)
{
    if (!lw || !lh) return 0;
    for (UINT k = 1; k <= 16; k *= 2) {
        UINT wf = lw / k, wc = (lw + k - 1) / k, hf = lh / k, hc = (lh + k - 1) / k;
        if ((w == wf || w == wc) && (h == hf || h == hc)) return 1;
    }
    return 0;
}

TexClass tex_classify(const D3D11_TEXTURE2D_DESC *d, int has_init, UINT lw, UINT lh, Ratio k)
{
    if (!d || has_init || ratio_is_one(k)) return TEX_KEEP;
    if (to_phys(k, d->Width) > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        to_phys(k, d->Height) > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return TEX_KEEP;
    if (d->CPUAccessFlags || d->Usage == D3D11_USAGE_STAGING || d->Usage == D3D11_USAGE_DYNAMIC) return TEX_KEEP;
    const UINT gpu = D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
    if (screen_match(d->Width, d->Height, lw, lh) && (d->BindFlags & gpu)) return TEX_SCREEN;
    if ((d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) && d->Usage == D3D11_USAGE_DEFAULT &&
        d->ArraySize == 1 && d->Width != d->Height && !(pow2(d->Width) && pow2(d->Height)) &&
        d->Width <= 4096 && d->Height <= 4096)
        return TEX_UI_RT;
    return TEX_KEEP;
}

int tex_origin_candidate(const D3D11_TEXTURE2D_DESC *d, int has_init, Ratio k)
{
    const UINT rs = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!d || has_init || ratio_is_one(k)) return 0;
    /* 19.11: «t:create» делает и глубину вида 3D (GUI2::eViewport: 512x512 и 1024x1024 R24G8 — 3D-иконки инвентаря).
     * Цель вида увеличивалась, глубина — нет: размеры расходились, мод отвязывал глубину, и модель рисовалась без неё
     * (задние грани поверх передних — чёрный клин на ящике с инструментами). Глубина — тот же кандидат */
    int target = (d->BindFlags & rs) == rs, depth = (d->BindFlags & D3D11_BIND_DEPTH_STENCIL) && !(d->BindFlags & D3D11_BIND_RENDER_TARGET);
    if ((!target && !depth) || d->SampleDesc.Count != 1 || d->ArraySize != 1 || d->MipLevels != 1) return 0;
    if (d->Usage != D3D11_USAGE_DEFAULT || d->CPUAccessFlags) return 0;
    return d->Width && d->Height && d->Width <= 4096 && d->Height <= 4096 &&
           to_phys(k, d->Width) <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           to_phys(k, d->Height) <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

UINT tex_full_mips(UINT w, UINT h)
{
    UINT m = 1, s = w > h ? w : h;
    while (s > 1) { s >>= 1; m++; }
    return m;
}

D3D11_TEXTURE2D_DESC tex_phys_desc(const D3D11_TEXTURE2D_DESC *d, Ratio k)
{
    D3D11_TEXTURE2D_DESC n = *d;
    n.Width = (UINT)to_phys(k, d->Width);
    n.Height = (UINT)to_phys(k, d->Height);
    if (!n.Width) n.Width = 1;   /* F14: сцена может быть меньше логического (рендер < 100%) */
    if (!n.Height) n.Height = 1;
    if (n.MipLevels == 0) n.MipLevels = tex_full_mips(d->Width, d->Height); /* игра ждёт столько мипов, сколько у логического */
    return n;
}
