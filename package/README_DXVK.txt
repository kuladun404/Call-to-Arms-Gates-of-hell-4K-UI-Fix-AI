DXVK add-on for Call to Arms Gates of hell - 4K UI Fix (AI) — version {version}
https://github.com/kuladun404/Call-to-Arms-Gates-of-hell-4K-UI-Fix-AI

=== РУССКИЙ ===

ПРОБНО. На свой страх и риск: это неофициальные моды, разработчики игры их не делали и не поддерживают.

Что это: DXVK 3.1.1 (официальный выпуск, https://github.com/doitsujin/dxvk/releases/tag/v3.1.1),
64-битные файлы без изменений:
  binaries\x64\d3d11_dxvk.dll  — d3d11.dll из DXVK, переименован, чтобы не мешать моду
  binaries\x64\dxgi.dll        — dxgi.dll из DXVK
DXVK переводит графику игры с Direct3D на Vulkan. На Windows это иногда даёт больше FPS
(особенно на видеокартах AMD и Intel). Нужен сам мод 4K UI Fix версии 2.1-alpha или новее.

ВНИМАНИЕ: если в binaries\x64 уже есть свой dxgi.dll (ReShade, Special K и т. п.), этот архив его заменит.
Сначала сохраните свой dxgi.dll в другом месте или не ставьте дополнение.
Уже стоит свой DXVK? Это дополнение не нужно: переименуйте d3d11.dll вашего DXVK в d3d11_dxvk.dll,
а его dxgi.dll оставьте как есть.

Установка: закройте игру и перетащите папку binaries из этого архива в папку игры (как мод).
Мод сам увидит d3d11_dxvk.dll и включит DXVK. В ui4k.log появится строка
«next_d3d11: ...d3d11_dxvk.dll is used instead of the system d3d11.dll».

Удаление: удалите ОБА файла — d3d11_dxvk.dll и dxgi.dll. Один dxgi.dll без пары ломает запуск игры.
Возвращаетесь на мод 2.0 или удаляете мод — СНАЧАЛА удалите оба файла DXVK.
Журналы DXVK call_to_arms_d3d11.log и call_to_arms_dxgi.log рядом с игрой можно удалить.

Linux / Steam Deck: это дополнение НЕ нужно, в Proton DXVK уже встроен (см. README мода).

Лицензия DXVK — DXVK_LICENSE.txt (zlib/libpng). DXVK сделан его авторами, не автором этого мода.

=== ENGLISH ===

EXPERIMENTAL. Use at your own risk: these are unofficial mods, not made or supported by the game's developers.

What it is: DXVK 3.1.1 (official release, https://github.com/doitsujin/dxvk/releases/tag/v3.1.1),
unmodified 64-bit files:
  binaries\x64\d3d11_dxvk.dll  — DXVK's d3d11.dll, renamed so it does not clash with the mod
  binaries\x64\dxgi.dll        — DXVK's dxgi.dll
DXVK translates the game's graphics from Direct3D to Vulkan. On Windows it sometimes gives more FPS
(especially on AMD and Intel graphics cards). Needs the 4K UI Fix mod itself, version 2.1-alpha or newer.

WARNING: if binaries\x64 already has its own dxgi.dll (ReShade, Special K etc.), this archive replaces it.
Save your dxgi.dll somewhere else first, or don't install the add-on.
Already using your own DXVK? You don't need this add-on: rename your DXVK's d3d11.dll to d3d11_dxvk.dll
and keep its dxgi.dll as it is.

Installing: close the game and drag the binaries folder from this archive into the game folder (like the mod).
The mod finds d3d11_dxvk.dll and turns DXVK on by itself. ui4k.log then has the line
"next_d3d11: ...d3d11_dxvk.dll is used instead of the system d3d11.dll".

Removing: delete BOTH files — d3d11_dxvk.dll and dxgi.dll. dxgi.dll alone breaks the game's start.
Going back to mod 2.0 or removing the mod — FIRST delete both DXVK files.
DXVK's logs call_to_arms_d3d11.log and call_to_arms_dxgi.log next to the game can be deleted.

Linux / Steam Deck: this add-on is NOT needed, Proton already has DXVK built in (see the mod's README).

DXVK license — DXVK_LICENSE.txt (zlib/libpng). DXVK is made by its authors, not by the author of this mod.
