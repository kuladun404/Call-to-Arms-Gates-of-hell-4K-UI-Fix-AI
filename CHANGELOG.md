# Что изменилось / Changelog

**[Русский](#русский) · [English](#english)**

---

## Русский

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
