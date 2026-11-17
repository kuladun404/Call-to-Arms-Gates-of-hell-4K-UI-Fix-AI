# Call to Arms Gates of hell - 4K UI Fix (AI)

**[Русский](#русский) · [English](#english)**

Мод интерфейса для **Call to Arms – Gates of Hell: Ostfront** на мониторах 2K, 4K и 21:9: крупный интерфейс (×1.25…×2) и 3D-мир в разрешении, которое вы выберете.
An interface mod for **Call to Arms – Gates of Hell: Ostfront** on 2K, 4K and 21:9 monitors: a large interface (×1.25…×2) and the 3D world at the resolution you choose.

> **2.0 — бета / beta.** Новая версия, написана заново. Стабильная 1.0 (только 4K, ×2, с перерисованными шрифтами) — [v1.0](../../releases/tag/v1.0).
> A new version, rewritten from scratch. The stable 1.0 (4K only, ×2, with redrawn fonts) is [v1.0](../../releases/tag/v1.0).

> Мод и инструменты созданы с помощью ИИ (Claude) — отсюда «(AI)» в названии.
> The mod and its tools were built with AI assistance (Claude) — hence "(AI)" in the name.

**⬇ [2.0 beta](../../releases/tag/v2.0-beta)** · [1.0](../../releases/tag/v1.0) · **[Что изменилось / Changelog](CHANGELOG.md)**
Также на / Also on: **[Nexus Mods](https://www.nexusmods.com/calltoarmsgatesofhellostfront/mods/29)** · **[Steam guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3839336153)**

---

## Русский

### Что делает мод
- Интерфейс крупнее в 1,25–2 раза на любом мониторе: 2K, 4K, 21:9.
- 3D-мир — в родном разрешении монитора или меньше: меньший мир даёт больше кадров, интерфейс остаётся чётким.
- Выбирается прямо в игре: «Параметры» → «Видео» → «Разрешение», строки вида `2560×1440  ×2` (мир × масштаб интерфейса).
- Сглаживание MSAA, HDR, глубина резкости и затенение HBAO+ работают вместе с модом.
- Файлы игры и exe не изменяются.

### Установка
1. Скачайте архив из [выпуска 2.0 beta](../../releases/tag/v2.0-beta) и распакуйте.
2. Закройте игру. Откройте папку игры: Steam → правой кнопкой по игре → Управление → Просмотреть локальные файлы.
3. Перетащите в неё папку `binaries` из архива: `d3d11.dll` и `ui4k.ini` окажутся в `binaries\x64`, рядом с `call_to_arms.exe`.
4. В игре: «Параметры» → «Видео» → «Разрешение», выберите строку и нажмите «Применить».

Если стояла 1.0 — просто замените файлы; папку `ui4k_fonts` можно удалить, 2.0 её не использует.
**Удаление:** закройте игру и удалите из `binaries\x64` файлы `d3d11.dll`, `ui4k.ini`, `ui4k_labels.pak` и `ui4k.log`. Разрешение игра вернёт сама.
Подробно — `README_RU.txt` в архиве.

### Сборка из исходников
Нужен Python 3.11+ и `pip install ziglang` (компилятор C ставить не нужно).
```
python build.py             # DLL -> out\d3d11.dll
python build.py --test      # тесты без игры
python build.py --package   # архив для игроков -> out\package
python build.py --install   # поставить в игру (игра должна быть закрыта)
```
Папка игры ищется через Steam автоматически; иначе задайте `GOH_DIR=<папка игры>`.
Как всё устроено — [docs/ARCHITECTURE_RU.md](docs/ARCHITECTURE_RU.md).

### Сетевая игра
Мод не трогает exe и файлы, которые игра сверяет по сети, но официально это не проверялось — на ваше усмотрение.

---

## English

### What it does
- The interface is 1.25 to 2 times larger on any monitor: 2K, 4K, 21:9.
- The 3D world renders at the monitor's native resolution or lower: a smaller world gives more frames, the interface stays sharp.
- Picked right in the game: Options → Video → Resolution, entries like `2560×1440  ×2` (world × interface scale).
- MSAA anti-aliasing, HDR, depth of field and HBAO+ shading work together with the mod.
- Game files and the exe are not changed.

### Installing
1. Download the archive from the [2.0 beta release](../../releases/tag/v2.0-beta) and unpack it.
2. Close the game. Open the game folder: Steam → right-click the game → Manage → Browse local files.
3. Drag the `binaries` folder from the archive into it: `d3d11.dll` and `ui4k.ini` end up in `binaries\x64`, next to `call_to_arms.exe`.
4. In the game: Options → Video → Resolution, pick an entry and press Apply.

Coming from 1.0: just replace the files; the `ui4k_fonts` folder can be deleted, 2.0 does not use it.
**Removing:** close the game and delete `d3d11.dll`, `ui4k.ini`, `ui4k_labels.pak` and `ui4k.log` from `binaries\x64`. The game sets its resolution back by itself.
Details: `README_EN.txt` in the archive.

### Building from source
Python 3.11+ and `pip install ziglang` (no C compiler needed).
```
python build.py             # DLL -> out\d3d11.dll
python build.py --test      # tests without the game
python build.py --package   # player archive -> out\package
python build.py --install   # install into the game (the game must be closed)
```
The game folder is found through Steam; otherwise set `GOH_DIR=<game folder>`.
How it works: [docs/ARCHITECTURE_RU.md](docs/ARCHITECTURE_RU.md) (in Russian).

### Multiplayer
The mod does not touch the exe or the files the game checks online, but this has not been officially verified — use at your discretion.

---

Unofficial fan mod. The game and its assets belong to Barbedwire Studios / Digitalmindsoft. License: MIT.
