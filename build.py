"""Сборка и установка прокси d3d11.dll (UI x2 при 4K-рендере) для Call to Arms - Gates of Hell.

  python build.py            собрать в out/
  python build.py --install  собрать и скопировать d3d11.dll (+ui4k.ini, если его нет) в игру
  python build.py --uninstall  удалить d3d11.dll и ui4k.ini из папки игры
Установка/удаление отказываются работать, пока call_to_arms.exe запущен.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
OUT = ROOT / "out"
sys.path.insert(0, str(ROOT / "tools"))
from steam_game import find_game  # noqa: E402


class _Game:
    """папка binaries\\x64 игры (GOH_DIR или поиск через Steam); ищется только при --install/--uninstall"""
    def __truediv__(self, other):
        return find_game() / "binaries" / "x64" / other

    def __str__(self):
        return str(find_game() / "binaries" / "x64")


GAME = _Game()
SYS_D3D11 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "d3d11.dll"
OWN = {"D3D11CreateDevice": "my_D3D11CreateDevice",
       "D3D11CreateDeviceAndSwapChain": "my_D3D11CreateDeviceAndSwapChain"}
CTX_THUNKS = 192  # >= слотов ID3D11DeviceContext4Vtbl (проверяется static_assert в scale_d3d.c)
SOURCES = ["proxy.c", "scale_d3d.c", "scale_input.c", "font_hd.c"]


def pe_exports(path):
    d = path.read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    osz = struct.unpack_from("<H", d, pe + 20)[0]
    erva = struct.unpack_from("<I", d, opt + 112)[0]
    secs = [struct.unpack_from("<8sIIII", d, opt + osz + i * 40) for i in range(nsec)]

    def off(r):
        for _, vs, va, rs, ro in secs:
            if va <= r < va + max(vs, rs):
                return r - va + ro
        raise ValueError(r)

    o = off(erva)
    _, nf, nn, af, an, ao = struct.unpack_from("<IIIIII", d, o + 16)
    names = []
    for i in range(nn):
        nr = struct.unpack_from("<I", d, off(an) + 4 * i)[0]
        names.append(d[off(nr):d.index(b"\0", off(nr))].decode())
    return names


def game_running():
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq call_to_arms.exe", "/NH"],
                       capture_output=True, text=True)
    return "call_to_arms.exe" in r.stdout.lower()


def generate():
    names = pe_exports(SYS_D3D11)
    stubs = [n for n in names if n not in OWN]
    gen = OUT / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "exports.inc").write_text("".join(f"X({n})\n" for n in stubs))
    asm = [".text"]
    for i, n in enumerate(stubs):
        asm += [f".globl stub_{i}", f"stub_{i}:", f"    jmpq *g_real+{8 * i}(%rip)"]
    # переходники обёртки контекста: this -> настоящий контекст, прыжок в его vtable[i]
    for i in range(CTX_THUNKS):
        asm += [f".globl ctx_thunk_{i}", f"ctx_thunk_{i}:", "    movq 8(%rcx), %rcx",
                "    movq (%rcx), %rax", f"    jmpq *{8 * i}(%rax)"]
    asm += [".data", ".p2align 3", ".globl g_ctx_thunks", "g_ctx_thunks:"]
    asm += [f"    .quad ctx_thunk_{i}" for i in range(CTX_THUNKS)]
    asm += ['.section .drectve,"yn"']
    for i, n in enumerate(stubs):
        asm.append(f'    .ascii " -export:{n}=stub_{i}"')
    for n, impl in OWN.items():
        asm.append(f'    .ascii " -export:{n}={impl}"')
    (gen / "stubs.S").write_text("\n".join(asm) + "\n")
    return gen, len(names)


def build():
    gen, n = generate()
    dll = OUT / "d3d11.dll"
    cmd = [sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", "-shared", "-O2",
           "-Wall", "-Wno-unused-function", "-I", str(SRC), "-I", str(gen),
           *[str(SRC / s) for s in SOURCES], str(gen / "stubs.S"),
           "-o", str(dll), "-luser32", "-lgdi32", "-lkernel32", "-ldxguid", "-luuid"]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    exp = pe_exports(dll)
    missing = set(pe_exports(SYS_D3D11)) - set(exp)
    if missing:
        sys.exit(f"ошибка: в собранной DLL нет экспортов {sorted(missing)}")
    print(f"OK: {dll} ({dll.stat().st_size} байт, экспортов {len(exp)}/{n})")
    return dll


def install(dll):
    if game_running():
        sys.exit("call_to_arms.exe запущен — закройте игру перед установкой")
    shutil.copy2(dll, GAME / "d3d11.dll")
    ini = GAME / "ui4k.ini"
    if not ini.exists():
        shutil.copy2(ROOT / "ui4k.ini", ini)
    fonts = OUT / "ui4k_fonts"  # чёткие атласы шрифтов (tools/make_fonts.py)
    if fonts.is_dir():
        dst = GAME / "ui4k_fonts"
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(fonts, dst)
        print(f"атласы шрифтов: {len(list(dst.glob('*.raw')))} шт., заплатки: {len(list(dst.glob('*.pat')))} шт.")
    print(f"установлено в {GAME}")


def uninstall():
    if game_running():
        sys.exit("call_to_arms.exe запущен — закройте игру перед удалением")
    for f in ("d3d11.dll", "ui4k.ini", "ui4k.log"):
        p = GAME / f
        if p.exists():
            p.unlink()
            print(f"удалён {p}")
    for d in ("ui4k_fonts", "ui4k_dump"):
        p = GAME / d
        if p.exists():
            shutil.rmtree(p)
            print(f"удалена папка {p}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--uninstall", action="store_true")
    a = ap.parse_args()
    if a.uninstall:
        uninstall()
    else:
        d = build()
        if a.install:
            install(d)
