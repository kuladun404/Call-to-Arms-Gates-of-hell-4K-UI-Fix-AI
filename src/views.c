/* Э9 (19.11; «их нужно делать только в том масштабе который выбран у игрока. и нужно сделать перерисовку открытого
 * инвентаря после переключения»): после смены масштаба интерфейса на ходу — виды 3D игры заново в новом масштабе и
 * иконки предметов нарисованы заново.
 *
 * Реверс 19.11 (Ghidra, стеки diag_stack):
 *  - иконку предмета игра рисует один раз и держит в общем кэше Stuff::eViewManager (вектор +0x18..+0x20, запись на
 *    пару «предмет, стиль»; 0x7fae10 ищет, 0x7fb090 создаёт: ячейка, модель в общий вид вдвое крупнее, уменьшение 2:1
 *    в ячейку). Записи живут до сообщения 0xfb5 (выгрузка боя): обработчик — слот 1 таблицы eViewManager (0x7fb7f0)
 *    освобождает все записи. Поэтому после смены масштаба открытый инвентарь показывал старые ячейки растянутыми;
 *  - общий вид для моделей (1024x1024 с глубиной, +0x48) и цели интерфейса (1360x768, 1920x1080) держит
 *    Render::eTargetManager. Смена размера экрана (0x4d1080 «screen:apply_set_size») шлёт cmSetScreenModeBegin (0x40f)
 *    и cmSetScreenModeEnd (0x410); обработчик менеджера (слот 1, 0x6482a0) на 0x40f освобождает цели, на 0x410 делает
 *    их заново через «t:create» того же логического размера. Но 0x40f он выполняет, только если байт 0x141575652 = 0,
 *    а под DX11 там 1 — цели не пересоздаются, и вид оставался в масштабе запуска.
 * Мод в момент смены масштаба (preset_apply — в оконном потоке, внутри apply_set_size игры, где та сама делает это без
 * DX11) зовёт обработчик менеджера целей с 0x40f (на время вызова байт-запрет 0) и 0x410 — «t:create» под новым
 * масштабом мода; и обработчик кэша иконок с 0xfb5. Только код игры, только эти два объекта (не рассылка всем).
 * Классы — по именам RTTI, объекты — по адресу таблицы в данных exe, обработчики сверены по коду (cmp/sub с номером
 * сообщения, единственная проверка байта-запрета). Не сошлось (обновление игры) — ничего не делается, строка в лог. */
#include <string.h>
#include "common.h"
#include "rtti.h"

volatile LONG g_presented;   /* был Present: смена масштаба — уже на ходу (не первая строка при запуске) */

typedef ULONG_PTR(*PFN_ON)(void *self, void *msg);
static struct {
    void *tm, *vm;          /* Render::eTargetManager, Stuff::eViewManager */
    PFN_ON tm_on, vm_on;    /* их слот 1 — обработчик сообщений */
    volatile BYTE *gate;    /* байт-запрет освобождения целей при смене режима (под DX11 — 1) */
} V;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

/* память читается (объект-кандидат мог оказаться случайной ячейкой с тем же числом) */
static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const BYTE *)p + n <= (const BYTE *)mi.BaseAddress + mi.RegionSize;
}

/* длина функции по таблице раскрутки: MSVC делит функцию на части (пролог, тело, хвост) — следующие части идут подряд и
 * помечены UNW_FLAG_CHAININFO (обработчик менеджера целей: 0x6482a0, 0x6482cd, 0x648431 — проверка байта-запрета в
 * третьей). Без записи раскрутки (функция-лист) — до maxn. */
static int handler_end(const BYTE *f, SIZE_T maxn, SIZE_T *n)
{
    ULONG64 base = 0, a = (ULONG64)(ULONG_PTR)f;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(a, &base, NULL);
    if (!rf || base + rf->BeginAddress != a) { *n = maxn; return 1; }
    ULONG64 end = base + rf->EndAddress;
    for (int i = 0; i < 16 && end - a < maxn; i++) {
        ULONG64 b2 = 0;
        PRUNTIME_FUNCTION r2 = RtlLookupFunctionEntry(end, &b2, NULL);
        if (!r2 || b2 + r2->BeginAddress != end || !((*(const BYTE *)(b2 + r2->UnwindData) >> 3) & UNW_FLAG_CHAININFO)) break;
        end = b2 + r2->EndAddress;
    }
    *n = end - a < maxn ? (SIZE_T)(end - a) : maxn;
    return *n > 16;
}

static BOOL CALLBACK find(PINIT_ONCE o, PVOID a, PVOID *c)
{
    (void)o; (void)a; (void)c;
    const BYTE *img = (const BYTE *)GetModuleHandleW(NULL);
    ImgSec s[64];
    int ns = img_sections(img, s, 64);
    ULONG64 base = (ULONG64)(ULONG_PTR)img;
    unsigned vt_tm = rtti_vtable(img, base, s, ns, ".?AVeTargetManager@Render@@");
    unsigned vt_vm = rtti_vtable(img, base, s, ns, ".?AVeViewManager@Stuff@@");
    unsigned vt_t = rtti_vtable(img, base, s, ns, ".?AVeTarget@Render@@");
    unsigned at[8];
    /* менеджер целей: объект с этой таблицей, у которого +0x48 — цель (Render::eTarget) */
    int k = vt_tm && vt_t ? img_find_qword(img, s, ns, base + vt_tm, at, 8) : 0;
    for (int i = 0; i < k && i < 8 && !V.tm; i++) {
        void *obj = (void *)(img + at[i]);
        void *scr = readable((BYTE *)obj + 0x48, 8) ? *(void **)((BYTE *)obj + 0x48) : NULL;
        if (scr && readable(scr, 8) && *(ULONG64 *)scr == base + vt_t) V.tm = obj;
    }
    if (V.tm) {
        const BYTE *f = *(const BYTE **)(img + vt_tm + 8);
        static const BYTE sub40f[] = {0x81, 0xE9, 0x0F, 0x04, 0x00, 0x00};   /* sub ecx, 0x40f */
        SIZE_T n;
        unsigned gate = 0;
        if (handler_end(f, 0x400, &n) && code_find(f, n < 0x40 ? n : 0x40, sub40f, sizeof sub40f) >= 0)
            gate = code_rip_cmp0(f, n, (unsigned)(f - img));
        for (int i = 0; gate && i < ns; i++)
            if (s[i].write && gate >= s[i].rva && gate < s[i].rva + s[i].size) { V.gate = (volatile BYTE *)(img + gate); break; }
        if (V.gate) V.tm_on = (PFN_ON)f;
        else V.tm = NULL;
    }
    /* кэш иконок: единственный объект, обработчик начинается с cmp dword [rdx+8], 0xfb5 */
    k = vt_vm ? img_find_qword(img, s, ns, base + vt_vm, at, 8) : 0;
    if (k == 1) {
        const BYTE *f = *(const BYTE **)(img + vt_vm + 8);
        static const BYTE cmpfb5[] = {0x81, 0x7A, 0x08, 0xB5, 0x0F, 0x00, 0x00};
        SIZE_T n;
        if (handler_end(f, 0x200, &n) && code_find(f, n < 0x20 ? n : 0x20, cmpfb5, sizeof cmpfb5) >= 0) {
            V.vm = (void *)(img + at[0]);
            V.vm_on = (PFN_ON)f;
        }
    }
    LOG("views: the game's 3D views (Render::eTargetManager) %s; item icon cache (Stuff::eViewManager) %s",
        V.tm ? "found" : "not found (game update?) - after a scale change they stay at the old scale until a restart",
        V.vm ? "found" : "not found (game update?) - open item icons stay stretched after a scale change until the next battle");
    if (V.tm) LOGV("views: target manager at rva 0x%llx, handler 0x%llx, gate byte 0x%llx = %u", (ULONG64)((BYTE *)V.tm - img),
                   (ULONG64)((BYTE *)V.tm_on - img), (ULONG64)((BYTE *)V.gate - img), *V.gate);
    return TRUE;
}

void views_scale_changed(Ratio was, Ratio now)
{
    if (!g_cfg.ui_refresh || !g_presented || ratio_eq(was, now)) return;
    InitOnceExecuteOnce(&g_once, find, NULL, NULL);
    struct { void *vt; UINT id; BYTE pad[0x44]; } m;   /* событие игры: номер на +8 — обработчики читают только его */
    if (V.tm) {
        memset(&m, 0, sizeof m);
        BYTE old = *V.gate;
        *V.gate = 0;
        m.id = 0x40f;   /* cmSetScreenModeBegin: освободить цели */
        V.tm_on(V.tm, &m);
        *V.gate = old;
        m.id = 0x410;   /* cmSetScreenModeEnd: сделать заново — «t:create» уже под новым масштабом */
        V.tm_on(V.tm, &m);
    }
    if (V.vm) {
        memset(&m, 0, sizeof m);
        m.id = 0xfb5;   /* кэш иконок предметов: освободить — игра нарисует их заново */
        V.vm_on(V.vm, &m);
    }
    if (V.tm || V.vm)
        LOG("views: interface x%d/%d -> x%d/%d - %s%s", was.num, was.den, now.num, now.den,
            V.tm ? "the game's 3D views made again at the new scale" : "", V.vm ? (V.tm ? ", item icons drawn again" : "item icons drawn again") : "");
}
