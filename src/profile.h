/* Настройки из профиля игры (текст options.set). Чистые функции — тестируются отдельно (tests/test_profile.c).
 * Мод профиль только читает. */
#pragma once

/* {hdr} / {hdr 1} -> 1, {hdr 0} -> 0, строки нет -> -1 */
int profile_hdr(const char *text);
/* сглаживание MSAA: {antialiasing none} -> 0; {antialiasing 2x} .. 16x -> число выборок; другое слово
 * (nonmaskable = Supersampling) -> 1; строки нет или значение пустое -> -1 */
int profile_aa(const char *text);
/* строка разрешения: {resolution ... {mode "1920x1080"}} -> 1, *w x *h; нет — 0 ({custom_mode} — не она) */
int profile_mode(const char *text, unsigned *w, unsigned *h);
