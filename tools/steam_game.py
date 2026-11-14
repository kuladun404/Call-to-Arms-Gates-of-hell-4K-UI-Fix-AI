"""Поиск папки Call to Arms – Gates of Hell / locate the game folder.

Порядок / order:
  1. переменная окружения GOH_DIR (корень игры, где лежат binaries\\ и resource\\);
  2. Steam: HKCU\\Software\\Valve\\Steam\\SteamPath -> steamapps\\libraryfolders.vdf -> все библиотеки ->
     steamapps\\common\\Call to Arms - Gates of Hell.
"""
import os
import re
from pathlib import Path

GAME_DIR_NAME = "Call to Arms - Gates of Hell"
EXE = Path("binaries") / "x64" / "call_to_arms.exe"


def _steam_root():
    try:
        import winreg
        for hive, key, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, key) as k:
                    return Path(winreg.QueryValueEx(k, name)[0])
            except OSError:
                continue
    except ImportError:
        pass
    return None


def _libraries(root):
    libs = [root]
    vdf = root / "steamapps" / "libraryfolders.vdf"
    if vdf.exists():
        for m in re.finditer(r'"path"\s+"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace")):
            libs.append(Path(m.group(1).replace("\\\\", "\\")))
    return libs


def find_game() -> Path:
    env = os.environ.get("GOH_DIR")
    if env:
        p = Path(env)
        if (p / EXE).exists():
            return p
        raise SystemExit(f"GOH_DIR={env}: нет {EXE} / not found")
    root = _steam_root()
    if root:
        for lib in _libraries(root):
            p = lib / "steamapps" / "common" / GAME_DIR_NAME
            if (p / EXE).exists():
                return p
    raise SystemExit("Игра не найдена: задайте GOH_DIR=<папка игры> / Game not found: set GOH_DIR=<game folder>")
