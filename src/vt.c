/* Патч слотов vtable. У объектов D3D11 (текстуры, устройство) своя встроенная копия vtable — патчим слот каждого объекта.
 * Оригинал обычно один на метод; если встретились разные — храним их по адресу слота.
 * v1 молча возвращал чужой оригинал при переполнении таблицы — здесь полный проход и запись в лог.
 *
 * Долгоживущие объекты с общей таблицей (swapchain, фабрика DXGI, устройство) перехватывает не только мод: оверлей
 * Steam ставит свои функции в те же слоты при каждом создании swapchain («Hooking vtable for swap chain»).
 * Если в этот момент в слоте стоит перехват мода, оверлей запоминает его как «оригинал»; мод, поставив свой перехват
 * снова, получал оверлей своим оригиналом — круг: вызов SetFullscreenState уходил в бесконечную рекурсию и игра
 * падала (включение MSAA пересоздаёт swapchain; проверка в игре 17.11, файл падения: 26 тысяч повторов одного адреса).
 * Правила для таких слотов (vt_patch):
 *  - на время настоящего создания устройства или swapchain мод снимает свои перехваты (vt_suspend) и возвращает после
 *    (vt_resume): чужой перехватчик видит таблицу такой, какой оставил, и не берёт мод своим оригиналом;
 *  - слот, в котором поверх мода уже стоит чужая функция, не переписывается: цепочка «чужой -> мод -> оригинал» цела;
 *  - оригинал хранится на пару «слот + перехват»: на одном слоте могут стоять и два перехвата самого мода
 *    (world.c и diag.c на CreateVertexShader), у каждого свой оригинал.
 * Для короткоживущих объектов со своей таблицей (текстуры, адаптеры, выходы) — vt_patch_obj, как раньше. */
#include "common.h"

#define SLOTMAP (1 << 16)
static struct { void **slot; void *orig; } g_slotmap[SLOTMAP];
static CRITICAL_SECTION g_vtcs;
static DWORD g_tls = TLS_OUT_OF_INDEXES;

typedef struct { void **slot; void *hook; void *orig; VtHook *h; int off; } Pin;
#define MAX_PIN 128
static Pin g_pin[MAX_PIN];   /* в порядке установки: поздний перехват стоит выше раннего */
static int g_npin;
static int g_susp;           /* глубина vt_suspend */
static SRWLOCK g_pinlock = SRWLOCK_INIT;

void vt_init(void)
{
    InitializeCriticalSection(&g_vtcs);
    g_tls = TlsAlloc();
}

static size_t slot_hash(void **slot) { return (size_t)(((size_t)slot >> 3) * 0x9E3779B97F4A7C15ull >> 48); }

static void slotmap_put(void **slot, void *orig)
{
    size_t i = slot_hash(slot);
    for (size_t k = 0; k < SLOTMAP; k++, i = (i + 1) & (SLOTMAP - 1))
        if (!g_slotmap[i].slot || g_slotmap[i].slot == slot) {
            g_slotmap[i].slot = slot;
            g_slotmap[i].orig = orig;
            return;
        }
    static int once;
    if (!once++) LOG("vt_patch: slot map is full (%d entries) - per-object originals are lost", SLOTMAP);
}

static void *slotmap_get(void **slot)
{
    size_t i = slot_hash(slot);
    for (size_t k = 0; k < SLOTMAP; k++, i = (i + 1) & (SLOTMAP - 1)) {
        if (g_slotmap[i].slot == slot) return g_slotmap[i].orig;
        if (!g_slotmap[i].slot) break;
    }
    return NULL;
}

static void write_ptr(void **slot, void *v)
{
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(slot, &mi, sizeof mi) &&
        (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
        *slot = v;
        return;
    }
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *slot = v;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

/* слот ещё существует (объект мог быть удалён) */
static int readable(void **slot)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(slot, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static void remember(VtHook *h, void *cur, size_t off)
{
    for (int i = 0; i < h->n; i++)
        if (h->orig[i] == cur) return;
    if (h->n < 8) h->orig[h->n++] = cur;
    else LOG("vt_patch: more than 8 originals for slot +%zu", off);
    if (h->n > 1) LOG("vt_patch: another original %p for slot +%zu (first %p)", cur, off, h->orig[0]);
}

static int known(const VtHook *h, void *p)
{
    for (int i = 0; i < h->n; i++)
        if (h->orig[i] == p) return 1;
    return 0;
}

/* короткоживущие объекты со своей таблицей (текстуры, адаптеры, выходы) */
void vt_patch_obj(void *obj, size_t off, void *hook, VtHook *h)
{
    void **slot = (void **)((char *)*(void ***)obj + off);
    void *cur = *slot;
    if (cur == hook) return;
    EnterCriticalSection(&g_vtcs);
    h->off = off;
    remember(h, cur, off);
    if (h->n > 1) slotmap_put(slot, cur);
    write_ptr(slot, hook);
    LeaveCriticalSection(&g_vtcs);
}

static Pin *pin_find(void **slot, void *hook)
{
    for (int i = 0; i < g_npin; i++)
        if (g_pin[i].slot == slot && g_pin[i].hook == hook) return &g_pin[i];
    return NULL;
}

/* swapchain, фабрика, устройство: слот может делить с модом чужой перехватчик (оверлей Steam) */
void vt_patch(void *obj, size_t off, void *hook, VtHook *h)
{
    void **slot = (void **)((char *)*(void ***)obj + off);
    AcquireSRWLockExclusive(&g_pinlock);
    void *cur = *slot;
    if (cur == hook) { ReleaseSRWLockExclusive(&g_pinlock); return; }
    Pin *p = pin_find(slot, hook);
    h->off = off;
    if (p && !known(h, cur)) {
        /* слот уже наш, а в нём другая функция, которую мод оригиналом не видел: кто-то встал поверх и держит мод
         * своим оригиналом (оверлей; или второй перехват самого мода). Переписать — значит замкнуть круг. Вызовы и так
         * доходят до этого перехвата через него. */
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 4)
            LOG("vt_patch: slot +%zu already carries another hook on top of the mod's (%p) - left as is", off, cur);
        ReleaseSRWLockExclusive(&g_pinlock);
        return;
    }
    if (!p) {
        if (g_npin < MAX_PIN) p = &g_pin[g_npin++];
        else {
            static int once;
            if (!once++) LOG("vt_patch: more than %d shared slots - the rest are patched without tracking", MAX_PIN);
        }
    }
    h->pin = 1;
    remember(h, cur, off);
    if (p) { p->slot = slot; p->hook = hook; p->orig = cur; p->h = h; p->off = 0; }
    h->pslot = slot;
    h->porig = cur;
    write_ptr(slot, hook);
    ReleaseSRWLockExclusive(&g_pinlock);
}

/* перед настоящим созданием устройства / swapchain: вернуть в общие слоты то, что стояло до мода.
 * С конца: поздний перехват мода стоит поверх раннего. */
void vt_suspend(void)
{
    AcquireSRWLockExclusive(&g_pinlock);
    if (g_susp++ == 0)
        for (int i = g_npin - 1; i >= 0; i--) {
            Pin *p = &g_pin[i];
            p->off = 0;
            if (readable(p->slot) && *p->slot == p->hook) { write_ptr(p->slot, p->orig); p->off = 1; }
        }
    ReleaseSRWLockExclusive(&g_pinlock);
}

/* после него: перехваты мода обратно, в порядке установки; если за это время в слоте появилась чужая функция — она
 * теперь оригинал нижнего перехвата мода (её собственный оригинал — то, что стояло без мода, круга нет) */
void vt_resume(void)
{
    AcquireSRWLockExclusive(&g_pinlock);
    if (g_susp > 0 && --g_susp == 0)
        for (int i = 0; i < g_npin; i++) {
            Pin *p = &g_pin[i];
            if (!p->off) continue;
            p->off = 0;
            if (!readable(p->slot)) continue;
            void *cur = *p->slot;
            if (cur == p->hook) continue;
            if (cur != p->orig) {
                LOG("vt_resume: slot +%zu got another hook underneath (%p, was %p) - chained after the mod's", p->h->off, cur, p->orig);
                p->orig = cur;
                remember(p->h, cur, p->h->off);
                if (p->h->pslot == p->slot) p->h->porig = cur;
            }
            write_ptr(p->slot, p->hook);
        }
    ReleaseSRWLockExclusive(&g_pinlock);
}

void *vt_orig(void *obj, VtHook *h)
{
    void **slot = (void **)((char *)*(void ***)obj + h->off);
    if (h->pin) {
        void *o = NULL;
        AcquireSRWLockShared(&g_pinlock);
        if (h->pslot == slot) o = h->porig;   /* обычный случай: один объект на перехват */
        else
            for (int i = 0; i < g_npin; i++)
                if (g_pin[i].slot == slot && g_pin[i].h == h) { o = g_pin[i].orig; break; }
        ReleaseSRWLockShared(&g_pinlock);
        return o ? o : h->orig[0];
    }
    if (h->n == 1) return h->orig[0];
    void *o = slotmap_get(slot);
    return o ? o : h->orig[0];
}

/* Последний рубеж: перехват вызван из самого себя слишком глубоко — где-то всё-таки замкнулся круг перехватчиков.
 * Лучше вернуть игре ошибку, чем исчерпать стек. vt_enter() -> глубина; после работы — vt_leave(). */
int vt_enter(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES) return 1;
    INT_PTR d = (INT_PTR)TlsGetValue(g_tls) + 1;
    TlsSetValue(g_tls, (LPVOID)d);
    if (d == VT_MAX_DEPTH + 1) {
        static volatile LONG said;
        if (!InterlockedExchange(&said, 1))
            LOG("vt: a hooked call re-entered itself %d times - hook cycle with another program's hooks; call refused", VT_MAX_DEPTH);
    }
    return (int)d;
}

void vt_leave(void)
{
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    INT_PTR d = (INT_PTR)TlsGetValue(g_tls);
    if (d > 0) TlsSetValue(g_tls, (LPVOID)(d - 1));
}
