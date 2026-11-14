"""Проверка готовых атласов ui4k_fonts: каждый глиф ×2, уменьшенный обратно, должен совпадать
с оригиналом 1080p (та же ячейка, то же положение, та же масса). Печатает худшие глифы.
  python tools/check_fonts.py [регулярка по имени .fnt]"""
import re
import sys
import zipfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_fonts as M  # noqa: E402
from preview_fonts import apply_pat  # noqa: E402

LIMIT = 0.10  # средняя ошибка глифа, выше которой глиф считается «не попавшим»


FONTS_DIR = M.OUT  # можно сверить другую сборку: --dir <папка ui4k_fonts>


def load_pages(at):
    pages = []
    for p in at.pages:
        h = M.fnv64_np(np.ascontiguousarray(p[:, :, [2, 1, 0, 3]]))
        raw, pat = FONTS_DIR / f"{h:016x}.raw", FONTS_DIR / f"{h:016x}.pat"
        if raw.exists():
            a = np.frombuffer(raw.read_bytes(), np.uint8).reshape(p.shape[0] * 2, p.shape[1] * 2, 4)[:, :, [2, 1, 0, 3]]
            pages.append(a.astype(np.float32) / 255)
        elif pat.exists():
            pages.append(apply_pat(M.resize(p.astype(np.float32) / 255, p.shape[1] * 2, p.shape[0] * 2), pat.read_bytes()))
        else:
            pages.append(None)
    return pages


SIZE_LIMIT = 0.25  # допуск по границам чернил (лево/верх/право/низ), пикселей 1080p


def check_one(n, fonts_dir=None):
    """(строка отчёта, глифов с ошибкой > LIMIT, глифов с отклонением размера > SIZE_LIMIT, всего, сумма ошибок)"""
    import ttf_fit as T
    global FONTS_DIR
    if fonts_dir:
        FONTS_DIR = Path(fonts_dir)
    pak = zipfile.ZipFile(M.GAME / "resource" / "interface" / "interface.pak")
    try:
        at = M.Atlas(pak, n)
    except KeyError:
        return None
    pages = load_pages(at)
    if all(p is None for p in pages):
        return None
    chars = at.f["chars"]
    if len(chars) > M.BIG_CHARS:
        chars = [c for c in chars if any(a <= c[0] < b for a, b in M.BIG_RANGES)]
    errs, sizes, grey = [], [], []  # grey: доля серых пикселей края в 4K (меньше — резче)
    for c in chars:
        cid, x, y, w, h, xo, yo, adv, pg, chnl = c
        if not (w and h) or pages[pg] is None:
            continue
        g = at.glyph(c)[0]
        reg = pages[pg][y * 2:(y + h) * 2, x * 2:(x + w) * 2]
        reg = reg[:, :, M.CH.get(chnl, [3])[0]] if g.ndim == 2 else reg
        errs.append((M.check(reg, g), cid))
        if g.ndim == 2:
            ink = reg[reg > 0.15]
            grey.append((float(np.mean(ink < 0.85)) if ink.size else 0.0, ink.size))
        if g.ndim == 2:
            e0, e1 = T._edges(g), T._edges(T._down(np.clip(reg, 0, 1), 2))
            if e0 and e1:
                sizes.append((max(abs(a - b) for a, b in zip(e0, e1)), cid))
    errs.sort(reverse=True)
    sizes.sort(reverse=True)
    bad = [e for e in errs if e[0] > LIMIT]
    bad_size = [e for e in sizes if e[0] > SIZE_LIMIT]
    nm = lambda c: chr(c) if c < 0x110000 else "<нет символа>"
    worst = " ".join(f"{nm(c)}:{e:.2f}" for e, c in errs[:5])
    worst_sz = " ".join(f"{nm(c)}:{e:.2f}" for e, c in sizes[:4])
    line = (f"  {n:<22} глифов {len(errs):4d}  средн. {np.mean([e for e, _ in errs]):.3f}  > {LIMIT}: {len(bad):3d}  "
            f"размер > {SIZE_LIMIT}px: {len(bad_size):3d}   худшие {worst}   размер {worst_sz}")
    gw = sum(n for _, n in grey)
    gr = sum(f * n for f, n in grey) / max(1, gw)
    line += f"   серость {gr:.3f}"
    return line, len(bad), len(bad_size), len(errs), sum(e for e, _ in errs), gr * gw, gw


def main():
    import os
    from concurrent.futures import ProcessPoolExecutor
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    fdir = None
    if "--dir" in args:
        k = args.index("--dir")
        fdir = args[k + 1]
        del args[k:k + 2]
    rx = args[0] if args else "."
    pak = zipfile.ZipFile(M.GAME / "resource" / "interface" / "interface.pak")
    names = [n for n in sorted(n[5:] for n in pak.namelist() if n.startswith("font/") and n.endswith(".fnt"))
             if re.search(rx, n)]
    tot = [0, 0, 0, 0.0, 0.0, 0]
    with ProcessPoolExecutor(os.cpu_count()) as ex:
        for r in ex.map(check_one, names, [fdir] * len(names)):
            if r is None:
                continue
            print(r[0])
            for k in range(6):
                tot[k] += r[k + 1]
    print(f"всего глифов {tot[2]}, средняя ошибка {tot[3] / max(1, tot[2]):.4f}; "
          f"ошибка > {LIMIT}: {tot[0]}; размер отличается > {SIZE_LIMIT}px: {tot[1]}; серость края {tot[4] / max(1, tot[5]):.3f}")


if __name__ == "__main__":
    main()
