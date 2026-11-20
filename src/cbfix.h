#pragma once
/* F08: экранные константы в константных буферах. Чистая функция — тест tests/test_cbfix.c.
 * Ищет соседние пары (W,H), (1/W,1/H), (W/2,H/2), (2/W,2/H), (0.5/W,0.5/H), (W/4,H/4), (4/W,4/H) логического
 * экрана и заменяет их теми же величинами физического экрана (точные pw/ph, не умножение на k).
 * Возвращает число заменённых пар. */
int cb_fix_floats(float *f, unsigned n, long lw, long lh, long pw, long ph);
/* Э3: то же, но только пары, начинающиеся в at[0..nat) (места из имён переменных шейдеров игры); остальные числа не
 * трогаются и не проверяются. Значение сверяется так же — один регистр в разных проходах значит разное. */
int cb_fix_at(float *f, unsigned n, const unsigned *at, int nat, long lw, long lh, long pw, long ph);
/* то же; exact[j] = 1 — у места at[j] только точное значение экрана, без допуска ±1 пиксель (20.11: место центра экрана —
 * в мировых проходах позиция камеры, допуск принимал камеру в некоторых точках карты за экран). exact NULL — как cb_fix_at */
int cb_fix_at_x(float *f, unsigned n, const unsigned *at, const unsigned char *exact, int nat, long lw, long lh, long pw,
                long ph);
/* Г2: одиночная величина, пропорциональная высоте кадра (HBAO+ RadiusToScreen): пары её не находят. Умножается на то
 * же, на что правка пар изменила высоту: ref — номер числа 1/высота той же структуры, ref_before — его значение до
 * правки пар. 1 — исправлено; 0 — пара не менялась или номера не подходят. */
int cb_fix_follow(float *f, unsigned n, unsigned idx, unsigned ref, float ref_before);
/* Устойчивость к обновлениям: номер RadiusToScreen в буфере HBAO+ — по устройству, а не числом 12. В структуре HBAO+
 * за ним идут R² и −1/R² (16.11: [12] радиус, [13] 324, [14] −0,00308642). Единственный i с f[i] > 0, f[i+1] > 0 и
 * f[i+2]·f[i+1] = −1 (с допуском); −1 — такого места нет или их несколько (20.11: при угле обзора ≈ 41° то же даёт
 * [7..9]). cb_hbao_radius_at — то же условие в одном месте i. */
int cb_hbao_radius_index(const float *f, unsigned n);
int cb_hbao_radius_at(const float *f, unsigned n, unsigned i);
