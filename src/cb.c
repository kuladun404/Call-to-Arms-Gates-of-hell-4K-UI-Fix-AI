/* F08: постэффекты считают UV из констант экрана (SV_Position * 1/W) — с физическими целями без пересчёта
 * получается уменьшенный «призрак» сцены в углу. В CB размеров из cb_fix (в float: 156 — сцена/пост, 84 — HBAO+)
 * логические величины экрана заменяются физическими при UpdateSubresource и Map -> Unmap.
 * UI-проекцию (CB 232 float) не трогать — иначе UI сжимается. scan_cb=1 пишет найденные пары в лог
 * (после патча игры — найти новые размеры).
 * F14: при рендере не 100% у сцены и выхода разные размеры, а CB156 привязан и к тем, и к другим. Храним логическую
 * копию содержимого и в момент рисования (cb_pre_draw) переписываем буфер под коэффициент привязанной цели. */
#include <stdio.h>
#include "common.h"
#include "cbfix.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_MAP)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                                           D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *PFN_UNMAP)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *PFN_UPD)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                        const void *, UINT, UINT);

static int cb_size(ID3D11Resource *r, UINT *bytes)
{
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    *bytes = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

/* размеры из ui4k.ini (cb_fix) и узнанные у игры: буфер сцены из её shader_constants.h, буферы библиотеки HBAO+ */
static int wanted(UINT floats)
{
    if (!g_cfg.ncbfix) return 0;   /* cb_fix пуст — правка буферов выключена */
    for (int i = 0; i < g_cfg.ncbfix; i++)
        if (g_cfg.cbfix[i] == (int)floats) return 1;
    return floats == (UINT)anchors_scene_floats() || anchors_hbao(floats);
}

/* scan_cb: логические пары экрана в любом CB — по одной строке на (буфер, смещение) */
static void scan(ID3D11Resource *r, const float *f, UINT n, const char *how)
{
    Screen s = screen_get();
    if (!s.lw || n < 2) return;
    static void *seen[1024];
    static volatile LONG ns;
    float tmp[2];
    for (UINT i = 0; i + 1 < n; i++) {
        tmp[0] = f[i]; tmp[1] = f[i + 1];
        if (!cb_fix_floats(tmp, 2, s.lw, s.lh, s.pw, s.ph)) continue;
        void *key = (void *)((ULONG_PTR)r ^ ((ULONG_PTR)i << 48));
        int dup = 0;
        for (LONG j = 0; j < ns && j < 1024; j++) if (seen[j] == key) { dup = 1; break; }
        if (dup || ns >= 1024) continue;
        LONG k = InterlockedIncrement(&ns);   /* ревизия кода 4, п. 5: два потока могли пройти проверку выше вместе */
        if (k > 1024) continue;
        seen[k - 1] = key;
        LOG("scan_cb %s: CB %p (%u floats) float[%u] = (%g, %g)%s", how, r, n, i, f[i], f[i + 1],
            wanted(n) ? " [fixed]" : "");
    }
}

/* снимок кадра: какие числа в CB похожи на размер экрана (логический, физический, половины, обратные) — до и после */
static void diag_vals(ID3D11DeviceContext *c, const char *what, const float *before, const float *after, UINT n, Ratio r)
{
    Screen s = screen_get();
    char b[1400];
    int p = snprintf(b, sizeof b, "%s r=%d/%d:", what, r.num, r.den);
    for (UINT i = 0; i < n && p < (int)sizeof b - 40; i++) {
        float v = before[i], a = v < 0 ? -v : v;
        if (a == 0) continue;
        float w = a < 1 ? 1 / a : a;
        static const float m[] = {1, 0.5f, 0.25f, 2};
        int hit = 0;
        for (int k = 0; k < 4; k++) {
            float t1 = (float)s.lw * m[k], t2 = (float)s.lh * m[k], t3 = (float)s.pw * m[k], t4 = (float)s.ph * m[k];
            float q[4] = {t1, t2, t3, t4};
            for (int j = 0; j < 4; j++) if (w > q[j] * 0.97f && w < q[j] * 1.03f) hit = 1;
            float t5 = 2560.0f * m[k], t6 = 1440.0f * m[k], t7 = 1280.0f * m[k], t8 = 720.0f * m[k];
            if ((w > t5 * 0.99f && w < t5 * 1.01f) || (w > t6 * 0.99f && w < t6 * 1.01f) || (w > t7 * 0.99f && w < t7 * 1.01f) ||
                (w > t8 * 0.99f && w < t8 * 1.01f)) hit = 1;
        }
        if (hit) p += snprintf(b + p, sizeof b - p, " [%u]%g->%g", i, before[i], after[i]);
    }
    diag_note(c, "%s", b);
}

/* логические пары экрана -> физические для цели с коэффициентом r (num 0 — масштаб интерфейса, как раньше) */
/* Г2: HBAO+ RadiusToScreen — радиус затенения в точках кадра (16.11: [12] буфера на 84). Игра считает его для логической
 * высоты; с настоящими картинками большего размера затенение искало бы соседей на вдвое меньшем расстоянии — мелкие
 * детали мира выглядят плоско («мыльно»). */

/* Устойчивость к обновлениям: буфер HBAO+ узнаётся по размеру, который создала сама библиотека (anchors.c), номер
 * радиуса — по устройству (R² и −1/R² следом, cb_hbao_radius_index); hbao_radius>0 в ui4k.ini — номер вручную, 0 — не
 * трогать. Радиус умножается на то же, на что изменилась высота кадра, если в буфере нашлась экранная пара. */
/* номер RadiusToScreen в буфере HBAO+ из n чисел. 20.11: устройство буфера за игру не меняется — номер находится один раз
 * (единственное подходящее место) и дальше держится; *ok — в этой записи на нём действительно радиус. Иначе в кадре, где
 * угол обзора проходит ≈ 41°, подходят два места (cbfix.c) — и мигание. kn — размер буфера (−1: место занимается), ki —
 * номер; у HBAO+ 16.11 — буфер на 84, на 8 — служебный, без радиуса. -1 — радиус ещё не нашёлся */
static int hbao_r(const float *f, UINT n, int *ok)
{
    static volatile LONG kn[8], ki[8];
    *ok = 0;
    for (int k = 0; k < 8; k++) {
        LONG v = kn[k];
        if (v == (LONG)n) { *ok = cb_hbao_radius_at(f, n, (unsigned)ki[k]); return (int)ki[k]; }
        if (v) continue;
        int i = cb_hbao_radius_index(f, n);
        if (i < 0) return -1;
        *ok = 1;
        if (InterlockedCompareExchange(&kn[k], -1, 0) != 0) continue;
        InterlockedExchange(&ki[k], i);
        InterlockedExchange(&kn[k], (LONG)n);
        LOG("gamedata: HBAO+ shading radius found at float %d of the %u-float buffer (R2 and -1/R2 follow it) - kept from now on",
            i, n);
        return i;
    }
    int i = cb_hbao_radius_index(f, n);
    *ok = i >= 0;
    return i;
}

static int hbao_index(const float *f, UINT n)
{
    if (!g_cfg.hbao_radius || !anchors_hbao(n)) return -1;
    if (g_cfg.hbao_radius > 0) return (UINT)g_cfg.hbao_radius < n ? g_cfg.hbao_radius : -1;
    int ok, i = hbao_r(f, n, &ok);
    return ok ? i : -1;
}

/* Э3: буфер сцены — только пары в местах из имён переменных шейдеров игры.
 * Ревизия 20.11, Ф1: буфер HBAO+ — только числа до радиуса. В структуре HBAO+ первыми идут 1/размер четверти и кадра
 * ([4..7] у 16.11), за радиусом — мировые числа: позиция камеры [40..42], матрица с ней [64..79] (реверс, «Шейдеры»).
 * Проверка всех чисел подряд удваивала бы камеру, оказавшуюся рядом с (1920, 1080), (960, 540) или (480, 270). Все числа —
 * только пока радиус не нашёлся (служебный буфер HBAO+ на 8 чисел, другая версия библиотеки) */
static int scene_fix(float *f, UINT n, long lw, long lh, long pw, long ph)
{
    int ns = 0;
    const unsigned *at = anchors_screen_slots(&ns);
    if (at && g_cfg.cb_names && n == (UINT)anchors_scene_floats())
        return cb_fix_at_x(f, n, at, anchors_screen_exact(), ns, lw, lh, pw, ph);
    if (anchors_hbao(n)) {
        int ok, r = g_cfg.hbao_radius > 0 ? g_cfg.hbao_radius : hbao_r(f, n, &ok);
        if (r > 1 && (UINT)r <= n) return cb_fix_floats(f, (UINT)r, lw, lh, pw, ph);
    }
    return cb_fix_floats(f, n, lw, lh, pw, ph);
}

void cb_key(void)
{
    static int was;
    int ns = 0;
    if (!anchors_screen_slots(&ns)) return;
    int down = ui4k_hotkey(VK_F6, 0);
    if (down && !was) {
        LONG on = !InterlockedXor(&g_cfg.cb_names, 1);
        LOG("cb: scene buffer - %s (Ctrl+F6)", on ? "named places only" : "every float checked (v2 way)");
    }
    was = down;
}

static int fix_r(float *f, UINT n, Ratio r)
{
    Screen s = screen_get();
    int hb = hbao_index(f, n);
    long pw = s.pw, ph = s.ph;
    if (r.num && !ratio_eq(r, g_cfg.scale)) { pw = (long)to_phys(r, s.lw); ph = (long)to_phys(r, s.lh); }
    int fixed = scene_fix(f, n, s.lw, s.lh, pw, ph);
    if (hb >= 0 && fixed && s.lh) f[hb] *= (float)ph / (float)s.lh;
    return fixed;
}

/* есть ли в логическом содержимом экранные числа — при любом коэффициенте цели (проба на удвоенном размере) */
static int has_screen(const float *f, UINT n)
{
    Screen s = screen_get();
    float t[CB_TRKF];
    if (!s.lw || !s.lh || n > CB_TRKF) return 0;
    memcpy(t, f, n * sizeof(float));
    return scene_fix(t, n, s.lw, s.lh, s.lw * 2, s.lh * 2) > 0;
}

/* ---- F14/L8: логические копии CB с экранными константами, по контексту ---- */
static int split(void) { return !ratio_eq(g_cfg.scene, g_cfg.scale); }

void cb_ctx_forget(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < CB_TRK; i++)
        if (s->trk[i].r) { ID3D11Resource_Release(s->trk[i].r); s->trk[i].r = NULL; }
    dof_ctx_forget(c);
    box_ctx_forget(c);
}

/* f — логическое содержимое; правит его под коэффициент цели контекста и запоминает копию этого контекста */
static void fix(ID3D11DeviceContext *c, ID3D11Resource *r, float *f, UINT n)
{
    CtxState *s = &CTXW(c)->st;
    Ratio cur = s->rk;
    if (split() && n > CB_TRKF) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 2)
            LOG("cb: %u-float constant buffer is larger than %d - on entries with world != screen it is not refitted per "
                "target (game update?)", n, CB_TRKF);
    }
    if (split() && n <= CB_TRKF) {
        int i = 0, fr = -1;
        for (; i < CB_TRK && s->trk[i].r != r; i++)
            if (!s->trk[i].r && fr < 0) fr = i;
        /* ревизия кода 4, п. 1: место в таблице — только буферу с экранными числами (служебный буфер HBAO+ на 8 чисел их
         * не имеет: переписывать под цель в нём нечего, а место понадобится буферу сцены). Уже взятый буфер копию
         * обновляет всегда — иначе при смене цели вернулись бы старые числа */
        if (i == CB_TRK && fr >= 0 && has_screen(f, n)) { i = fr; ID3D11Resource_AddRef(r); s->trk[i].r = r; }   /* держим: пишем в него позже */
        if (i < CB_TRK) {
            memcpy(s->trk[i].log, f, n * sizeof(float));
            s->trk[i].n = n;
            s->trk[i].applied = cur.num ? cur : g_cfg.scale;
        }
    }
    float before[CB_TRKF];
    if (g_cap && n <= CB_TRKF) memcpy(before, f, n * sizeof(float));
    float ldim = dof_ldim(f, n);
    fix_r(f, n, cur);
    dof_written(c, r, f, n, ldim);   /* Г4: что ушло в буфер — радиус размытия правится при рисовании */
    box_written(c, r, f, n);         /* Э8: и шаг уменьшения шейдера копирования */
    if (g_cap && n <= CB_TRKF) {
        diag_vals(c, n == 156 ? "  CB156 fix" : "  CB fix", before, f, n, cur);
        int hb = hbao_index(before, n);
        if (hb >= 0) diag_note(c, "  CB%u HBAO+ radius [%d] %g -> %g", n, hb, before[hb], f[hb]);
        char b[4096];   /* всё содержимое (логическое): искать числа, меняющиеся от кадра к кадру */
        int p = snprintf(b, sizeof b, "  CB%u raw:", n);
        for (UINT i = 0; i < n && p < (int)sizeof b - 20; i++) p += snprintf(b + p, sizeof b - p, " %.6g", before[i]);
        diag_note(c, "%s", b);
    }
}

void cb_pre_draw(ID3D11DeviceContext *c)
{
    CtxState *s = &CTXW(c)->st;
    Ratio cur = s->rk;
    if (!cur.num) return;
    for (int i = 0; i < CB_TRK; i++) {
        if (!s->trk[i].r || ratio_eq(s->trk[i].applied, cur)) continue;
        ID3D11Resource *r = s->trk[i].r;
        UINT n = s->trk[i].n;
        float tmp[CB_TRKF];
        memcpy(tmp, s->trk[i].log, n * sizeof(float));
        s->trk[i].applied = cur;
        fix_r(tmp, n, cur);
        if (g_cap) diag_vals(c, n == 156 ? "  CB156 rewrite" : "  CB rewrite", s->trk[i].log, tmp, n, cur);
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
        if (d.Usage == D3D11_USAGE_DYNAMIC) {
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(RC(c), r, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, tmp, n * sizeof(float));
                ID3D11DeviceContext_Unmap(RC(c), r, 0);
            }
        } else if (d.Usage == D3D11_USAGE_DEFAULT) {
            ID3D11DeviceContext_UpdateSubresource(RC(c), r, 0, NULL, tmp, 0, 0);
        }
        dof_written(c, r, tmp, n, dof_ldim(s->trk[i].log, n));
        box_written(c, r, tmp, n);
    }
}

static HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT fl,
                                        D3D11_MAPPED_SUBRESOURCE *m)
{
    HRESULT hr = ORIGC(c, Map, PFN_MAP)(RC(c), r, sub, t, fl, m);
    UINT bytes;
    if (SUCCEEDED(hr) && m && m->pData &&
        (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE || t == D3D11_MAP_WRITE_NO_OVERWRITE) && cb_size(r, &bytes) &&
        (g_cfg.scan_cb || wanted(bytes / 4) || g_cap)) {
        CtxState *s = &CTXW(c)->st;
        for (int i = 0; i < MAX_CBMAP; i++)
            if (!s->cbm[i].r) { s->cbm[i].r = r; s->cbm[i].p = (float *)m->pData; s->cbm[i].n = bytes / 4; break; }
    }
    return hr;
}

static void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub)
{
    CtxState *s = &CTXW(c)->st;
    for (int i = 0; i < MAX_CBMAP; i++)
        if (s->cbm[i].r == r) {
            if (g_cfg.scan_cb) scan(r, s->cbm[i].p, s->cbm[i].n, "map");
            if (wanted(s->cbm[i].n)) {
                if (g_cap) diag_note(c, "CB%u map %06x", s->cbm[i].n, (unsigned)((ULONG_PTR)r & 0xffffff));
                fix(c, r, s->cbm[i].p, s->cbm[i].n);
            } else if (g_cap) diag_cb_raw(c, r, s->cbm[i].p, s->cbm[i].n);   /* 20.11, снимок: прочие буферы */
            s->cbm[i].r = NULL;
            break;
        }
    ORIGC(c, Unmap, PFN_UNMAP)(RC(c), r, sub);
}

typedef void(STDMETHODCALLTYPE *PFN_UPD1)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                         const void *, UINT, UINT, UINT);

/* общий путь UpdateSubresource / UpdateSubresource1 (one — вторая, с флагами копии) */
static inline void update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp,
                          UINT dp, int one, UINT flags, void *ra)
{
    UINT bytes;
    float tmp[4096];
    void *heap = NULL;
    D3D11_BOX pb;
    if (r && data && !overlay_call(ra)) {
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            fontpages_upload(c, r, sub, box, data, rp);   /* Э6: страница шрифта? — запомнить до копии в рабочую */
            /* В0: картинка игры логического размера в увеличенную текстуру — растянуть самим, не давать драйверу
             * читать за концом памяти игры */
            if (upload_prepare(r, sub, &box, &data, &rp, &dp, &pb, &heap, box && upload_quirk(c, r)) < 0) return;
        } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            /* ревизия v3, п. 4: описание буфера — одно на заливку (их ~2 000 за кадр, реверс) и только когда нужно */
            int cbw = !box && (g_cfg.ncbfix || g_cfg.scan_cb), fpw = fontpages_vb_wanted(c), dgw = g_cap && !box;
            D3D11_BUFFER_DESC bd = {0};
            if (cbw || fpw || dgw) ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &bd);
            if (fpw) fontpages_vb_update(r, &bd, box, data);   /* Э6: копия вершин букв — до CB */
            bytes = bd.ByteWidth;
            /* 20.11, снимок: что игра пишет в прочие буферы констант — камера (сцена VS), положение объекта */
            if (dgw && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && !wanted(bytes / 4)) diag_cb_raw(c, r, (const float *)data, bytes / 4);
            if (!cbw || !(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) goto pass;
            if (g_cfg.scan_cb) scan(r, (const float *)data, bytes / 4, "upd");
            if (bytes <= sizeof tmp && wanted(bytes / 4)) {
                if (g_cap) diag_note(c, "CB%u update %06x", bytes / 4, (unsigned)((ULONG_PTR)r & 0xffffff));
                memcpy(tmp, data, bytes);
                fix(c, r, tmp, bytes / 4);
                data = tmp;
            }
        }
    }
pass:
    /* вторая функция — слот ID3D11DeviceContext1 той же таблицы настоящего контекста (как у переходников обёртки) */
    if (one) ((PFN_UPD1)((ID3D11DeviceContext1 *)RC(c))->lpVtbl->UpdateSubresource1)(RC(c), r, sub, box, data, rp, dp, flags);
    else ORIGC(c, UpdateSubresource, PFN_UPD)(RC(c), r, sub, box, data, rp, dp);
    upload_done(heap);
}

static void STDMETHODCALLTYPE hk_UpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                                   const D3D11_BOX *box, const void *data, UINT rp, UINT dp)
{
    update(c, r, sub, box, data, rp, dp, 0, 0, __builtin_return_address(0));
}

static void STDMETHODCALLTYPE hk_UpdateSubresource1(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                                    const D3D11_BOX *box, const void *data, UINT rp, UINT dp, UINT flags)
{
    update(c, r, sub, box, data, rp, dp, 1, flags, __builtin_return_address(0));
}

void cb_install(void)
{
    CTX_HOOK(UpdateSubresource, hk_UpdateSubresource);     /* всегда: загрузка пикселей в увеличенные текстуры (В0) */
    CTX_HOOK(UpdateSubresource1, hk_UpdateSubresource1);
    if (!g_cfg.ncbfix && !g_cfg.scan_cb) return;
    CTX_HOOK(Map, hk_Map);
    CTX_HOOK(Unmap, hk_Unmap);
}
