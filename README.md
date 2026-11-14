# Call to Arms Gates of hell - 4K UI Fix (AI)

**[Русский](#русский) · [English](#english)**

Мод интерфейса для **Call to Arms – Gates of Hell: Ostfront** на мониторах 4K: интерфейс ровно ×2, как на 1080p, при честном рендере 3D в 3840×2160, с чёткими векторными шрифтами.
A 4K interface fix for **Call to Arms – Gates of Hell: Ostfront**: the UI is exactly ×2 like on 1080p, the 3D is rendered at true 3840×2160, and the fonts are sharp vector redraws.

> Мод и инструменты созданы с помощью ИИ (Claude) — отсюда «(AI)» в названии.
> The mod and its tools were built with AI assistance (Claude) — hence "(AI)" in the name.

**⬇ [Скачать готовый мод / Download the mod](../../releases/latest)** — файл `GoH-UI4K-x2-v1.0.zip`.

---

## Русский

### Что делает мод
- Игра думает, что работает в 1920×1080, а прокси `d3d11.dll` рисует всё в 3840×2160.
- Весь интерфейс (меню, лобби, бой, настройки, «Завоевание») ровно в 2 раза крупнее, без наложений и обрезанного текста.
- 3D-сцена, вода, HBAO+ и постэффекты рисуются в настоящем 4K.
- Все растровые шрифты игры перерисованы векторно в 4K строго в оригинальных размерах — без размытия от растягивания.
- Заставка при запуске, курсор мыши и оверлей Steam — правильного размера.
- Файлы игры и exe не изменяются.

### Установка
1. [Скачайте](../../releases/latest) `GoH-UI4K-x2-v1.0.zip` и распакуйте.
2. Закройте игру. Скопируйте `d3d11.dll`, `ui4k.ini` и папку `ui4k_fonts` в `<папка игры>\binaries\x64\` (рядом с `call_to_arms.exe`).
3. В игре выставьте разрешение **1920×1080**, полноэкранный режим.

Подробно — [install/УСТАНОВКА.txt](install/УСТАНОВКА.txt).
**Удаление:** удалите `d3d11.dll`, `ui4k.ini`, `ui4k.log` и `ui4k_fonts` из `binaries\x64` и верните в игре 3840×2160.

### Сборка из исходников
Нужны Python 3.11+ и `pip install ziglang numpy pillow scipy`.
```
python build.py                       # DLL -> out\d3d11.dll
python tools\make_fonts.py --clean --big   # шрифты -> out\ui4k_fonts (~3 мин на всех ядрах)
python tools\check_fonts.py           # сверка каждой буквы с оригиналом
python build.py --install             # поставить DLL и шрифты в игру (игра должна быть закрыта)
```
Папка игры ищется через Steam автоматически; иначе задайте `GOH_DIR=<папка игры>`.
Как всё устроено — [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (англ.).

### Сетевая игра
Мод не трогает exe и файлы, которые игра сверяет по сети, но официально это не проверялось — на ваше усмотрение.

---

## English

### What it does
- The game thinks it runs at 1920×1080; the `d3d11.dll` proxy renders everything at 3840×2160.
- The whole interface (menus, lobby, battle, settings, Conquest) is exactly 2× larger, with no overlaps or clipped text.
- The 3D scene, water, HBAO+ and post-effects render at true 4K.
- All of the game's bitmap fonts are redrawn as vector outlines at 4K, at exactly the original size — no upscaling blur.
- Startup splash, mouse cursor and Steam overlay have the right size.
- No game files or the exe are modified.

### Install
1. [Download](../../releases/latest) `GoH-UI4K-x2-v1.0.zip` and unpack it.
2. Close the game. Copy `d3d11.dll`, `ui4k.ini` and the `ui4k_fonts` folder into `<game folder>\binaries\x64\` (next to `call_to_arms.exe`).
3. In the game, set the resolution to **1920×1080**, fullscreen.

Details: [install/INSTALL_EN.txt](install/INSTALL_EN.txt).
**Uninstall:** delete `d3d11.dll`, `ui4k.ini`, `ui4k.log` and `ui4k_fonts` from `binaries\x64` and set the game back to 3840×2160.

### Build from source
Python 3.11+ and `pip install ziglang numpy pillow scipy`.
```
python build.py                       # DLL -> out\d3d11.dll
python tools\make_fonts.py --clean --big   # fonts -> out\ui4k_fonts (~3 min on all cores)
python tools\check_fonts.py           # compares every glyph with the original
python build.py --install             # install DLL and fonts into the game (game must be closed)
```
The game folder is found through Steam; otherwise set `GOH_DIR=<game folder>`.
How it works: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Multiplayer
The mod leaves the exe and the files the game checks online untouched, but this has not been officially verified — use at your own discretion.

---

## Licenses / Лицензии
- Code (`src/`, `build.py`, `tools/*.py`): [MIT](LICENSE).
- Replacement fonts in `tools/fonts/`: their own licenses (SIL OFL 1.1, Apache 2.0, GUST) — see [tools/fonts/README.md](tools/fonts/README.md).
- The prebuilt `ui4k_fonts` in the release are redrawn from the game's own font atlases (and Arial outlines for the Arial-based fonts). Call to Arms – Gates of Hell and its assets belong to Barbedwire Studios / Digitalmindsoft; Arial belongs to Monotype/Microsoft. This is an unofficial fan mod, not affiliated with them.
- Готовые `ui4k_fonts` в релизе перерисованы из шрифтовых атласов игры (и контуров Arial). Игра и её ресурсы принадлежат Barbedwire Studios / Digitalmindsoft, Arial — Monotype/Microsoft. Это неофициальный фанатский мод.
