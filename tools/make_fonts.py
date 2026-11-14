"""Готовит чёткие атласы шрифтов ×SCALE для ui4k (font_hd в d3d11.dll).

Раскладка каждой страницы сохраняется (глиф в той же доле текстуры), меняется только детальность.
Источник глифа по приоритету:
  1. векторный контур той же гарнитуры (tools/fonts или Windows/Fonts), подогнанный к
     оригинальному глифу (ttf_fit.py): размер и положение оригинальные, край векторный;
  2. родной атлас игры той же гарнитуры с кеглем ≈ SCALE×S (без обрезки по ячейке);
  3. sharp_up — Lanczos ×SCALE с восстановлением резкого края.
Каждый подставленный глиф проверяется: уменьшенный обратно, он должен совпасть с оригиналом,
иначе берётся вариант 3. Результат: <game>\\binaries\\x64\\ui4k_fonts\\<fnv64>.raw (BGRA, ×SCALE),
имя = хеш исходных пикселей страницы так, как их заливает игра.

  python tools/make_fonts.py [--big] [--clean]
--big   — включить страницы 2048x1024 (main = Arial Unicode); нужен font_big=1 в ui4k.ini.
Пишет в dll/out/ui4k_fonts; в игру копирует build.py --install.
"""
import argparse
import io
import re
import sys
import zipfile
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bmfont  # noqa: E402
import ttf_fit  # noqa: E402
from steam_game import find_game  # noqa: E402

GAME = find_game()  # GOH_DIR или поиск через Steam
OUT = Path(__file__).resolve().parents[1] / "out" / "ui4k_fonts"  # build.py --install копирует в игру
SCALE = 2
RATIO = (1.6, 2.6)          # допустимое отношение кегля родного источника к S
SHIFT_PENALTY = 0.006
CORE = list(range(0x21, 0x7F)) + list(range(0x410, 0x450)) + [0x401, 0x451]
CORE_SET = set(CORE)
CORE_MIN = 0.80             # доля основных символов, которые должны найтись в чётком источнике
ACCEPT_CORR = 0.75          # мин. корреляция с оригиналом (размытые) — отсев чужих глифов
ACCEPT_ERR = 0.22            # глиф по метрикам принимается, если не грубо отличается (не тот символ)
LOCAL_PENALTY = 0.02           # штраф за ±1 пиксель от общего сдвига
SHIFT = 3                  # поиск сдвига глифа ±SHIFT пикселей (в координатах ×SCALE)
SHARPEN = 1.9               # крутизна края при увеличении без источника (0 — просто Lanczos)
EDGE_KEEP = 0.8             # штрихи с пиком ниже — без усиления
BIG_CHARS = 2000            # в огромных атласах (main) чёткий источник ищем только для этих диапазонов:
BIG_RANGES = [(0x20, 0x530), (0x2010, 0x2070), (0x20AC, 0x20AD), (0x2116, 0x2117)]
BIG_JOBS = 8                # одновременных задач с огромными страницами (память)
SHAPE_W = 0.5               # вес несовпадения формы (1 - корреляция) при выборе между кандидатами
KIND_BIAS = {"vector": 0.0, "trace": 0.012, "self": 0.02}  # при равенстве — контур шрифта
ALT_CORR = 0.90             # непринятый векторный/trace-вариант годится в запас, если форма та же
MAX_ERR = 0.13             # допустимая средняя ошибка (0..1) уменьшенного глифа против оригинала
WINFONTS = Path(r"C:\Windows\Fonts")
PROBE = set(map(ord, "HOnoaeswxkgdpАБВЖМОРСабвежмнорсыя0123"))
WEIGHTS = (300, 350, 400, 450, 500, 600, 700)  # кандидаты веса для вариативных шрифтов
SHARP_MIN_H = 6             # глифы ниже (в пикселях 1x) не усиливаем: из 4-5 пикселей форму не восстановить
SHARP_ERR = 0.06            # макс. ошибка sharp_up-глифа против оригинала (тонкие штрихи мелких кеглей)
CLIP_MAX = 0.06             # доля изображения глифа, которую можно потерять за краем ячейки
CLIP_MEDIAN = 0.02          # источник, чьи глифы обрезаются систематически, крупнее нужного — не берём
FONTDIR = Path(__file__).resolve().parent / "fonts"  # свободные шрифты (OFL и т.п.), см. fonts/README.txt
# гарнитура .fnt -> (обычный, жирный); элемент — имя файла или (имя, {ось: значение}) для вариативных
FONTS = {
    "Arial": ("arial.ttf", "arialbd.ttf"),
    "Arial Unicode MS": ("arial.ttf", "arialbd.ttf"),
    "Impact": ("impact.ttf", "impact.ttf"),
    "Arial Narrow": ("arial.ttf", "arialbd.ttf"),  # подгонка сужает контур по ячейке
    "Oswald": (("Oswald-VF.ttf", {"Weight": 400}), ("Oswald-VF.ttf", {"Weight": 700})),
    "Oswald Light": (("Oswald-VF.ttf", {"Weight": 300}), ("Oswald-VF.ttf", {"Weight": 500})),
    "Open Sans Condensed Light": (("OpenSans-VF.ttf", {"Weight": 300, "Width": 75}),
                                  ("OpenSans-VF.ttf", {"Weight": 600, "Width": 75})),
    "PT Sans Narrow": ("PTSansNarrow-Regular.ttf", "PTSansNarrow-Bold.ttf"),
    "Roboto Condensed": (("RobotoCondensed-VF.ttf", {"Weight": 400}), ("RobotoCondensed-VF.ttf", {"Weight": 700})),
    "Roboto Mono": (("RobotoMono-VF.ttf", {"Weight": 400}), ("RobotoMono-VF.ttf", {"Weight": 700})),
    "Myriad Pro": (("SourceSans3-VF.ttf", {"Weight": 400}), ("SourceSans3-VF.ttf", {"Weight": 700})),  # клон того же автора
    "Anka/Coder Condensed": ("AnkaCoder-C87-r.ttf", "AnkaCoder-C87-b.ttf"),
    "Bookman Old Style": ("texgyrebonum-regular.otf", "texgyrebonum-bold.otf"),
}


def vec_for(face, bold):
    """(путь, оси) векторного шрифта гарнитуры или None"""
    e = FONTS.get(face)
    if not e:
        return None
    e = e[1 if bold else 0]
    name, axes = (e, None) if isinstance(e, str) else e
    for d in (FONTDIR, WINFONTS):
        if (d / name).exists():
            return d / name, (tuple(sorted(axes.items())) if axes else None)
    return None


CH = {1: [2], 2: [1], 4: [0], 8: [3], 15: [0, 1, 2, 3]}  # BMFont chnl -> индексы RGBA


def fnv64(b: bytes) -> int:
    x = 1469598103934665603
    for v in b:
        x ^= v
        x = (x * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return x


def fnv64_np(a: np.ndarray) -> int:
    # быстрый FNV-1a по байтам (векторизовать нельзя — последовательный; режем на куски)
    return fnv64(a.tobytes())


class Atlas:
    def __init__(self, pak, fnt):
        self.name = fnt
        self.f = bmfont.read(pak.read("font/" + fnt))
        self.size = abs(self.f["info"][0])
        self.base = self.f["common"][1]
        self.pages = [np.array(Image.open(io.BytesIO(pak.read("font/" + p))).convert("RGBA")) for p in self.f["pages"]]
        self.chars = {c[0]: c for c in self.f["chars"]}

    def glyph(self, c):
        """(маска float32 h×w или h×w×4, xoff, yoff_от_базовой линии)"""
        cid, x, y, w, h, xo, yo, adv, page, chnl = c
        if w == 0 or h == 0:
            return None
        im = self.pages[page][y:y + h, x:x + w].astype(np.float32) / 255.0
        chans = CH.get(chnl, [0, 1, 2, 3])
        g = im[:, :, chans[0]] if len(chans) == 1 else im
        return g, xo, yo - self.base


def resize(a, w, h):
    w, h = max(1, w), max(1, h)
    if a.ndim == 2:
        return np.asarray(Image.fromarray(a, "F").resize((w, h), Image.LANCZOS))
    return np.stack([resize(a[:, :, i], w, h) for i in range(a.shape[2])], axis=2)


def sharp_up(page):
    """увеличение ×SCALE с восстановлением резких краёв (для глифов без чёткого источника):
    Lanczos, затем контраст вокруг 0.5 — край снова ~1 пиксель, как у родной отрисовки ×2.
    Тонкие штрихи (пик ниже EDGE_KEEP) не трогаем, чтобы не пропали."""
    S = SCALE
    up = np.clip(resize(page, page.shape[1] * S, page.shape[0] * S), 0, 1)
    if not SHARPEN:
        return up
    hard = np.clip((up - 0.5) * SHARPEN + 0.5, 0, 1)
    # локальный максимум 1x-пикселя исходника, растянутый на ×S
    src = page if page.ndim == 3 else page[:, :, None]
    pk = src.copy()
    for ax in (0, 1):
        pk = np.maximum(pk, np.maximum(np.roll(pk, 1, ax), np.roll(pk, -1, ax)))
    pk = np.repeat(np.repeat(pk, S, 0), S, 1).reshape(up.shape)
    w = np.clip((pk - 0.45) / (EDGE_KEEP - 0.45), 0, 1)  # 0 — тонкий штрих, 1 — полноценный
    return up + (hard - up) * w


def place(canvas_shape, img, ox, oy, box):
    """Кладёт img в холст размера canvas_shape (h,w[,c]) с левым верхним углом (ox,oy), обрезая по box."""
    out = np.zeros(canvas_shape, np.float32)
    bx0, by0, bx1, by1 = box
    h, w = img.shape[:2]
    x0, y0 = max(ox, bx0), max(oy, by0)
    x1, y1 = min(ox + w, bx1), min(oy + h, by1)
    if x1 > x0 and y1 > y0:
        out[y0 - by0:y1 - by0, x0 - bx0:x1 - bx0] = img[y0 - oy:y1 - oy, x0 - ox:x1 - ox]
    return out


def check(big, orig):
    """средняя абсолютная ошибка big (×SCALE), уменьшенного усреднением блоков, против orig"""
    h, w = orig.shape[:2]
    S = SCALE
    b = np.clip(big, 0, 1)
    small = b.reshape(h, S, w, S, *b.shape[2:]).mean(axis=(1, 3))
    return float(np.mean(np.abs(small - orig)))


def same_glyph(big, orig):
    """тот ли это символ: корреляция слегка размытых изображений (сдвиги хинтинга не мешают)"""
    from PIL import ImageFilter
    h, w = orig.shape[:2]
    S = SCALE
    b = np.clip(big, 0, 1)
    small = b.reshape(h, S, w, S, *b.shape[2:]).mean(axis=(1, 3))
    if small.ndim == 3:
        small, o = small[:, :, 3], orig[:, :, 3]
    else:
        o = orig
    pad = lambda a: np.pad(a, 2)
    def blur(a):  # сепарабельное ядро [1 2 1]/4, дважды
        a = pad(a).astype(np.float32)
        for _ in range(2):
            a = (np.roll(a, 1, 0) + 2 * a + np.roll(a, -1, 0)) / 4
            a = (np.roll(a, 1, 1) + 2 * a + np.roll(a, -1, 1)) / 4
        return a
    x, y = blur(small).ravel(), blur(o).ravel()
    if x.std() < 1e-6 or y.std() < 1e-6:
        return float(np.abs(x - y).mean()) < 0.05
    return float(np.corrcoef(x, y)[0, 1]) >= ACCEPT_CORR


def from_source(src: Atlas, c, k):
    """глиф из атласа-источника, отмасштабированный в k раз: (img, xoff, yoff_от_базы)"""
    sc = src.chars.get(c[0])
    if not sc:
        return None
    g = src.glyph(sc)
    if g is None:
        return None
    img, xo, yo = g
    h, w = img.shape[:2]
    return resize(img, round(w * k), round(h * k)), xo * k, yo * k


def from_ttf(font: ImageFont.FreeTypeFont, cid):
    try:
        ch = chr(cid)
        l, t, r, b = font.getbbox(ch, anchor="ls")
    except Exception:
        return None
    if r <= l or b <= t:
        return None
    mask = Image.new("L", (r - l, b - t), 0)
    from PIL import ImageDraw
    ImageDraw.Draw(mask).text((-l, -t), ch, font=font, fill=255, anchor="ls")
    return np.asarray(mask, np.float32) / 255.0, l, t


def build_atlas(pak, at: Atlas, sources, vec, stats):
    """Каждый глиф — векторный контур, подогнанный к оригиналу (ttf_fit.fit). Источник контура:
      1. vector — файл шрифта той же гарнитуры (vec = (путь, оси) или None);
      2. trace  — контур, восстановленный из самого крупного атласа игры той же гарнитуры (sources);
      3. self   — контур, восстановленный из самого глифа (форма заведомо оригинальная).
    Ячейка, положение и масса чернил — оригинальные во всех трёх случаях."""
    S = SCALE
    f = at.f
    # фон страницы (служебные пиксели вне глифов) — простое увеличение; глифы перезаписываются
    out_pages = [np.clip(resize(p.astype(np.float32) / 255.0, p.shape[1] * S, p.shape[0] * S), 0, 1) for p in at.pages]
    chars = f["chars"]
    if len(chars) > BIG_CHARS:
        chars = [c for c in chars if any(lo <= c[0] < hi for lo, hi in BIG_RANGES)]
    picked = {}
    glyphs = [(c, at.glyph(c)[0]) for c in chars if c[3] and c[4]]
    # 1. файл шрифта
    if vec:
        axes = vec[1]
        if axes and dict(axes).get("Weight"):
            # вес вариативного шрифта подбираем по атласу: наименьшая ошибка+несовпадение формы на основных глифах
            probe = [(c, g) for c, g in glyphs if c[0] in PROBE and g.ndim == 2][:24]
            best = None
            for wgt in WEIGHTS:
                ax = tuple(sorted({**dict(axes), "Weight": wgt}.items()))
                n = 0.0
                for c, g in probe:
                    r = ttf_fit.fit(g, vec[0], c[0], S, ax, force=True)
                    e, corr = ttf_fit.score(r, g, S) if r is not None else (0.3, 0.0)
                    n += e + SHAPE_W * (1 - corr)
                if best is None or n < best[0]:
                    best = (n, ax)
            axes = best[1]
            stats["weight"] = dict(axes)["Weight"]
    # кандидаты каждого глифа: cid -> [(оценка, принят, результат, вид)]. Вектор из файла шрифта и контур
    # из крупного атласа игры считаются всегда: где шрифт-замена рисует букву иначе (перечёркнутый «0»
    # в Anka Coder), контур игры ближе к оригиналу и выигрывает.
    cands = defaultdict(list)

    def consider(c, g, r, kind):
        if r is None:
            return
        e, corr = ttf_fit.score(r, g, S)
        ok = ttf_fit.accepted(e, corr)
        if ok or corr >= ALT_CORR or kind == "self":
            cands[c[0]].append((e + SHAPE_W * (1 - corr) + KIND_BIAS[kind], ok, r, kind))

    for c, g in glyphs:
        if g.ndim != 2:  # цветной глиф (chnl 15) остаётся увеличенным
            continue
        if vec:
            consider(c, g, ttf_fit.fit(g, vec[0], c[0], S, axes, force=True), "vector")
        for src in sources:  # самый крупный атлас той же гарнитуры, где этот символ есть
            sc = src.chars.get(c[0])
            sg = src.glyph(sc) if sc else None
            if sg is not None and sg[0].ndim == 2:
                consider(c, g, ttf_fit.fit(g, None, c[0], S, mst=ttf_fit.trace_master(sg[0]), force=True), "trace")
                break
        if not any(ok for _, ok, _, _ in cands[c[0]]):
            # контур из самого глифа (форма заведомо оригинальная); порог изолинии — лучший из нескольких
            for thr in ttf_fit.TRACE_THR:  # запасные пороги — только если основной не прошёл приёмку
                consider(c, g, ttf_fit.fit(g, None, c[0], S, mst=ttf_fit.trace_master(g, thr), force=True), "self")
                if any(ok for _, ok, _, _ in cands[c[0]]):
                    break
        cs = cands.pop(c[0], [])
        good = [t for t in cs if t[1]] or cs
        if good:
            _, _, r, kind = min(good, key=lambda t: t[0])
            picked[c[0]] = (r, kind)
    gmap = {c[0]: g for c, g in glyphs}
    stats["err"] = sum(check(r, gmap[cid]) for cid, (r, _) in picked.items())
    for c in f["chars"]:
        if not (c[3] and c[4]):
            continue
        if c[0] in picked:
            r, kind = picked[c[0]]
            put(out_pages[c[8]], c, r)
            stats[kind] += 1
        else:
            stats["none"] += 1  # пустой или цветной глиф — остаётся увеличенным
    return out_pages, picked


def clip_frac(at, c, cand, center):
    """доля изображения глифа-кандидата за краем ячейки при укладке по метрикам"""
    S = SCALE
    cid, x, y, w, h, xo, yo, adv, page, chnl = c
    r = cand(c)
    if r is None:
        return None
    img, sx, sy = r
    im = img if img.ndim == 2 else img[:, :, 3]
    dx = round(sx - xo * S) + center[0]
    dy = round(sy - (yo - at.base) * S) + center[1]
    cb = place((h * S, w * S), im, dx, dy, (0, 0, w * S, h * S))
    t = float(np.clip(im, 0, 1).sum())
    return (t - float(np.clip(cb, 0, 1).sum())) / t if t > 0 else None


def try_glyph(at: Atlas, c, cand, only_err=False, center=(0, 0), radius=SHIFT, penalty=SHIFT_PENALTY,
              want_shift=False, max_err=MAX_ERR):
    S = SCALE
    cid, x, y, w, h, xo, yo, adv, page, chnl = c
    r = cand(c)
    if r is None:
        return None
    img, sx, sy = r
    orig = at.glyph(c)[0]
    if orig.ndim == 3 and img.ndim == 2:
        img = np.repeat(img[:, :, None], 4, axis=2)
    if orig.ndim == 2 and img.ndim == 3:
        img = img[:, :, 3]
    # положение относительно пера и базовой линии -> в координатах коробки ×S
    dx0 = round(sx - xo * S) + center[0]
    dy0 = round(sy - (yo - at.base) * S) + center[1]
    shape = (h * S, w * S) + ((4,) if orig.ndim == 3 else ())
    best = None
    ry = SHIFT if radius == SHIFT else 0  # по вертикали — только общий сдвиг (единая базовая линия)
    for dy in range(dy0 - ry, dy0 + ry + 1):  # хинтинг/округления сдвигают глиф на пиксель
        for dx in range(dx0 - radius, dx0 + radius + 1):
            cb = place(shape, img, dx, dy, (0, 0, w * S, h * S))
            raw = check(cb, orig)
            # штраф за уход от ожидаемой позиции: при близкой ошибке побеждает общий сдвиг
            e = raw + penalty * (abs(dx - dx0) + abs(dy - dy0))
            if best is None or e < best[0]:
                best = (e, cb, raw, (dx - dx0 + center[0], dy - dy0 + center[1]))
    _, big, err, shift = best
    if not (want_shift or only_err):
        # глиф крупного источника может не влезть в ячейку — обрезанный не берём
        tot = float(np.clip(img, 0, 1).sum())
        if tot > 0 and (tot - float(np.clip(big, 0, 1).sum())) / tot > CLIP_MAX:
            return None
    if want_shift:
        return err, shift
    if only_err:
        return err
    if max_err == ACCEPT_ERR:  # режим «по метрикам»: проверяем только, что символ тот же
        return big if same_glyph(big, orig) else None
    return big if err <= max_err else None


def put(page_img, c, big):
    S = SCALE
    cid, x, y, w, h, xo, yo, adv, pg, chnl = c
    chans = CH.get(chnl, [0, 1, 2, 3])
    region = page_img[y * S:(y + h) * S, x * S:(x + w) * S]
    if big.ndim == 2:
        for ch in chans:
            region[:, :, ch] = big
    else:
        region[:] = big


def make_patch(at, page, picked, out):
    """формат .pat: b"UI4P", u32 n; n раз: u16 x,y,w,h (×SCALE), u8 маска байтов BGRA, 3 байта 0,
    затем w*h байт (одно значение во все байты маски) или w*h*4 (маска 0xF, BGRA)"""
    import struct
    S = SCALE
    recs = []
    # латиница/кириллица целиком: чёткий источник или sharp_up (DLL сама умеет только билинейно)
    for c in at.f["chars"]:
        cid = c[0]
        if not (c[3] and c[4]) or not any(lo <= cid < hi for lo, hi in BIG_RANGES):
            continue
        _, x, y, w, h, xo, yo, adv, pg, chnl = c
        if pg != page:
            continue
        chans = CH.get(chnl, [0, 1, 2, 3])
        reg = out[y * S:(y + h) * S, x * S:(x + w) * S]
        if len(chans) == 1:
            mask = 1 << (2 - chans[0] if chans[0] < 3 else 3)
            data = np.ascontiguousarray(reg[:, :, chans[0]]).tobytes()
        else:
            mask = 0xF
            data = np.ascontiguousarray(reg[:, :, [2, 1, 0, 3]]).tobytes()
        recs.append(struct.pack("<4H4B", x * S, y * S, w * S, h * S, mask, 0, 0, 0) + data)
    return b"UI4P" + struct.pack("<I", len(recs)) + b"".join(recs)


_W = {}  # кеш процесса-исполнителя: pak и загруженные атласы


def _atlas(name):
    if "pak" not in _W:
        _W["pak"] = zipfile.ZipFile(GAME / "resource" / "interface" / "interface.pak")
    if name not in _W:
        _W[name] = Atlas(_W["pak"], name)
    return _W[name]


def build_one(name, src_names, big_page):
    """собирает один .fnt (в отдельном процессе): (имя, строка лога, [(хеш, качество, расширение, байты)])"""
    import time
    t0 = time.time()
    at = _atlas(name)
    sources = [_atlas(n) for n in src_names]
    face, bold = at.f["face"], name.endswith("b.fnt")
    vec = vec_for(face, bold)
    stats = defaultdict(int)
    pages, picked = build_atlas(_W["pak"], at, sources, vec, stats)
    # одна и та же страница бывает у двух .fnt (roboto_con_15/15b, main_12/15): main() оставит лучший прогон
    quality = (-len(picked), stats["err"])
    res = []
    for i, (orig, big) in enumerate(zip(at.pages, pages)):
        # байты так, как их заливает игра: BGRA, сверху вниз
        h = fnv64_np(np.ascontiguousarray(orig[:, :, [2, 1, 0, 3]]))
        out = np.clip(big * 255 + 0.5, 0, 255).astype(np.uint8)
        if big_page:  # огромные страницы: только заплатка с чёткими глифами, остальное DLL увеличит сама
            res.append((h, quality, "pat", make_patch(at, i, picked, out)))
        else:
            res.append((h, quality, "raw", np.ascontiguousarray(out[:, :, [2, 1, 0, 3]]).tobytes()))
    src_desc = ",".join(src_names) or "-"
    line = (f"  {name:<24} vec={vec[0].name if vec else '-'} src[{src_desc}] "
            f"vector={stats['vector']}" + (f"(w{stats['weight']})" if stats['weight'] else "")
            + f" trace={stats['trace']} self={stats['self']} none={stats['none']} "
            f"ошибка={stats['err'] / max(1, len(picked)):.3f} ({time.time() - t0:.0f} с)")
    return name, line, res


def main():
    import os
    import time
    from concurrent.futures import ProcessPoolExecutor, as_completed
    sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--big", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--only", help="регулярка по имени .fnt")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count(), help="процессов (по умолчанию все ядра)")
    a = ap.parse_args()
    t0 = time.time()
    pak = zipfile.ZipFile(GAME / "resource" / "interface" / "interface.pak")
    fnts = sorted(n[5:] for n in pak.namelist() if n.startswith("font/") and n.endswith(".fnt"))
    atlases = {}
    for n in fnts:
        try:
            atlases[n] = Atlas(pak, n)
        except KeyError as e:
            print(f"  {n}: {e} — пропуск")
    fam = defaultdict(list)  # (face, bold) -> atlases
    for at in atlases.values():
        fam[(at.f["face"], at.name.endswith("b.fnt"))].append(at)
    jobs = []
    for name, at in atlases.items():
        if a.only and not re.search(a.only, name):
            continue
        dims = at.pages[0].shape[1], at.pages[0].shape[0]
        if dims == (2048, 1024) and not a.big:
            continue
        if dims not in ((256, 256), (512, 256), (512, 512), (2048, 1024)):
            print(f"  {name}: страница {dims} — пропуск")
            continue
        face, bold = at.f["face"], name.endswith("b.fnt")
        # источники контура: атласы той же гарнитуры крупнее этого, самый крупный первым
        srcs = sorted((s for s in fam[(face, bold)] if s is not at and s.size > at.size), key=lambda s: -s.size)
        n_chars = len(at.f["chars"]) if len(at.f["chars"]) <= BIG_CHARS else 1300
        jobs.append((n_chars * at.size ** 2, name, [s.name for s in srcs[:2]], dims == (2048, 1024)))
    jobs.sort(reverse=True)  # тяжёлые первыми — пул загружен равномерно
    OUT.mkdir(parents=True, exist_ok=True)
    if a.clean:
        for p in [*OUT.glob("*.raw"), *OUT.glob("*.pat")]:
            p.unlink()
    done = {}  # хеш страницы -> качество записанного прогона
    written = 0
    # страницы 2048×1024 (main_*) берут до ~2 ГБ на процесс: их — отдельным пулом из BIG_JOBS процессов,
    # процесс после каждой такой задачи перезапускается (память возвращается системе)
    n_big = min(BIG_JOBS, max(1, a.jobs // 2))
    with ProcessPoolExecutor(max_workers=n_big, max_tasks_per_child=1) as ex_big,             ProcessPoolExecutor(max_workers=max(1, a.jobs - n_big)) as ex:
        futs = [(ex_big if big else ex).submit(build_one, name, srcs, big) for _, name, srcs, big in jobs]
        for k, fu in enumerate(as_completed(futs), 1):
            name, line, res = fu.result()
            print(f"[{k}/{len(futs)} {time.time() - t0:5.0f}с]" + line)
            for i, (h, quality, ext, data) in enumerate(res):
                if h in done and done[h] <= quality:
                    print(f"  {name}: страница {i} уже собрана лучше — пропуск")
                    continue
                done[h] = quality
                for old in OUT.glob(f"{h:016x}.*"):
                    old.unlink()
                (OUT / f"{h:016x}.{ext}").write_bytes(data)
                written += 1
    print(f"готово: {written} страниц в {OUT} за {time.time() - t0:.0f} с")


if __name__ == "__main__":
    main()
