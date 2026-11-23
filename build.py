"""GoH UI4K v2: сборка, тесты, установка прокси d3d11.dll.

  python build.py              собрать в out/
  python build.py --test       собрать и прогнать тесты без игры (tests/)
  python build.py --install    собрать и поставить в игру (бэкап того, что заменяется)
  python build.py --uninstall  убрать мод и вернуть файлы из бэкапа
  python build.py --package    собрать архив для игроков: out/package/GoH-UI4K-<версия>.zip (только файлы)

Нужен Python с пакетом ziglang (pip install ziglang). Папка игры: GOH_DIR или поиск через Steam.
Установка и удаление отказываются работать, пока call_to_arms.exe из этой папки запущен.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"
OUT = ROOT / "out"
TESTS = ROOT / "tests"
sys.path.insert(0, str(ROOT / "tools"))
from steam_game import find_game  # noqa: E402

SYS_D3D11 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "d3d11.dll"
OWN = {"D3D11CreateDevice": "my_D3D11CreateDevice",
       "D3D11CreateDeviceAndSwapChain": "my_D3D11CreateDeviceAndSwapChain"}
CTX_THUNKS = 192  # >= слотов ID3D11DeviceContext4Vtbl (static_assert в ctx.c)
SOURCES = ["proxy.c", "config.c", "log.c", "scale.c", "vt.c", "ctx.c", "device.c", "screen.c", "classify.c",
           "resources.c", "swapchain.c", "iat.c", "overlay.c", "viewport.c",
           "cbfix.c", "cb.c", "fmt.c", "upload.c", "profile.c", "dpi.c", "input.c", "copy.c", "cursor.c", "modes.c", "diag.c", "draw.c", "lum.c", "loc.c", "world.c",
           "dxbc.c", "pstag.c", "dof.c", "box.c", "rtti.c", "views.c", "gamedata.c", "anchors.c", "d3dbg.c", "perf.c", "origin.c", "fontpages.c", "fontpages_core.c", "crash.c"]
HLSL = {"lum_tile": "lum_tile.hlsl", "lum_hist": "lum_hist.hlsl"}  # compute-шейдеры -> байткод в gen/lum_cs.h
CC = [sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu"]
VERSION = "2.1-alpha2"
INSTALLED = ["d3d11.dll", "ui4k.ini", "ui4k_fonts2.dat", "ui4k_fonts4.dat"]   # что кладём в binaries\x64
FONT_DATA = OUT / "fontpages"                # образцы букв Э6 (файлы ui4k_fonts*.dat из архива выпуска)
LEFTOVERS = ["ui4k.log", "ui4k.2.log", "ui4k.3.log", "ui4k.4.log", "ui4k_cs.txt", "ui4k_vs.txt", "ui4k_labels.pak"]  # что создаёт мод при работе


def cache_name(shader):
    """путь записи в кэше шейдеров игры: MD5 от имени шейдера в нижнем регистре (src/gamedata.h)"""
    import hashlib
    return "shader/cache/dx11.0/" + hashlib.md5(shader.lower().encode()).hexdigest()


def cache_entry(shader, code):
    """содержимое записи кэша: байт длины имени, имя, байткод DXBC"""
    return bytes([len(shader)]) + shader.encode() + code


def write_fake_pak(path, extra=()):
    """архив шейдеров «игры после обновления»: те же имена, другой байткод (tests/test_gamedata.c)"""
    import zipfile
    dxbc = b"DXBC" + b"NEWSHADER-UPDATE" + struct.pack("<III", 1, 32, 0)
    ui = ["vs[VS]$VCOLOR$VTEXCOORDS#0$TEXCOORDS#0$USE_VCOLOR", "vs[VS]$TEXCOORDS#1$VTEXCOORDS#1$USE_VCOLOR$VNORMAL$VCOLOR",
          "vs[VS]$VNORMAL$VCOLOR$VTEXCOORDS#1$TEXCOORDS#1$USE_VCOLOR"]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        z.writestr("shader/dx10/shader_constants.h", "// game after an update\n#define pscSceneGroup\t_PSG(pscgScene, 40)\n"
                   "#define pscDOFParams\t\t_PSC(pscgScene, 18)\n#define pscLELightPos\t_PSC(pscgScene, 5)\n")
        # Э3: места экранных чисел по именам переменных — регистры переехали (c2.x, pscLELightPos.z, pscDOFParams.y)
        z.writestr("shader/dx10/postprocess/copy.hlsl", "float2 src_size : packoffset(c2.x);\n")
        z.writestr("shader/dx10/postprocess/lenseffects/lens_effects.hlsl", "float2 textureDims_LR\t: packoffset(pscLELightPos.z);\n")
        z.writestr("shader/dx10/postprocess/dof/dof_blur.hlsl", "float2\t\t  textureDimensions\t\t: packoffset(pscDOFParams.y);\n")
        z.writestr(cache_name(ui[0]) + ".vso", cache_entry(ui[0], dxbc))
        z.writestr(cache_name(ui[1]) + ".vso", cache_entry("vs[VS]$SOMETHING_ELSE", dxbc))   # чужое имя внутри
        z.writestr(zipfile.ZipInfo(cache_name(ui[2]) + ".vso"), cache_entry(ui[2], dxbc), compress_type=zipfile.ZIP_DEFLATED)
        for name, data in extra:
            z.writestr(name, data)


def user_dir():
    """запасная папка пакета подписей (папка игры только для чтения), см. loc.c"""
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))) / "ui4k"
BACKUP_DIR = "ui4k_backup"


# ---------------- сборка ----------------
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


def generate():
    names = pe_exports(SYS_D3D11)
    stubs = [n for n in names if n not in OWN]
    gen = OUT / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "exports.inc").write_text("".join(f"X({n})\n" for n in stubs))
    asm = [".text"]
    for i, n in enumerate(stubs):
        asm += [f".globl stub_{i}", f"stub_{i}:", f"    jmpq *g_real+{8 * i}(%rip)"]
    # переходники обёртки контекста: this -> настоящий контекст (+8), прыжок в его vtable[i]
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
    gen_shaders(gen)
    return gen, len(names)


def gen_shaders(gen):
    """HLSL -> байткод cs_5_0 системным d3dcompiler_47 (тот же 10.1, что у игры), в gen/lum_cs.h"""
    import ctypes
    from ctypes import wintypes as W
    dc = ctypes.WinDLL(str(SYS_D3D11.parent / "d3dcompiler_47.dll"))
    comp = dc.D3DCompile
    comp.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                     ctypes.c_char_p, ctypes.c_char_p, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_void_p),
                     ctypes.POINTER(ctypes.c_void_p)]
    comp.restype = ctypes.c_long

    def blob(p):  # ID3DBlob: vtable [QI, AddRef, Release, GetBufferPointer, GetBufferSize]
        vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(p)
        n = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(p)
        data = ctypes.string_at(ptr, n)
        ctypes.WINFUNCTYPE(W.ULONG, ctypes.c_void_p)(vt[2])(p)
        return data

    out = ["/* generated by build.py from src/lum_*.hlsl, do not edit */"]
    for name, f in HLSL.items():
        src = (SRC / f).read_bytes()
        code, err = ctypes.c_void_p(), ctypes.c_void_p()
        hr = comp(src, len(src), f.encode(), None, None, b"main", b"cs_5_0", 1 << 15, 0, ctypes.byref(code),
                  ctypes.byref(err))  # 1<<15 = D3DCOMPILE_OPTIMIZATION_LEVEL3
        if hr < 0 or not code.value:
            sys.exit(f"ошибка HLSL {f}: {blob(err).decode(errors='replace') if err.value else hex(hr & 0xffffffff)}")
        data = blob(code)
        out.append(f"static const BYTE {name}[{len(data)}] = {{" + ",".join(str(b) for b in data) + "};")
    (gen / "lum_cs.h").write_text("\n".join(out) + "\n")


def version():
    """2.0-<дата правки>-<номер правки>; «+» — собрано с несохранёнными в git изменениями. Видна в ui4k.log и в имени архива"""
    def git(*a):
        r = subprocess.run(["git", *a], cwd=ROOT, capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else ""
    h, d = git("rev-parse", "--short", "HEAD"), git("log", "-1", "--format=%cs")
    if not h:
        return VERSION
    return f"{VERSION}-{d.replace('-', '.')}-{h}" + ("+" if git("status", "--porcelain", "--untracked-files=no") else "")


def build(pdb=False):
    """pdb — с файлом символов out/d3d11.pdb: имена функций мода для профилировщика (perf\\goh_prof.exe).
    Машинный код тот же (-g его не меняет), но в DLL записан путь к .pdb — такую сборку игрокам не давать"""
    gen, n = generate()
    dll = OUT / "d3d11.dll"
    cmd = [*CC, "-shared", "-O2", *(["-g"] if pdb else []), "-Wall", "-Wextra", "-Wno-unused-parameter", f'-DUI4K_VERSION="{version()}"',
           "-I", str(SRC), "-I", str(gen),
           *[str(SRC / s) for s in SOURCES], str(gen / "stubs.S"),
           "-o", str(dll), "-luser32", "-lkernel32", "-lgdi32", "-ldxguid", "-luuid", "-lshell32", "-lbcrypt"]
    subprocess.run(cmd, check=True)
    exp = pe_exports(dll)
    missing = set(pe_exports(SYS_D3D11)) - set(exp)
    if missing:
        sys.exit(f"ошибка: в собранной DLL нет экспортов {sorted(missing)}")
    print(f"OK: {dll} ({dll.stat().st_size} байт, экспортов {len(exp)}/{n})")
    return dll


# ---------------- тесты ----------------
def run_tests(dll, debug=False):
    tdir = OUT / "tests"
    tdir.mkdir(parents=True, exist_ok=True)
    fails = 0
    dbg = {"tests": 0, "lines": 0, "missing": False}   # --debug-layer: сообщения слоя отладки Direct3D по тестам
    if debug:
        os.environ["UI4K_D3D_DEBUG"] = "1"

    def compile_(name, *srcs, libs=()):
        exe = tdir / f"{name}.exe"
        subprocess.run([*CC, "-O1", "-Wall", "-I", str(SRC), *map(str, srcs), "-o", str(exe), *libs], check=True)
        return exe

    def run(name, *args, known=None):
        """known — номер находки ревизии, которую тест воспроизводит и которая ещё не исправлена: провал ожидаем
        (KNOWN), а вот неожиданный успех — ошибка: пометку пора снять"""
        nonlocal fails
        r = subprocess.run(list(map(str, args)), capture_output=True, text=True, errors="replace")
        out = (r.stdout + r.stderr).strip().splitlines()
        ok = r.returncode == 0 and out and out[-1].startswith("PASS")
        if known:
            print(f"{'FAIL (исправлено? снимите пометку known)' if ok else 'KNOWN'}  {name} [{known}]")
            fails += bool(ok)
            for line in ([] if ok else [l for l in out if l.startswith(("FAIL line", "CRASH"))][:4]):
                print("      " + line)
            return
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if debug:   # что нашёл слой отладки в логах этого теста
            found = []
            for a in args:
                lp = Path(str(a)) / "ui4k.log"
                if Path(str(a)).is_dir() and lp.exists():
                    for l in lp.read_text(errors="replace").splitlines():
                        if "d3d debug: " in l and ("ERROR" in l or "CORRUPTION" in l or "WARNING" in l):
                            found.append(l.split("d3d debug: ", 1)[1])
                        dbg["missing"] |= "debug layer is not installed" in l
            if found:
                dbg["tests"] += 1
                dbg["lines"] += len(found)
                for l in found[:8]:
                    print("      [d3d] " + l[:220])
        if not ok:
            fails += 1
            for line in out[-12:]:
                print("      " + line)
            if r.returncode not in (0, 1, 2, 3):   # тест упал, а не сообщил о провале
                code = r.returncode & 0xFFFFFFFF
                what = {0xC00000FD: "исчерпан стек (бесконечная рекурсия)", 0xC0000005: "нарушение доступа"}.get(code, "")
                print(f"      тест упал: код 0x{code:08X} {what}")

    run("scale (дробный масштаб, округление)", compile_("test_scale", TESTS / "test_scale.c", SRC / "scale.c"))
    run("classify (какие текстуры масштабируются)",
        compile_("test_classify", TESTS / "test_classify.c", SRC / "classify.c", SRC / "scale.c"))
    # Э2: поиск «t:create» на искусственном образе и в настоящем exe игры (если она установлена)
    try:
        real_exe = find_game() / "binaries" / "x64" / "call_to_arms.exe"
    except (SystemExit, Exception):
        real_exe = None
    fp_dat = FONT_DATA / "ui4k_fonts2.dat"
    run("fontpages: sample file, page fingerprint, own letter cells: atlas, xk from samples, engine strips (E6)",
        compile_("test_fontpages", TESTS / "test_fontpages.c", SRC / "fontpages_core.c"), *([str(fp_dat)] if fp_dat.exists() else []))
    fd_exe = compile_("test_fontdll", TESTS / "test_fontdll.c", SRC / "fontpages_core.c", libs=("-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # ревизия v3, пп. 1, 4, 5, 6
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("fonts through the DLL: own letters with other keys off, a changed page refused by CRC32, no copy of a "
            "deferred-context vertex buffer, the waiting page held (review v3)", fd_exe, t)
    of_exe = compile_("test_origin_find", TESTS / "test_origin_find.c", SRC / "origin.c")
    run("origin: \"t:create\" -> reference -> function by unwind table (E2)", of_exe,
        *([real_exe] if real_exe and real_exe.exists() else []))
    og_exe = compile_("test_origin", TESTS / "test_origin.c", libs=("-ldxgi", "-ldxguid", "-luuid"))
    for mode, name in (("", "window caches of any shape made by \"t:create\" are enlarged, others by shape (E2)"),
                       ("off", "origin=0: shape rule only, no search (E2)")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            run(name, og_exe, t, *([mode] if mode else []))
    run("cbfix (экранные пары в CB)", compile_("test_cbfix", TESTS / "test_cbfix.c", SRC / "cbfix.c"))
    run("dxbc: constant-buffer variable by name from shader reflection (G4)",
        compile_("test_dxbc", TESTS / "test_dxbc.c", SRC / "dxbc.c", libs=("-ldxguid", "-luuid")))
    run("rtti: game classes by MSVC RTTI name, their objects, the handler's gate byte (E9)",
        compile_("test_rtti", TESTS / "test_rtti.c", SRC / "rtti.c"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # устойчивость к обновлениям: архив шейдеров игры
        fake = Path(t) / "shader.pak"
        write_fake_pak(fake)
        args = [str(fake)]
        try:
            real = find_game() / "resource" / "shader.pak"
            if real.exists():
                args.append(str(real))
        except (SystemExit, Exception):
            pass
        run("game shader archive: shaders found by name, scene buffer size (updated game and the real one)",
            compile_("test_gamedata", TESTS / "test_gamedata.c", SRC / "gamedata.c", libs=("-lbcrypt",)), *args)
    run("fmt (форматы пикселей, пересчёт картинки процессором)", compile_("test_fmt", TESTS / "test_fmt.c", SRC / "fmt.c"))
    run("profile (настройки HDR и MSAA из профиля игры)",
        compile_("test_profile", TESTS / "test_profile.c", SRC / "profile.c"))
    run("exposure shaders read the whole scene (lum.c)",
        compile_("test_lum", TESTS / "test_lum.c", "-I", OUT / "gen", libs=("-ld3d11", "-ldxguid", "-luuid")))

    # swapchain и текстуры (F04/F05) при ×1.5
    res_exe = compile_("test_res", TESTS / "test_res.c", libs=("-ldxgi", "-ldxguid", "-luuid", "-luser32"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\n")   # ровно 3/2 на любом мониторе
        run("swapchain + textures + CB x1.5 (F04/F05/F08)", res_exe, t)
        # В1: мод объявил процесс понимающим DPI и пишет настоящий масштаб Windows (а не 100% непонимающего процесса)
        u32 = ctypes.WinDLL("user32")
        u32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        old = u32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        real_scale = u32.GetDpiForSystem() * 100 // 96
        u32.SetThreadDpiAwarenessContext(ctypes.c_void_p(old))
        m = re.search(r"dpi: process (?:was \S+, now|is) (.+?)(?: - could not change it)? \(Windows scale (\d+)%\)",
                      (Path(t) / "ui4k.log").read_text(errors="replace"))
        ok = bool(m) and "per-monitor" in m[1] and int(m[2]) == real_scale
        print(f"{'PASS' if ok else 'FAIL'}  dpi: process DPI aware, log shows the real Windows scale {real_scale}% "
              f"({m[0] if m else 'no dpi line'})")
        fails += not ok
        vp_exe = compile_("test_vp", TESTS / "test_vp.c", libs=("-ldxguid", "-luuid"))
        run("viewport/scissor x1.5 (F07, рисование)", vp_exe, t)
        in_exe = compile_("test_input", TESTS / "test_input.c", libs=("-luser32", "-ldxguid", "-luuid"))
        run("input/windows x1.5 (F10)", in_exe, t)
        cp_exe = compile_("test_copy", TESTS / "test_copy.c", libs=("-ldxguid", "-luuid"))
        run("copy/stretch x1.5 (F09)", cp_exe, t)
        cur_exe = compile_("test_cursor", TESTS / "test_cursor.c", libs=("-luser32", "-lgdi32"))
        run("cursor + splash x1.5 (F11)", cur_exe, t, 3, 2)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # курсор при целом x2 — повтор пикселей
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=2\nlog=1\nfit_native=0\n")
        run("cursor + splash x2 (F11)", cur_exe, t, 2, 1)

    # ревизия, вылет при MSAA в игре 17.11: второй swapchain и чужой перехватчик тех же функций (оверлей Steam)
    hc_exe = compile_("test_hookcycle", TESTS / "test_hookcycle.c", libs=("-ldxgi", "-ldxguid", "-luuid", "-luser32"))
    for mode, title in (("below", "other hooker below the mod, as the Steam overlay"), ("above", "other hooker above the mod")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@scale=1.5@log=1@fit_native=0@"
            (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
            run(f"second swap chain does not loop the hooks: {title} (MSAA crash)", hc_exe, t, mode)

    # ревизия, В1: мод сам объявляет процесс понимающим DPI
    dpi_exe = compile_("test_dpi", TESTS / "test_dpi.c", libs=("-luser32",))
    for mode, extra in (("on", ""), ("off", "dpi_aware=0@")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@scale=1.5@log=1@fit_native=0@" + extra
            (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
            run(f"dpi awareness set by the mod: {mode} (B1)", dpi_exe, t, mode)

    # ревизия, В0/В3: граница «процессор <-> видеокарта» и вызовы с несходящимися настоящими размерами
    bd_exe = compile_("test_boundary", TESTS / "test_boundary.c", libs=("-ldxgi", "-ldxguid", "-luuid", "-luser32"))
    for mode, title, known in (
            ("upload", "game uploads pixels into an enlarged texture (the MSAA crash, B0)", None),
            ("readback", "screenshot / save preview: staging textures stay logical (B3)", None),
            ("copy", "copies between textures of different real size", None),
            ("resolve", "MSAA resolve into an enlarged pair and back", None),
            ("targets", "render target and depth of different real size", None),
            ("depthcopy", "game copies a whole depth buffer between enlarged textures (G2, real video card)", None),
            ("depthcopy_warp", "the same whole depth copy on the software device", None),
            ("binds", "a copy drawn by the mod keeps the game's shader and UAV bindings (code review 1)", None),
            ("pool", "temporary textures of converted copies are reused and released when unused (code review 5)", None),
            ("deferred", "region upload in a deferred context without driver command lists (WARP, some AMD / Intel; code review 2)", None)):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=2\nfit_native=0\n")
            run(f"boundary {mode}: {title}", bd_exe, t, mode, known=known)

    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # С6: лог на уровне log=1
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@scale=1.5@log=1@fit_native=0@"
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("log=1 does not grow by a line per created texture (C6)", bd_exe, t, "loglimit")

    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # HDR и MSAA меняются в игре без перезапуска
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "prof" / "1").mkdir(parents=True)
        (Path(t) / "prof" / "1" / "options.set").write_text("{options {video {hdr} {antialiasing none}}}")
        ini = "[ui4k]@enabled=1@scale=1.5@log=1@fit_native=0@presets=0@render=50@test_profiles=" + str(Path(t) / "prof") + "@"
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("HDR and MSAA switched in the game without restart are followed (profile re-read)", bd_exe, t, "profile")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # то же с безопасным режимом (msaa_safe=1)
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "prof" / "1").mkdir(parents=True)
        (Path(t) / "prof" / "1" / "options.set").write_text("{options {video {hdr} {antialiasing none}}}")
        ini = "[ui4k]@enabled=1@scale=1.5@log=1@fit_native=0@presets=0@render=50@msaa_safe=1@test_profiles=" + str(Path(t) / "prof") + "@"
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("msaa_safe=1: MSAA switched on in the game puts the world at interface size", bd_exe, t, "profile_safe")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # П7: HDR включён + MSAA, мир меньше экрана
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        ini = "[ui4k]@game_hdr=1@game_aa=4@enabled=1@scale=1.5@log=1@fit_native=0@presets=0@render=50@"
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("HDR on + MSAA with a smaller world: multisampled world targets at world size (P7)", bd_exe, t, "hdrmsaa")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # ревизия кода пп. 3–4: лог и пакет подписей
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        ini = ("[ui4k]@game_hdr=1@game_aa=0@enabled=1@log=1@presets=1@labels=1@fit_native=1@test_monitor=3840x2160@"
               "test_appdata=" + str(Path(t) / "appdata") + "@")
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("labels pak written next to the DLL (code review 3)", bd_exe, t, "labels")
        run("second copy of the game: own log, first log untouched, labels from the pak already there (code review 4)",
            bd_exe, t, "second")
        (Path(t) / "ui4k_labels.pak").unlink()
        (Path(t) / "ui4k_labels.pak").mkdir()   # записать рядом с DLL нельзя
        run("game folder not writable: labels pak in the user folder (code review 3)", bd_exe, t, "labels_ro")
    for exp, mode, title in ((None, "dof", "radius x s^2 by default (as the game at the world resolution)"),
                             (None, "dofx", "nine blur shaders alive: the first is still recognised (code review 4)"),
                             (None, "dofbig", "scene buffer of 300 floats after a game update (code review 4)"),
                             (1, "dof1", "dof_radius=1: radius x s (as the game at the logical resolution)"),
                             (0, "dof0", "dof_radius=0: left as is")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # Г4: радиус размытия глубины резкости
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@scale=1.5@log=1@fit_native=0@" + ("" if exp is None else f"dof_radius={exp}@")
            ini += "cb_fix=300,84@" if mode == "dofbig" else ""
            (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
            run(f"depth of field blur: {title} (G4)", bd_exe, t, mode)
    for mode, extra, title in (("box", "", "copy shader steps by the real texels of an enlarged source"),
                               ("box0", "box_step=0@", "box_step=0: the game's step")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # Э8: шаг уменьшения иконок инвентаря
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@scale=1.5@log=1@fit_native=0@" + extra
            (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
            run(f"inventory icons: {title} (E8)", bd_exe, t, mode)

    # F03: список режимов настоящего монитора (без монитора — пропуск)
    md_exe = compile_("test_modes", TESTS / "test_modes.c", SRC / "scale.c",
                      libs=("-ldxgi", "-luser32", "-ldxguid", "-luuid"))
    for ini, args, title in (("scale=1.5", (3, 2), "x1.5"), ("scale=2", (2, 1), "x2"),
                             ("scale=1.5\nfit_native=0", (3, 2, "--off"), "fit_native=0")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            (Path(t) / "ui4k.ini").write_text(f"[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\n{ini}\nlog=1\npresets=0\n")
            run(f"display modes {title} (F03)", md_exe, t, *args)

    # L8: список разрешений = строки «мир + масштаб», выбор строки переключает оба
    ps_exe = compile_("test_presets", TESTS / "test_presets.c", libs=("-luser32", "-lgdi32", "-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "prof").mkdir()   # свой пустой профиль: не профиль игрока на этом ПК (его строка влияет на запуск)
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=2\nall_monitors=0\n"
                                          f"test_profiles={Path(t) / 'prof'}\n")
        run("presets: world + interface per entry (L8)", ps_exe, t)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # 19.11: запуск — наименьшая строка, затем строка профиля
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "prof" / "1").mkdir(parents=True)
        (Path(t) / "prof" / "1" / "options.set").write_text(
            '{options\n\t{video\n\t\t{resolution\n\t\t\t{custom_mode "1280x720"}\n\t\t\t{mode "1920x1080"}\n\t\t}\n'
            '\t\t{hdr 1}\n\t\t{antialiasing none}\n\t}\n}\n')
        (Path(t) / "ui4k.ini").write_text("[ui4k]\nenabled=1\nlog=2\ntest_monitor=3840x2160\nall_monitors=0\n"
                                          f"test_profiles={Path(t) / 'prof'}\n")
        run("start: the game opens on the least entry (eDeviceScreen), the mod takes the profile entry's scale at once",
            ps_exe, t, "--startup", "1920x1080")
    # С5: в списке Windows есть режим больше монитора (DSR / VSR) — монитором остаётся рабочий стол
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        ini = "[ui4k]@game_hdr=1@game_aa=0@enabled=1@log=2@test_dsr=7680x4320@all_monitors=0@"
        (Path(t) / "ui4k.ini").write_text(ini.replace("@", chr(10)))
        run("native = desktop mode, not the largest in the list (C5, DSR)", ps_exe, t, "--dsr")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # второй монитор другого размера (17.11)
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nlog=2\ntest_monitor=3840x2160\n"
                                          "test_monitor2=3440x1440\n")
        run("4K + 21:9, game on 4K: only 16:9 entries and one entry to the 21:9 monitor, which moves the game there", ps_exe, t,
            "--two")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # в профиле игры строка монитора 21:9
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "prof" / "1").mkdir(parents=True)
        (Path(t) / "prof" / "1" / "options.set").write_text(
            '{options\n\t{video\n\t\t{resolution\n\t\t\t{custom_mode "1280x720"}\n\t\t\t{mode "2580x1080"}\n\t\t}\n'
            '\t\t{hdr 1}\n\t\t{antialiasing none}\n\t}\n}\n')
        (Path(t) / "ui4k.ini").write_text("[ui4k]\nenabled=1\nlog=2\ntest_monitor=3840x2160\ntest_monitor2=3440x1440\n"
                                          f"test_profiles={Path(t) / 'prof'}\n")
        run("4K + 21:9, the game profile has a 21:9 entry: only 21:9 entries, the game opens on the 21:9 monitor", ps_exe, t,
            "--two21")
    # F13: таблица строк для других мониторов (монитор подменён ключом test_monitor)
    for mon in ("3840x2160", "2560x1440", "1920x1080", "3440x1440", "2560x1080", "1920x1200"):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nlog=2\ntest_monitor=" + mon + "\n")
            run(f"presets table for monitor {mon} (F13)", ps_exe, t, "--table")

    # обёртки контекстов на WARP-устройстве через собранную DLL
    ctx_exe = compile_("test_ctx", TESTS / "test_ctx.c", libs=("-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=3/2\nlog=2\nfit_native=0\n")
        run("ctx (обёртки, deferred, идентичность, 2 устройства)", ctx_exe, t)
        log = (Path(t) / "ui4k.log").read_text(errors="replace")
        ok = "scale=3/2" in log and "wrapped as" in log
        print(f"{'PASS' if ok else 'FAIL'}  log (scale=3/2 и обёртки в ui4k.log)")
        fails += not ok
        # enabled=0: чистый проброс, ничего не оборачивается
        (Path(t) / "ui4k.ini").write_text("[ui4k]\nenabled=0\nscale=2\nlog=1\n")
        run("passthrough (enabled=0)", ctx_exe, t, "--passthrough")

    # разведка F14: снимок кадра в лог
    dg_exe = compile_("test_diag", TESTS / "test_diag.c", libs=("-luser32", "-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\ndiag_at=1\n")
        run("diag frame capture (F14)", dg_exe, t)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # 20.11: «запись прошлого»
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\ndiag_ring=1\ndiag_at=120\n")
        run("diag ring: the last frames kept in memory, written on Ctrl+F11 (flash hunt 20.11)", dg_exe, t, "ring")

    # F14/L8: мир без HDR — своя цель мира, переход на первом рисовании интерфейса
    # устойчивость к обновлениям игры: «игра после обновления» через собранную DLL
    up_exe = compile_("test_update", TESTS / "test_update.c", SRC / "gamedata.c",
                      libs=("-ldxgi", "-luser32", "-ldxguid", "-luuid", "-lbcrypt"))
    fake_lib = OUT / "tests" / "GFSDK_SSAO_fake.win64.dll"
    subprocess.run([*CC, "-O0", "-shared", str(TESTS / "gfsdk_fake.c"), "-o", str(fake_lib), "-ld3d11", "-ldxguid", "-luuid"],
                   check=True)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        shutil.copy2(fake_lib, Path(t) / fake_lib.name)
        run("updated game: interface VS by name, scene buffer of a new size, HBAO+ buffer and radius moved, exposure rebuilt "
            "from its source", up_exe, t)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # 23.11: в архиве только исходники, кэш собрала сама игра
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        shutil.copy2(fake_lib, Path(t) / fake_lib.name)
        run("modded game: archive with sources only, shaders from the folder the game compiles into (interface VS by name, "
            "exposure rebuilt)", up_exe, t, "ucache")
    try:   # то же на архиве настоящей игры, если она здесь есть
        real_pak = find_game() / "resource" / "shader.pak"
    except (SystemExit, Exception):
        real_pak = None
    if real_pak and real_pak.exists():
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            run("real game archive: exposure shaders rebuilt from the game's own sources, interface VS by name", up_exe, t, "real",
                real_pak)

    wo_exe = compile_("test_world", TESTS / "test_world.c", libs=("-ldxgi", "-luser32", "-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("world without HDR: own world target, interface on top (F14/L8)", wo_exe, t)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("world, HDR off in game settings: copies at world size (F14/L8)", wo_exe, t, "hdr0")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # экран читается сведением выборок, как в игре
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("world without HDR, screen read with ResolveSubresource like the game does", wo_exe, t, "resolve")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # С4: без строк списка (presets=0, render=50)
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("world without HDR, presets=0 + render=50: own world target in every frame (C4)", wo_exe, t, "nopresets")
    # П7: MSAA + HDR выключен + строка «мир меньше экрана» — своя цель мира с выборками экрана
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("MSAA with a smaller world: own world target multisampled like the screen (P7)", wo_exe, t, "msaa")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # Г1: MSAA включён в бою
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("MSAA switched on in a battle: world size stays, own target gets the samples (G1)", wo_exe, t, "msaa_live")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("msaa_safe=1: safe mode, world at interface size (B0)", wo_exe, t, "msaa_safe")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # Э1: режим замеров
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        run("perf=1: frame times, GPU time of frame, interface and own world copy, census of x1 caches in xk targets (E1)",
            wo_exe, t, "perf")
    for mode, name in (("lines", "interface lines (minimap frame) drawn as thick as a logical pixel: 3 draws at x1.5 (E5)"),
                       ("lines_off", "thick_lines=0: interface line drawn once (E5)"),
                       ("lines_nopre", "presets=0, world = screen: interface lines still thick (review 19.11, 6)")):
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            run(name, wo_exe, t, mode)

    # L8: свой пакет подписей в localizations
    loc_exe = compile_("test_loc", TESTS / "test_loc.c")
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\n")
        run("labels pak shown in localizations (L8)", loc_exe, t)

    # F14: сцена и выход с разными коэффициентами
    rn_exe = compile_("test_render", TESTS / "test_render.c", libs=("-luser32", "-ldxguid", "-luuid"))
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\nrender=200\n")
        run("render 200% x scale 1.5: scene x3, output x1.5 (F14)", rn_exe, t)
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:   # render=WxH: доля родной ширины монитора
        shutil.copy2(dll, Path(t) / "d3d11.dll")
        (Path(t) / "ui4k.ini").write_text("[ui4k]\ngame_hdr=1\ngame_aa=0\nenabled=1\nscale=1.5\nlog=1\nfit_native=0\nrender=1280x720\n")
        run("ctx with render=1280x720 (F14)", ctx_exe, t)
        log = (Path(t) / "ui4k.log").read_text(errors="replace")
        m = re.search(r"render width 1280 \(monitor width (\d+)\): scene textures (\d+)/(\d+)", log)
        ok = bool(m) and (int(m[1]) == 0 or abs(int(m[2]) / int(m[3]) - 1.5 * 1280 / int(m[1])) < 1e-9)
        print(f"{'PASS' if ok else 'FAIL'}  render=1280x720 -> scene ratio = 1.5 * 1280 / monitor ({m[0] if m else 'no line'})")
        fails += not ok
    # next_d3d11=: следующая d3d11.dll в цепочке (DXVK на Windows) — полный путь, сама себя, нет файла
    sys_d3d = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "d3d11.dll"
    # (значение next_d3d11, положить рядом d3d11_dxvk.dll — копию системной, что должно быть в логе, чего не должно)
    for val, dxvk, want, nope in ((str(sys_d3d), False, "is used instead of the system d3d11.dll", None),
                                  ("d3d11.dll", False, "is the mod itself - ignored", None),
                                  ("no_such_d3d11.dll", False, "cannot load", None),
                                  ("", True, "d3d11_dxvk.dll is used instead", None),        # дополнение DXVK: само
                                  ("system", True, "loaded into", "is used instead"),        # system — даже при нём
                                  ("", False, "loaded into", "next_d3d11"),                  # без него — как было
                                  ("system", "dxgi", "is DXVK's dxgi.dll", None),            # dxgi.dll DXVK без пары
                                  ("", "reshade", "loaded into", "WARNING")):                 # чужой dxgi.dll — молча
        with tempfile.TemporaryDirectory(prefix="ui4k_v2_") as t:
            shutil.copy2(dll, Path(t) / "d3d11.dll")
            if dxvk is True:
                shutil.copy2(sys_d3d, Path(t) / "d3d11_dxvk.dll")
            elif dxvk:   # только сличение содержимого, сам файл не грузится: заглушка с меткой DXVK или без
                (Path(t) / "dxgi.dll").write_bytes(b"MZ" + b"\0" * 70000 + (b"DXVK_LOG_LEVEL" if dxvk == "dxgi" else b"ReShade"))
            (Path(t) / "ui4k.ini").write_text(f"[ui4k]\ngame_hdr=0\ngame_aa=0\nenabled=1\nscale=2\nlog=1\nfit_native=0\nnext_d3d11={val}\n")
            name = f"next_d3d11={Path(val).name or '(empty)'}{' + d3d11_dxvk.dll' if dxvk is True else f' + {dxvk} dxgi.dll' if dxvk else ''}"
            run(f"{name}: device works", ctx_exe, t)
            log = (Path(t) / "ui4k.log").read_text(errors="replace")
            ok = want in log and "FATAL" not in log and not (nope and nope in log)
            print(f"{'PASS' if ok else 'FAIL'}  {name} -> log says \"{want}\"{f', not \"{nope}\"' if nope else ''}")
            fails += not ok

    fails += test_install()
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_pkg_") as t:   # архив для игроков: только файлы, без программ
        try:
            problems = check_package(package(dll, t), dll, version())
        except SystemExit as e:   # package() сам отказывается собирать неправильный архив
            problems = [str(e)]
        print(f"{'PASS' if not problems else 'FAIL'}  player package: binaries/x64/d3d11.dll + ui4k.ini + README, no programs")
        for p in problems:
            print("      " + p)
        fails += bool(problems)
    print("ВСЕ ТЕСТЫ ПРОЙДЕНЫ" if not fails else f"ОШИБОК: {fails}")
    if debug:
        if dbg["missing"]:
            print("проверка Direct3D: слоя отладки нет в системе (компонент Windows «Графические средства») - тесты шли без неё")
        else:
            print(f"проверка Direct3D: {dbg['lines']} сообщений в {dbg['tests']} тестах")
    return fails


def test_install():
    """установка/удаление на поддельной папке игры: бэкап чужих файлов и точное восстановление"""
    with tempfile.TemporaryDirectory(prefix="ui4k_v2_game_") as t:
        x64 = Path(t) / "binaries" / "x64"
        x64.mkdir(parents=True)
        (x64 / "call_to_arms.exe").write_bytes(b"MZ fake")
        (x64 / "d3d11.dll").write_bytes(b"previous proxy")
        (x64 / "ui4k.ini").write_bytes(b"[ui4k]\nold=1\n")
        (x64 / "winmm.dll").write_bytes(b"foreign")
        before = {p.name: p.read_bytes() for p in x64.iterdir()}
        dll = OUT / "d3d11.dll"
        install(dll, Path(t))
        mid_ok = (x64 / "d3d11.dll").read_bytes() == dll.read_bytes() and (x64 / BACKUP_DIR).is_dir()
        install(dll, Path(t))  # повторная установка не должна затереть исходный бэкап
        (x64 / "ui4k.log").write_text("log")
        (x64 / "ui4k.2.log").write_text("log of a second copy of the game")
        udir = Path(t) / "user" / "ui4k"   # пакет подписей в папке пользователя (папка игры была только для чтения)
        udir.mkdir(parents=True)
        (udir / "ui4k_labels.pak").write_bytes(b"PK")
        # профили: один с режимом из строк мода, другой с настоящим режимом монитора
        native, modes = display_modes()
        prof = Path(t) / "profiles"
        (prof / "a").mkdir(parents=True)
        (prof / "b").mkdir(parents=True)
        (prof / "a" / "options.set").write_text('{video {mode "1857x1045"} {hdr 0}}')
        real = f'{{video {{mode "{native[0]}x{native[1]}"}} {{hdr 0}}}}'
        (prof / "b" / "options.set").write_text(real)
        uninstall(Path(t), prof, udir)
        after = {p.name: p.read_bytes() for p in x64.iterdir()}
        if udir.exists():
            print("      user folder with the labels pak is left after uninstall")
            mid_ok = False
        prof_ok = ((prof / "a" / "options.set").read_text() == f'{{video {{mode "{native[0]}x{native[1]}"}} {{hdr 0}}}}' and
                   (prof / "a" / "options.set.ui4k-uninstall.bak").exists() and
                   (prof / "b" / "options.set").read_text() == real and
                   not (prof / "b" / "options.set.ui4k-uninstall.bak").exists())
        if not prof_ok:
            print(f"      profiles: a={(prof / 'a' / 'options.set').read_text()!r} native={native}")
        ok = mid_ok and before == after and prof_ok
        print(f"{'PASS' if ok else 'FAIL'}  install/uninstall (бэкап, точный откат, режим профиля)")
        if not ok:
            print(f"      before={sorted(before)} after={sorted(after)} mid_ok={mid_ok}")
        return 0 if ok else 1


# ---------------- установка ----------------
def game_running(game_root):
    """запущен ли call_to_arms.exe именно из этой папки (по полному пути процесса)"""
    target = str((game_root / "binaries" / "x64" / "call_to_arms.exe").resolve()).lower()
    psapi, k32 = ctypes.WinDLL("psapi"), ctypes.WinDLL("kernel32")
    pids = (wintypes.DWORD * 4096)()
    got = wintypes.DWORD()
    psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
    for pid in pids[:got.value // 4]:
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            continue
        buf = ctypes.create_unicode_buffer(1024)
        n = wintypes.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) and buf.value.lower() == target:
            k32.CloseHandle(h)
            return True
        k32.CloseHandle(h)
    return False


def install(dll, game_root=None):
    game_root = game_root or find_game()
    if game_running(game_root):
        sys.exit("call_to_arms.exe запущен — закройте игру перед установкой")
    x64 = game_root / "binaries" / "x64"
    bdir = x64 / BACKUP_DIR
    manifest = bdir / "manifest.json"
    if not manifest.exists():  # первый раз: сохранить то, что было до мода
        bdir.mkdir(exist_ok=True)
        saved = []
        for f in INSTALLED:
            if (x64 / f).exists():
                shutil.copy2(x64 / f, bdir / f)
                saved.append(f)
        manifest.write_text(json.dumps({"saved": saved, "installed": INSTALLED}, ensure_ascii=False, indent=1))
        print(f"бэкап: {saved or 'нечего сохранять'} -> {bdir}")
    shutil.copy2(dll, x64 / "d3d11.dll")
    shutil.copy2(ROOT / "ui4k.ini", x64 / "ui4k.ini")
    for f in ("ui4k_fonts2.dat", "ui4k_fonts4.dat"):   # Э6: без них — страницы шрифтов игры
        if (FONT_DATA / f).exists():
            shutil.copy2(FONT_DATA / f, x64 / f)
        else:
            print(f"нет {FONT_DATA / f} - чёткие шрифты не установлены (положите файлы из архива выпуска)")
    print(f"установлено в {x64}")


class DEVMODEW(ctypes.Structure):
    _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD), ("dmDriverVersion", wintypes.WORD),
                ("dmSize", wintypes.WORD), ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                ("dmPosition", wintypes.LONG * 2), ("dmDisplayOrientation", wintypes.DWORD),
                ("dmDisplayFixedOutput", wintypes.DWORD), ("dmColor", wintypes.SHORT), ("dmDuplex", wintypes.SHORT),
                ("dmYResolution", wintypes.SHORT), ("dmTTOption", wintypes.SHORT), ("dmCollate", wintypes.SHORT),
                ("dmFormName", wintypes.WCHAR * 32), ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD),
                ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD), ("dmDisplayFlags", wintypes.DWORD),
                ("dmDisplayFrequency", wintypes.DWORD)] + [("_rest", ctypes.c_byte * 64)]


def display_modes():
    """(родной режим, множество настоящих режимов) основного монитора — без мода, прямо у Windows"""
    u32 = ctypes.WinDLL("user32")
    d = DEVMODEW()
    d.dmSize = ctypes.sizeof(DEVMODEW) - 64
    modes, i = set(), 0
    while u32.EnumDisplaySettingsW(None, i, ctypes.byref(d)):
        modes.add((d.dmPelsWidth, d.dmPelsHeight))
        i += 1
    u32.EnumDisplaySettingsW(None, -2, ctypes.byref(d))   # ENUM_REGISTRY_SETTINGS — рабочий стол
    return (d.dmPelsWidth, d.dmPelsHeight), modes


def profiles_dir():
    docs = ctypes.create_unicode_buffer(260)
    ctypes.WinDLL("shell32").SHGetFolderPathW(None, 5, None, 0, docs)   # CSIDL_PERSONAL
    return Path(docs.value) / "my games" / "gates of hell" / "profiles"


def fix_profiles(pdir, native, modes):
    """режим из строк мода (1856x1044 и т.п.), которого нет у монитора, -> родной; бэкап options.set.ui4k-uninstall.bak"""
    fixed = []
    for f in sorted(Path(pdir).glob("*/options.set")) if Path(pdir).is_dir() else []:
        txt = f.read_bytes().decode("utf-8", "surrogateescape")
        m = re.search(r'\{mode "(\d+)x(\d+)"\}', txt)
        if not m or (int(m[1]), int(m[2])) in modes:
            continue
        shutil.copy2(f, f.with_name("options.set.ui4k-uninstall.bak"))
        txt = txt[:m.start()] + f'{{mode "{native[0]}x{native[1]}"}}' + txt[m.end():]
        f.write_bytes(txt.encode("utf-8", "surrogateescape"))
        fixed.append(f"{f.parent.name}: {m[1]}x{m[2]} -> {native[0]}x{native[1]}")
    return fixed


def uninstall(game_root=None, pdir=None, udir=None):
    game_root = game_root or find_game()
    if game_running(game_root):
        sys.exit("call_to_arms.exe запущен — закройте игру перед удалением")
    x64 = game_root / "binaries" / "x64"
    bdir = x64 / BACKUP_DIR
    manifest = bdir / "manifest.json"
    if not manifest.exists():
        sys.exit(f"нет {manifest}: мод v2 не ставился этим скриптом, удалять нечего")
    m = json.loads(manifest.read_text())
    for f in sorted(set(m["installed"]) | set(INSTALLED)) + LEFTOVERS:   # и файлы, добавленные позже (образцы букв Э6)
        if (x64 / f).exists():
            (x64 / f).unlink()
    for f in m["saved"]:
        shutil.copy2(bdir / f, x64 / f)
    shutil.rmtree(bdir)
    u = udir or user_dir()   # пакет подписей, если папка игры была только для чтения
    if (u / "ui4k_labels.pak").exists():
        (u / "ui4k_labels.pak").unlink()
    if u.exists() and not any(u.iterdir()):
        u.rmdir()
    print(f"удалено; восстановлено из бэкапа: {m['saved'] or 'ничего'}")
    # без мода строк «мир × масштаб» нет: режим, которого у монитора нет, вернуть на родной
    native, modes = display_modes()
    for line in fix_profiles(pdir or profiles_dir(), native, modes):
        print(f"профиль {line} (бэкап options.set.ui4k-uninstall.bak)")


# ---------------- архив для игроков ----------------
PACKAGE_FILES = {"binaries/x64/d3d11.dll", "binaries/x64/ui4k.ini", "binaries/x64/ui4k_fonts2.dat",
                 "binaries/x64/ui4k_fonts4.dat", "README_RU.txt", "README_EN.txt"}


def player_text(path, ver):
    """текст для Блокнота: UTF-8 с BOM, концы строк Windows, {version} -> версия"""
    t = path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("{version}", ver)
    return "﻿".encode() + t.replace("\n", "\r\n").encode("utf-8")


def package(dll, out_dir=None):
    """архив для игроков: только файлы, без программ. Установка — папку binaries в папку игры (d3d11.dll и ui4k.ini
    лягут в binaries\\x64), удаление — стереть их. Инструкция — package/README_*.txt"""
    import zipfile
    ver = version()
    out_dir = Path(out_dir or OUT / "package")
    out_dir.mkdir(parents=True, exist_ok=True)
    zp = out_dir / f"GoH-UI4K-{ver}.zip"
    ini = (ROOT / "ui4k.ini").read_text(encoding="utf-8").replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, "binaries/x64/d3d11.dll")
        z.writestr("binaries/x64/ui4k.ini", ini)
        for f in ("ui4k_fonts2.dat", "ui4k_fonts4.dat"):   # Э6: образцы букв
            if not (FONT_DATA / f).exists():
                sys.exit(f"нет {FONT_DATA / f} - положите файлы букв из архива выпуска в {FONT_DATA}")
            z.write(FONT_DATA / f, "binaries/x64/" + f)
        for name in ("README_RU.txt", "README_EN.txt"):
            z.writestr(name, player_text(ROOT / "package" / name, ver))
    problems = check_package(zp, dll, ver)
    if problems:
        sys.exit("архив собран неправильно: " + "; ".join(problems))
    print(f"архив для игроков: {zp} ({zp.stat().st_size // 1024} КБ, версия {ver})")
    return zp


# Дополнение DXVK: официальные 64-битные файлы DXVK без изменений, лежат вне репозитория (чужие программы в git не кладём).
# Принимаются только файлы ровно с этими SHA-256 — те, что проверены на VirusTotal (d3d11 0/70, dxgi 0/71, 21.11.2026).
DXVK_DIR = ROOT.parent / "release" / "dxvk-3.1.1"
DXVK_FILES = {"d3d11.dll": ("binaries/x64/d3d11_dxvk.dll", "0a203b6255c893430f0a8461c618962df04e62aa41538d5a875b21868317cf9f"),
              "dxgi.dll": ("binaries/x64/dxgi.dll", "e3c1178eb7f0dd59f91bcfcc257549e5b9cfdb5e8508719293de7f8704f53160")}


def package_dxvk(out_dir=None):
    """архив дополнения DXVK: два файла DXVK (d3d11 переименован в d3d11_dxvk.dll — мод находит его сам),
    лицензия DXVK и инструкция. Без программ."""
    import hashlib
    import zipfile
    ver = version()
    out_dir = Path(out_dir or OUT / "package")
    out_dir.mkdir(parents=True, exist_ok=True)
    zp = out_dir / f"GoH-UI4K-{ver}-DXVK-addon.zip"
    for src, (_, sha) in DXVK_FILES.items():
        p = DXVK_DIR / "x64" / src
        if not p.exists() or hashlib.sha256(p.read_bytes()).hexdigest() != sha:
            sys.exit(f"{p}: нет файла или не тот (SHA-256 не совпадает с проверенным на VirusTotal)")
    lic = DXVK_DIR / "LICENSE.txt"
    if not lic.exists():
        sys.exit(f"нет {lic} (лицензия DXVK)")
    with zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED) as z:
        for src, (dst, _) in DXVK_FILES.items():
            z.write(DXVK_DIR / "x64" / src, dst)
        z.writestr("DXVK_LICENSE.txt", player_text(lic, ver))
        z.writestr("README_DXVK.txt", player_text(ROOT / "package" / "README_DXVK.txt", ver))
    with zipfile.ZipFile(zp) as z:
        names = sorted(n for n in z.namelist() if not n.endswith("/"))
        want = sorted([d for d, _ in DXVK_FILES.values()] + ["DXVK_LICENSE.txt", "README_DXVK.txt"])
        if names != want or any(hashlib.sha256(z.read(d)).hexdigest() != s for d, s in DXVK_FILES.values()):
            sys.exit(f"дополнение DXVK собрано неправильно: {names}")
    print(f"дополнение DXVK: {zp} ({zp.stat().st_size // 1024} КБ, версия {ver})")
    return zp


def check_package(zp, dll, ver):
    """что в архиве: ровно PACKAGE_FILES, ни одной программы, та же DLL, ini и инструкции с версией"""
    import zipfile
    bad = []
    with zipfile.ZipFile(zp) as z:
        names = {n for n in z.namelist() if not n.endswith("/")}
        if names != PACKAGE_FILES:
            bad.append(f"файлы {sorted(names)} вместо {sorted(PACKAGE_FILES)}")
        if any(n.lower().endswith((".exe", ".bat", ".cmd", ".ps1", ".vbs", ".msi")) for n in names):
            bad.append("в архиве программа")
        if "binaries/x64/d3d11.dll" in names and z.read("binaries/x64/d3d11.dll") != dll.read_bytes():
            bad.append("d3d11.dll не та, что собрана")
        if "binaries/x64/ui4k.ini" in names:
            ini = z.read("binaries/x64/ui4k.ini").decode("utf-8")
            if "[ui4k]" not in ini or "enabled=1" not in ini or "\n" in ini.replace("\r\n", ""):
                bad.append("ui4k.ini без [ui4k] / enabled=1 или с концами строк не для Windows")
        for n in ("README_RU.txt", "README_EN.txt"):
            if n in names:
                t = z.read(n)
                if not t.startswith(b"\xef\xbb\xbf") or ver.encode() not in t or b"{version}" in t:
                    bad.append(f"{n}: нет BOM или версии")
    if ver.encode() not in dll.read_bytes():
        bad.append(f"в d3d11.dll нет версии {ver}")
    return bad


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--test", action="store_true")
    g.add_argument("--install", action="store_true")
    g.add_argument("--uninstall", action="store_true")
    g.add_argument("--package", action="store_true", help="архив для игроков в out/package (только файлы, без программ)")
    ap.add_argument("--debug-layer", action="store_true", help="с --test: слой отладки Direct3D, его сообщения по тестам")
    ap.add_argument("--pdb", action="store_true", help="с файлом символов (имена функций для профилировщика); не для --package")
    a = ap.parse_args()
    if a.pdb and a.package:
        sys.exit("--pdb не для архива игроков: в DLL был бы путь к .pdb на этом компьютере")
    if a.uninstall:
        uninstall()
    else:
        d = build(a.pdb)
        if a.test:
            sys.exit(1 if run_tests(d, a.debug_layer) else 0)
        if a.install:
            install(d)
        if a.package:
            package(d)
            package_dxvk()
