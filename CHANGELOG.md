# Что изменилось / Changelog

**[Русский](#русский) · [English](#english)**

---

## Русский

### 2.1-alpha2 — 23.11.2026 (пробная)

**Исправлено**
- **Игра с модами, у которых в архиве шейдеров нет готового кэша.** Некоторые моды заменяют `resource\shader.pak` архивом только с исходниками. Тогда игра собирает шейдеры сама и кладёт их в `Документы\my games\gates of hell\shader_cache\dx11.0`. Теперь мод ищет шейдеры и там. Раньше он в этом случае опирался только на данные игры от 16.11 и мог не узнать шейдеры интерфейса. В логе: «… names found (N in the archive, M compiled by the game)».
- **Инструкции яснее.** Строка запуска `WINEDLLOVERRIDES=…` — только для Linux / Steam Deck: в Windows с ней игра не запускается. Добавлено, как оставить свой DXVK: переименовать его `d3d11.dll` в `d3d11_dxvk.dll`, а его `dxgi.dll` оставить.

### 2.1-alpha — 21.11.2026 (пробная)

**Новое**
- **DXVK на Windows.** Мод может передавать работу не системному `d3d11.dll`, а DXVK (графика через Vulkan). Дополнение `…-DXVK-addon.zip` в выпуске — официальный DXVK 3.1.1 без изменений, оба файла проверены на VirusTotal. Если рядом с модом лежит `d3d11_dxvk.dll`, мод включает DXVK сам; настройка `next_d3d11=` в `ui4k.ini` задаёт другой файл, `next_d3d11=system` — всегда системный.
- **Linux / Steam Deck (Proton), пробно.** Там DXVK уже встроен; нужен только параметр запуска `WINEDLLOVERRIDES="d3d11=n,b" %command%`. Мод пишет в `ui4k.log`, что работает под Wine.
- Предупреждение «на свой страх и риск» в инструкциях архива.
- Если игра упадёт, мод записывает в `ui4k.log`, где именно (модуль и цепочку вызовов) — по этой записи можно найти причину.

**Исправлено**
- С DXVK игра вылетала при смене MSAA в настройках: мод второй раз оборачивал окно игры поверх обёртки драйвера, и вызовы шли по кругу.

**Что проверено:** DXVK на Windows, NVIDIA — меню и бой (иконки и текст такие же, как без DXVK), смена масштаба на ходу, HDR, смена MSAA в обе стороны, сворачивание окна, снимок Steam (F12). **Не проверено:** Linux и Steam Deck, видеокарты AMD и Intel с DXVK.

### 2.0 — 20.11.2026

Выпуск 2.0. Всё, что было в бете, плюс чёткие буквы, чёткие иконки предметов и исправления по разбору игры.

**Новое**
- **Чёткий текст.** Буквы интерфейса рисуются в высоком разрешении, а не растягиваются (файлы `ui4k_fonts2.dat` и `ui4k_fonts4.dat`). Буквы других языков и значки — как в игре.
- **Чёткие иконки предметов** в инвентаре (оружие, боеприпасы, снаряжение — их игра рисует из 3D-моделей). Остальные картинки интерфейса (карточки отрядов, кнопки, значки) увеличены как есть.
- **Линии интерфейса** (рамка обзора на миникарте) — толщиной в логический пиксель, а не в один пиксель экрана.
- **Смена масштаба на ходу**: после «Применить» окна, 3D-виды и иконки предметов перерисовываются в новом масштабе, без перезапуска игры.
- **Запуск сразу в масштабе профиля**: виды 3D и первые буквы больше не остаются в масштабе первой строки списка.

**Исправлено**
- Кусок мира мигал со сдвигом при начале и конце бега в прямом управлении (смена угла обзора).
- Освещение и туман портились, когда камера оказывалась над определённым местом карты.
- Затенение HBAO+ могло мигнуть, когда камера проходила над определёнными координатами.
- Отражение в воде — в размере игры, как без мода.
- Глубина резкости в меню паузы больше не роняет FPS (радиус размытия — как у игры, `dof_radius=0`).
- Счётчик FPS RivaTuner и другие оверлеи больше не сбивают мод.
- На мониторах 1080p и 1440p в списке нет строк, которые игра всё равно не показывает (меньше 1360×768).
- Снимок экрана Steam (F12) больше не показывает мир без интерфейса в углу.

**Для проверки**
- `keys=1` в `ui4k.ini` включает Ctrl+F6 / F7 / F9 / F10 — сравнить с правкой мода и без неё (по умолчанию выключено: в бою F6–F11 — бойцы отряда).

**Что ещё не проверено**
- Видеокарты AMD и Intel (проверено только на NVIDIA).
- Монитор 2560×1440 вживую (проверено тестами).
- Сглаживание MSAA 2x и 8x (4x проверено).

Если что-то не так — пришлите `ui4k.log` из `binaries\x64` и опишите, что делали, какая видеокарта и монитор.

### 2.0 beta — 17.11.2026

Мод написан заново. Это бета: на NVIDIA и мониторах 4K и 21:9 проверен в игре, остальное — см. «Что ещё не проверено».

**Новое**
- **Любой монитор**: 2K, 4K, широкоформатные 21:9. Больше не нужно ставить в игре 1920×1080.
- **Масштаб интерфейса на выбор**: ×1, ×1.25, ×1.5, ×1.75, ×2 и «интерфейс как на Full HD» (на 4K это ×2, на мониторах высотой 1440 — ×1.33).
- **3D-мир отдельно от интерфейса**: в родном разрешении монитора или меньше (1440, 1080). Меньший мир даёт больше кадров в секунду, а интерфейс остаётся чётким.
- **Всё выбирается в самой игре**: «Параметры» → «Видео» → «Разрешение», строки вида `2560×1440  ×2` (мир × масштаб интерфейса) с подписями на русском и английском. Сменить можно в любой момент кнопкой «Применить».
- **Два монитора**: в списке строки для монитора, на котором игра, и строка «другой монитор» для перехода на второй. Список подстраивается под 16:9 и 21:9.
- **Сглаживание MSAA** работает вместе с модом.
- **Яркость, глубина резкости и затенение HBAO+** выглядят так же, как у самой игры в выбранном разрешении.
- **Обновления игры**: при запуске мод сам находит нужные шейдеры в архиве игры (только читает его), поэтому патч игры чаще всего не ломает мод.
- **Установка — только копирование файлов**: папку `binaries` из архива — в папку игры. Удаление — стереть файлы, разрешение игра вернёт сама. Программ в архиве нет.
- Версия мода пишется в `ui4k.log` — по логу видно, какая версия стоит.
- Режим проверки Direct3D (`d3d_debug=1` в `ui4k.ini`) — для поиска ошибок на видеокартах AMD и Intel. Он уже нашёл и помог исправить одну ошибку.

**Чего пока нет по сравнению с 1.0**
- Перерисованных векторных шрифтов: текст увеличивается из оригинальных шрифтов игры. Папка `ui4k_fonts` от 1.0 в 2.0 не используется, её можно удалить.

**Что ещё не проверено**
- Видеокарты AMD и Intel (мод должен работать, но проверен только на NVIDIA).
- Монитор 2560×1440 с соотношением 16:9 вживую (проверено тестами).
- Масштаб Windows больше 100% (125%, 150%) без галочки совместимости «высокое разрешение» у `call_to_arms.exe`.
- Сглаживание MSAA 2x и 8x (4x проверено).

Если что-то не так — пришлите `ui4k.log` из `binaries\x64` и опишите, что делали, какая видеокарта и монитор.

### 1.0 — 14.11.2026
- Первая версия: только 4K, интерфейс ровно ×2 при игре в 1920×1080, 3D в 3840×2160.
- Чёткие векторные шрифты строго в оригинальных размерах.
- Заставка, курсор и оверлей Steam правильного размера.

---

## English

### 2.1-alpha2 — 2026-11-23 (experimental)

**Fixed**
- **Games with mods whose shader archive has no precompiled cache.** Some mods replace `resource\shader.pak` with an archive of sources only. The game then compiles the shaders itself into `Documents\my games\gates of hell\shader_cache\dx11.0`. The mod now looks there too. Before, it relied only on the game data of 16.11 in that case and could miss the interface shaders. The log says: "… names found (N in the archive, M compiled by the game)".
- **Clearer instructions.** The launch option `WINEDLLOVERRIDES=…` is for Linux / Steam Deck only: on Windows the game doesn't start with it. Added how to keep your own DXVK: rename its `d3d11.dll` to `d3d11_dxvk.dll` and keep its `dxgi.dll`.

### 2.1-alpha — 2026-11-21 (experimental)

**New**
- **DXVK on Windows.** The mod can hand the work to DXVK (graphics through Vulkan) instead of the system `d3d11.dll`. The add-on `…-DXVK-addon.zip` in the release is the official DXVK 3.1.1, unmodified, both files checked on VirusTotal. If `d3d11_dxvk.dll` lies next to the mod, the mod turns DXVK on by itself; `next_d3d11=` in `ui4k.ini` picks another file, `next_d3d11=system` always the system one.
- **Linux / Steam Deck (Proton), experimental.** DXVK is already built in there; only the launch option `WINEDLLOVERRIDES="d3d11=n,b" %command%` is needed. The mod notes in `ui4k.log` that it runs under Wine.
- "Use at your own risk" note in the archive's instructions.
- If the game crashes, the mod writes into `ui4k.log` where exactly (module and call chain) — that record helps find the cause.

**Fixed**
- With DXVK the game crashed when MSAA was changed in the settings: the mod wrapped the game window a second time on top of the driver's wrapper, and calls went round in a loop.

**Tested:** DXVK on Windows, NVIDIA — menu and battle (icons and text the same as without DXVK), scale change on the fly, HDR, MSAA switched both ways, minimizing the window, Steam screenshot (F12). **Not tested:** Linux and Steam Deck, AMD and Intel graphics with DXVK.

### 2.0 — 2026-11-20

The 2.0 release. Everything from the beta plus sharp letters, sharp item icons and fixes found by taking the game apart.

**New**
- **Sharp text.** Interface letters are drawn at high resolution instead of being stretched (files `ui4k_fonts2.dat` and `ui4k_fonts4.dat`). Letters of other languages and icons are as in the game.
- **Sharp item icons** in the inventory (weapons, ammo, equipment — the game draws them from 3D models). Other interface images (unit cards, buttons, markers) are enlarged as they are.
- **Interface lines** (the view frame on the minimap) are one logical pixel thick, not one screen pixel.
- **Changing the scale on the fly**: after Apply, windows, 3D views and item icons are redrawn at the new scale, no restart needed.
- **Starts at the profile's scale**: 3D views and the first letters no longer stay at the scale of the first list entry.

**Fixed**
- A piece of the world flashed, shifted, at the start and end of a sprint in direct control (field of view change).
- Lighting and fog went wrong while the camera was over a certain spot of the map.
- HBAO+ shading could flash while the camera passed over certain coordinates.
- Water reflection is at the game's size, as without the mod.
- Depth of field in the pause menu no longer drops FPS (blur radius as the game's, `dof_radius=0`).
- The RivaTuner FPS counter and other overlays no longer confuse the mod.
- On 1080p and 1440p monitors the list has no entries the game would not show anyway (smaller than 1360×768).
- A Steam screenshot (F12) no longer shows the world without the interface in a corner.

**For testing**
- `keys=1` in `ui4k.ini` enables Ctrl+F6 / F7 / F9 / F10 to compare with and without a mod fix (off by default: in battle F6–F11 select squad members).

**Not tested yet**
- AMD and Intel graphics (tested on NVIDIA only).
- A 2560×1440 monitor in person (covered by tests).
- MSAA 2x and 8x (4x tested).

If something is wrong, send `ui4k.log` from `binaries\x64` and describe what you did, your graphics card and monitor.

### 2.0 beta — 2026-11-17

The mod has been rewritten from scratch. This is a beta: it is tested in game on NVIDIA with 4K and 21:9 monitors; for the rest see "Not tested yet".

**New**
- **Any monitor**: 2K, 4K, ultrawide 21:9. No need to set the game to 1920×1080 any more.
- **Interface scale of your choice**: ×1, ×1.25, ×1.5, ×1.75, ×2 and "UI like Full HD" (×2 on 4K, ×1.33 on 1440-high monitors).
- **3D world separate from the interface**: at the monitor's native resolution or lower (1440, 1080). A smaller world gives more frames per second while the interface stays sharp.
- **Everything is picked in the game itself**: Options → Video → Resolution, entries like `2560×1440  ×2` (world × interface scale) with Russian and English labels. Switch any time with Apply.
- **Two monitors**: the list has the entries of the monitor the game is on plus an "other monitor" entry to move to the second one. The list follows 16:9 and 21:9.
- **MSAA anti-aliasing** works together with the mod.
- **Brightness, depth of field and HBAO+ shading** look the same as the game's own at the chosen resolution.
- **Game updates**: at start the mod finds the shaders it needs in the game's archive by itself (read only), so a game patch usually does not break it.
- **Installing is just copying files**: the `binaries` folder from the archive into the game folder. Removing is deleting the files; the game sets its resolution back by itself. There are no programs in the archive.
- The mod's version is written to `ui4k.log`, so the log shows which version is installed.
- Direct3D check mode (`d3d_debug=1` in `ui4k.ini`) to find problems on AMD and Intel graphics cards. It has already found one bug, now fixed.

**Not in 2.0 yet (was in 1.0)**
- Redrawn vector fonts: the text is enlarged from the game's original fonts. The `ui4k_fonts` folder from 1.0 is not used by 2.0 and can be deleted.

**Not tested yet**
- AMD and Intel graphics cards (it should work, but has only been tested on NVIDIA).
- A real 2560×1440 16:9 monitor (covered by tests).
- Windows display scaling above 100% (125%, 150%) without the "high DPI" compatibility tick on `call_to_arms.exe`.
- MSAA 2x and 8x (4x is tested).

If something goes wrong, send `ui4k.log` from `binaries\x64` and describe what you did, your graphics card and monitor.

### 1.0 — 2026-11-14
- First version: 4K only, interface exactly ×2 with the game at 1920×1080, 3D at 3840×2160.
- Sharp vector-redrawn fonts at exactly the original sizes.
- Correct splash, cursor and Steam overlay size.
