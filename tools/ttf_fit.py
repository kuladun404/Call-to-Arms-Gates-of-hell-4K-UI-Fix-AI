"""Подгонка векторного глифа (TTF/OTF) к глифу атласа 1080p.

Контур буквы из шрифта той же гарнитуры растягивается/сдвигается так, чтобы его уменьшенная
копия совпала с исходным глифом (ячейка, положение, толщина штриха — оригинальные), и рендерится
в ячейку ×S с честным сглаживанием. Результат никогда не выходит за ячейку.
"""
from functools import lru_cache

import numpy as np
from PIL import Image, ImageDraw, ImageFont

MASTER_PX = 256   # кегль эталонного рендера контура
SS = 4            # суперсэмплинг при рендере в ячейку ×S
SS_BIG = 40       # буквы крупнее (пиксели ×S) — суперсэмплинг 2×2
FIT_ERR = 0.075   # макс. средняя ошибка уменьшенного результата против оригинала
FIT_CORR = 0.90   # мин. корреляция с оригиналом
FIT_ERR_HI = 0.13   # второй критерий: ошибка побольше,
FIT_CORR_HI = 0.97  # но форма почти идентична
WEIGHT_MAX = 0.75   # предел смещения контура (пиксели ×S) при подгонке толщины штриха
EDGE_W = 0.1       # штраф за 1 px суммарного сдвига границ чернил (в единицах средней ошибки)


@lru_cache(maxsize=None)
def _font(path, axes=None):
    """axes — ((имя оси, значение), ...) для вариативного шрифта, например (("Weight", 700),)"""
    font = ImageFont.truetype(str(path), MASTER_PX)
    if axes:
        want = dict(axes)
        vals = []
        for ax in font.get_variation_axes():
            n = ax["name"].decode() if isinstance(ax["name"], bytes) else str(ax["name"])
            vals.append(want.get(n, ax["default"]))
        font.set_variation_by_axes(vals)
    return font


@lru_cache(maxsize=128)  # ~1 МБ на контур; процессов сборки много
def master(path, cid, axes=None):
    """контур символа в высоком разрешении, обрезанный по чернилам; None если глифа нет"""
    if not 0 <= cid < 0x110000:
        return None
    font = _font(path, axes)
    ch = chr(cid)
    try:
        if font.getmask(ch).getbbox() is None:
            return None
        l, t, r, b = font.getbbox(ch)
    except Exception:
        return None
    if r <= l or b <= t:
        return None
    pad = 4
    im = Image.new("L", (r - l + 2 * pad, b - t + 2 * pad), 0)
    ImageDraw.Draw(im).text((pad - l, pad - t), ch, font=font, fill=255)
    bb = im.getbbox()
    if not bb:
        return None
    im = im.crop(bb)
    # поля, чтобы box при resize не выходил за картинку
    m = max(8, max(im.size) // 4)  # поля; за ними отрисовка считает «далеко снаружи»
    out = Image.new("F", (im.size[0] + 2 * m, im.size[1] + 2 * m), 0.0)
    out.paste(im.convert("F"), (m, m))
    return out, m, im.size[0], im.size[1]


def _sdf(img):
    """поле расстояний до контура (пиксели эталона; > 0 — снаружи), уточнённое по сглаживанию края"""
    from scipy.ndimage import distance_transform_edt
    a = np.asarray(img, np.float32) / 255.0
    inside = a >= 0.5
    d_out = distance_transform_edt(~inside)
    d_in = distance_transform_edt(inside)
    sd = np.where(inside, 0.5 - d_in, d_out - 0.5)
    edge = (a > 0) & (a < 1)  # сглаженный край эталона: расстояние линейно по покрытию
    return np.where(edge, 0.5 - a, sd).astype(np.float32)


_SDF = {}


def field(mst):
    """поле расстояний эталона (кешируется на несколько последних контуров)"""
    img = mst[0]
    key = id(img)
    if key not in _SDF:
        if len(_SDF) > 6:
            _SDF.clear()
        _SDF[key] = (img, _sdf(img))  # img держим, чтобы id не переиспользовался
    return _SDF[key][1]


FAR = 1e4  # значение поля за краем эталона (далеко снаружи)


def _render_field(fm, rect, w2, h2, ss=SS):
    """поле расстояний fm=(массив, поле, ширина, высота) в прямоугольник rect: (поле на сетке ×S·ss,
    пикселей поля на пиксель сетки). Билинейная выборка — поле гладкое, её точности хватает."""
    sd, m, mw, mh = fm
    x0, y0, x1, y1 = rect
    if x1 - x0 < 0.25 or y1 - y0 < 0.25:
        return None, 1.0
    W, H = w2 * ss, h2 * ss
    sx = mw / ((x1 - x0) * ss)
    sy = mh / ((y1 - y0) * ss)
    xs = m + ((np.arange(W) + 0.5) - x0 * ss) * sx - 0.5  # центры пикселей сетки в пикселях поля
    ys = m + ((np.arange(H) + 0.5) - y0 * ss) * sy - 0.5
    bh, bw = sd.shape
    xi = np.clip(np.floor(xs).astype(np.int32), 0, bw - 2)
    yi = np.clip(np.floor(ys).astype(np.int32), 0, bh - 2)
    fx = np.clip(xs - xi, 0, 1).astype(np.float32)[None, :]
    fy = np.clip(ys - yi, 0, 1).astype(np.float32)[:, None]
    rows = sd[yi] * (1 - fy) + sd[yi + 1] * fy  # сетка разделима: сначала по строкам, потом по столбцам
    f = rows[:, xi] * (1 - fx) + rows[:, xi + 1] * fx
    f[:, (xs < 0) | (xs > bw - 1)] = FAR
    f[(ys < 0) | (ys > bh - 1), :] = FAR
    return f, 0.5 * (sx + sy)


def _shade(f, k, dm, w2, h2, ss=SS):
    """буква из поля: край — линейный переход шириной в пиксель сетки, контур смещён наружу на dm пикселей
    поля; затем усреднение ss×ss (сглаживание)"""
    v = np.clip(0.5 - (f - dm) / max(k, 1e-3), 0, 1)
    return v.reshape(h2, ss, w2, ss).mean(axis=(1, 3))


def _render(mst, rect, w2, h2):
    """контур в прямоугольник rect=(x0,y0,x1,y1) (координаты ячейки ×S), холст w2×h2"""
    img, m, mw, mh = mst
    x0, y0, x1, y1 = rect
    if x1 - x0 < 0.25 or y1 - y0 < 0.25:
        return None
    W, H = w2 * SS, h2 * SS
    sx = mw / ((x1 - x0) * SS)
    sy = mh / ((y1 - y0) * SS)
    box = (m - x0 * SS * sx, m - y0 * SS * sy, m + (W - x0 * SS) * sx, m + (H - y0 * SS) * sy)
    bw, bh = img.size
    if box[0] < 0 or box[1] < 0 or box[2] > bw or box[3] > bh:
        # слишком далеко за краем: расширяем поля
        ext = int(max(-box[0], -box[1], box[2] - bw, box[3] - bh)) + 2
        big = Image.new("F", (bw + 2 * ext, bh + 2 * ext), 0.0)
        big.paste(img, (ext, ext))
        img = big
        box = tuple(v + ext for v in box)
    r = np.asarray(img.resize((W, H), Image.BILINEAR if max(sx, sy) < 2 else Image.LANCZOS, box=box), np.float32) / 255.0
    return np.clip(r.reshape(h2, SS, w2, SS).mean(axis=(1, 3)), 0, 1)


def _down(big, S):
    h, w = big.shape[0] // S, big.shape[1] // S
    return big.reshape(h, S, w, S).mean(axis=(1, 3))


def _edges(a):
    """субпиксельные границы чернил исходного глифа (лево, верх, право, низ) в пикселях 1x"""
    px, py = a.max(axis=0), a.max(axis=1)
    thr = 0.04
    xs, ys = np.nonzero(px > thr)[0], np.nonzero(py > thr)[0]
    if not len(xs) or not len(ys):
        return None
    i0, i1, j0, j1 = xs[0], xs[-1], ys[0], ys[-1]
    return (i0 + 1 - min(px[i0], 1), j0 + 1 - min(py[j0], 1), i1 + min(px[i1], 1), j1 + min(py[j1], 1))


def _blur(a):
    a = np.pad(a, 2).astype(np.float32)
    for _ in range(2):
        a = (np.roll(a, 1, 0) + 2 * a + np.roll(a, -1, 0)) / 4
        a = (np.roll(a, 1, 1) + 2 * a + np.roll(a, -1, 1)) / 4
    return a


TRACE_K = 16       # увеличение растрового глифа при восстановлении контура


def score(big, orig, S=2):
    """(средняя ошибка, корреляция размытых) уменьшенного big против orig"""
    d = _down(np.clip(big, 0, 1), S)
    e = float(np.mean(np.abs(d - orig)))
    x, y = _blur(d).ravel(), _blur(orig).ravel()
    if x.std() < 1e-6 or y.std() < 1e-6:
        return e, 0.0
    return e, float(np.corrcoef(x, y)[0, 1])


def accepted(e, corr):
    return (e <= FIT_ERR and corr >= FIT_CORR) or (e <= FIT_ERR_HI and corr >= FIT_CORR_HI)


TRACE_THR = (0.5, 0.35, 0.65, 0.8)  # пороги изолинии (доля пика): выше — серые просветы не сливаются, ниже — тонкие штрихи не пропадают


def trace_master(glyph, thr=0.5):
    """векторный контур из растрового глифа (атлас игры той же гарнитуры): увеличение ×TRACE_K
    бикубикой и порог thr (доля пика глифа) — гладкая граница на месте изолинии покрытия.
    Формат как у master(): (картинка «F» 0..255 с полями, поле, ширина, высота чернил)."""
    from PIL import ImageFilter
    g = np.clip(np.asarray(glyph, np.float32), 0, 1)
    if g.max() < 0.05:
        return None
    h, w = g.shape
    k = TRACE_K
    pad = 2
    gp = np.pad(g, pad)
    up = np.asarray(Image.fromarray(gp, "F").resize(((w + 2 * pad) * k, (h + 2 * pad) * k), Image.BICUBIC), np.float32)
    mask = (up >= thr * float(g.max())).astype(np.float32) * 255.0
    im = Image.fromarray(mask, "F")
    bb = Image.fromarray((mask > 0).astype(np.uint8) * 255, "L").getbbox()
    if not bb:
        return None
    im = im.crop(bb)
    m = max(8, max(im.size) // 4)  # поля; за ними отрисовка считает «далеко снаружи»
    out = Image.new("F", (im.size[0] + 2 * m, im.size[1] + 2 * m), 0.0)
    out.paste(im, (m, m))
    return out, m, im.size[0], im.size[1]


def fit(orig, path, cid, S=2, axes=None, info=False, mst=None, force=False):
    """orig — маска глифа 1x (h×w, 0..1). Возвращает маску ×S той же ячейки или None.
    mst — готовый контур (trace_master) вместо шрифта; force — без проверки приёмки."""
    h, w = orig.shape
    if orig.max() < 0.05:
        return None
    if mst is None:
        if path is None:  # контур из растра не получился (пустой глиф)
            return None
        mst = master(path, cid, axes)
    if mst is None:
        return None
    e = _edges(orig)
    if e is None:
        return None
    rect = [v * S for v in e]
    w2, h2 = w * S, h * S

    def cost(big):
        """попиксельная ошибка + штраф за сдвиг границ чернил (размер буквы — строго оригинальный)"""
        d = _down(big, S)
        e1 = float(np.mean(np.abs(d - orig)))
        ed = _edges(d)
        dev = 9.0 if ed is None else sum(abs(a - b) for a, b in zip(ed, e))
        return e1 + EDGE_W * dev, e1

    ss = SS if max(w2, h2) <= SS_BIG else 2  # крупным буквам хватает 2×2: край задаёт поле расстояний
    # эталон как поле расстояний, уменьшенное так, чтобы на пиксель сетки ×S·SS приходилось ~1.5 пикселя поля
    sd = field(mst)
    _, m, mw, mh = mst
    q = 0.5 * (mw / max(rect[2] - rect[0], 0.5) + mh / max(rect[3] - rect[1], 0.5)) / ss
    fct = max(1, int(q / 1.5))
    if fct > 1:
        hh, ww = sd.shape[0] // fct * fct, sd.shape[1] // fct * fct
        sd = sd[:hh, :ww].reshape(hh // fct, fct, ww // fct, fct).mean(axis=(1, 3)) / fct
    fm = (np.ascontiguousarray(sd, np.float32), m / fct, mw / fct, mh / fct)
    dm = [0.0]  # смещение контура (пиксели поля)

    def err(r):
        f, k = _render_field(fm, r, w2, h2, ss)
        if f is None:
            return 9.0, None
        big = _shade(f, k, dm[0], w2, h2, ss)
        return cost(big)[0], big

    def descend(steps):
        nonlocal best_c, best, rect
        for step in steps:
            improved = True
            while improved:
                improved = False
                for k in range(4):
                    for d in (-step, step):
                        r = list(rect)
                        r[k] += d
                        c2, b2 = err(r)
                        if c2 < best_c - 1e-5:
                            best_c, best, rect, improved = c2, b2, r, True

    best_c, best = err(rect)
    if best is None:
        return None
    descend((1.0, 0.5, 0.25))  # покоординатный спуск по границам прямоугольника
    # толщина штриха: контур смещается (порог поля расстояний), пока масса чернил не совпадёт с оригиналом;
    # край остаётся векторно-резким
    f, k = _render_field(fm, rect, w2, h2, ss)
    per = ss * k  # пикселей поля на пиксель ×S
    target = float(orig.sum())
    lo, hi = -WEIGHT_MAX, WEIGHT_MAX
    for _ in range(12):
        d = 0.5 * (lo + hi)
        if float(_down(_shade(f, k, d * per, w2, h2, ss), S).sum()) > target:
            hi = d
        else:
            lo = d
    d = 0.5 * (lo + hi) * per
    b = _shade(f, k, d, w2, h2, ss)
    if cost(b)[0] < best_c:
        dm[0], best, best_c = d, b, cost(b)[0]
        descend((0.25, 0.125))
    else:
        descend((0.125,))
    best_e = cost(best)[1]
    x, y = _blur(_down(best, S)).ravel(), _blur(orig).ravel()
    if x.std() < 1e-6 or y.std() < 1e-6:
        return None
    corr = float(np.corrcoef(x, y)[0, 1])
    # та же форма (корреляция) и малая ошибка; при очень высокой корреляции допускаем больше
    # попиксельной ошибки — это хинтинг 1080p сдвинул штрихи, а не другая буква
    if not force and not ((best_e <= FIT_ERR and corr >= FIT_CORR) or (best_e <= FIT_ERR_HI and corr >= FIT_CORR_HI)):
        return None
    if info:  # масштаб контура по x и y (пикселей эталона на пиксель ×S) — для проверки пропорций
        _, _, mw, mh = mst
        return best, (mw / (rect[2] - rect[0]), mh / (rect[3] - rect[1]), mw, mh)
    return best
