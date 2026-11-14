"""Сравнение «как сейчас» (атлас 1080p, растянутый ×2) и «новый атлас ×2» на строке текста.
python tools/preview_fonts.py out.png font1.fnt [font2.fnt ...]  (сначала запустить make_fonts.py)"""
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_fonts as M  # noqa: E402

TEXT = "Willys MB (пул.) 85 ЛС / 6 КО Гранатометчик, мех. [USAGF] M3A1 Дальность: 60 м"


def render(at, pages, text, S, scale_pages):
    """pages: список float-страниц (уже ×S или 1x — тогда растягиваем билинейно, как GPU)"""
    lh = at.f["common"][0]
    width = sum(at.chars.get(ord(c), at.chars.get(32))[7] for c in text) + 8
    img = np.zeros((lh * S + 8, width * S + 8), np.float32)
    pen = 4
    for ch in text:
        c = at.chars.get(ord(ch))
        if not c:
            continue
        cid, x, y, w, h, xo, yo, adv, page, chnl = c
        if w and h:
            if scale_pages:
                g = at.glyph(c)[0]
                g = g[:, :, 3] if g.ndim == 3 else g
                big = M.resize(g, w * S, h * S)  # как билинейная выборка при растяжении ×2
            else:
                reg = pages[page][y * S:(y + h) * S, x * S:(x + w) * S]
                chans = M.CH.get(chnl, [3])
                big = reg[:, :, chans[0]] if len(chans) == 1 else reg[:, :, 3]
            X, Y = (pen + xo) * S, 4 + yo * S
            hh, ww = big.shape
            img[Y:Y + hh, X:X + ww] = np.maximum(img[Y:Y + hh, X:X + ww], np.clip(big, 0, 1))
        pen += adv
    return img


def apply_pat(page, b):
    import struct
    n = struct.unpack_from("<I", b, 4)[0]
    off = 8
    for _ in range(n):
        x, y, w, h, m = struct.unpack_from("<4HB", b, off)
        off += 12
        if m == 15:
            d = np.frombuffer(b, np.uint8, w * h * 4, off).reshape(h, w, 4)[:, :, [2, 1, 0, 3]] / 255
            page[y:y + h, x:x + w] = d
            off += w * h * 4
        else:
            d = np.frombuffer(b, np.uint8, w * h, off).reshape(h, w) / 255
            for bi, ci in ((0, 2), (1, 1), (2, 0), (3, 3)):
                if m & (1 << bi):
                    page[y:y + h, x:x + w, ci] = d
            off += w * h
    return page


def main():
    out, names = sys.argv[1], sys.argv[2:]
    pak = zipfile.ZipFile(M.GAME / "resource" / "interface" / "interface.pak")
    rows = []
    for n in names:
        at = M.Atlas(pak, n)
        new_pages = []
        for p in at.pages:
            h = M.fnv64_np(np.ascontiguousarray(p[:, :, [2, 1, 0, 3]]))
            rp = M.OUT / f"{h:016x}.raw"
            if rp.exists():
                a = np.frombuffer(rp.read_bytes(), np.uint8).reshape(p.shape[0] * 2, p.shape[1] * 2, 4)[:, :, [2, 1, 0, 3]]
                new_pages.append(a.astype(np.float32) / 255)
            else:  # .pat: увеличенная страница + чёткие глифы (как в DLL)
                new_pages.append(apply_pat(M.resize(p.astype(np.float32) / 255, p.shape[1] * 2, p.shape[0] * 2),
                                           (M.OUT / f"{h:016x}.pat").read_bytes()))
        rows.append(render(at, None, TEXT, 2, True))
        rows.append(render(at, new_pages, TEXT, 2, False))
    W = max(r.shape[1] for r in rows)
    canvas = np.zeros((sum(r.shape[0] for r in rows) + 6 * len(rows), W), np.float32)
    y = 0
    for r in rows:
        canvas[y:y + r.shape[0], :r.shape[1]] = r
        y += r.shape[0] + 6
    Image.fromarray((canvas * 255).astype(np.uint8)).save(out)


if __name__ == "__main__":
    main()
