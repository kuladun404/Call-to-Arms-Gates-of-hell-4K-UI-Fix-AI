# Replacement fonts / Шрифты-замены

Outline sources for `tools/make_fonts.py`. The game's own bitmap fonts are refitted to these outlines; the fonts themselves are not installed into the game.
Контуры для `tools/make_fonts.py`: растровые шрифты игры перерисовываются по этим контурам; сами файлы в игру не ставятся.

| File | Font | Replaces in game | License |
|---|---|---|---|
| `Oswald-VF.ttf` | Oswald | Oswald, Oswald Light | SIL OFL 1.1 — `licenses/Oswald-OFL.txt` |
| `OpenSans-VF.ttf` | Open Sans | Open Sans Condensed Light | SIL OFL 1.1 — `licenses/OpenSans-OFL.txt` |
| `PTSansNarrow-*.ttf` | PT Sans Narrow | PT Sans Narrow | SIL OFL 1.1 — `licenses/PTSansNarrow-OFL.txt` |
| `RobotoCondensed-VF.ttf` | Roboto Condensed | Roboto Condensed | Apache 2.0 — `licenses/RobotoCondensed-Apache-2.0.txt` |
| `RobotoMono-VF.ttf` | Roboto Mono | Roboto Mono | SIL OFL 1.1 — `licenses/RobotoMono-OFL.txt` |
| `SourceSans3-VF.ttf` | Source Sans 3 | Myriad Pro | SIL OFL 1.1 — `licenses/SourceSans3-OFL.txt` |
| `AnkaCoder-C87-*.ttf` | Anka/Coder Condensed | Anka/Coder Condensed | SIL OFL 1.1 — `licenses/AnkaCoder-OFL.txt` |
| `texgyrebonum-*.otf` | TeX Gyre Bonum | Bookman Old Style | GUST Font License — `licenses/TeXGyre-GUST-FONT-LICENSE.txt` |

Arial, Arial Narrow, Arial Unicode MS and Impact are taken from `C:\Windows\Fonts` at build time and are not redistributed here.
Arial, Arial Narrow, Arial Unicode MS и Impact берутся при сборке из `C:\Windows\Fonts` и здесь не распространяются.
Pobeda has no free outline: it is traced from the game's own largest atlas.
