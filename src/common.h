/* GoH UI4K v2 — общие объявления прокси d3d11.dll. План и журнал: хранилище Obsidian «Call to arms Mracdown». */
#pragma once
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include "scale.h"

#ifndef UI4K_VERSION              /* build.py задаёт: 2.0-<дата правки>-<номер правки git> */
#define UI4K_VERSION "2.0-dev"
#endif

/* ---- config.c (F01) ---- */
typedef struct {
    int enabled;   /* 0 = чистый проброс, хуки не ставятся */
    Ratio scale;   /* физическое / логическое разрешение */
    int log;       /* 0 нет, 1 основное, 2 подробно */
    int cbfix[16], ncbfix;  /* F08 cb_fix: размеры CB (в float) с экранными константами */
    int scan_cb;            /* F08: писать найденные экранные пары в лог */
    int stretch_copy;       /* F09: копию обычная -> масштабированная растягивать (иконки инвентаря) */
    int fit_native;         /* F03: подогнать k под родной экран, игре — логический список режимов */
    int diag, diag_at;      /* разведка F14: снимок кадра по Ctrl+F11 / на N-м Present */
    int render;             /* F14: разрешение 3D-сцены в процентах от монитора (100 = как монитор) */
    int render_w;           /* F14: или ширина рендера в пикселях (render=2560x1440), 0 — проценты */
    Ratio scene;            /* F14: коэффициент текстур сцены = scale * render / 100 */
    int presets;            /* L8: список разрешений игры = строки «мир + масштаб» */
    int labels;             /* L8: подписи строк списка разрешений (loc.c) */
    int lum_fix;            /* автоэкспозиция по всей картинке мира (lum.c) */
    BYTE ui_vs[8]; int has_ui_vs;   /* world.c: ещё один VS интерфейса (контрольная сумма DXBC, ui_vs=16 hex) */
    int game_hdr;           /* HDR в настройках игры при запуске: 1 вкл, 0 выкл, -1 не знаем (config.c, только чтение) */
    int test_mw, test_mh;   /* monitor=WxH — считать монитор таким (ручная правка; в тестах F13 — test_monitor) */
    int test_dsr_w, test_dsr_h;   /* только тесты (С5): test_dsr=WxH — в списке Windows есть режим больше монитора */
    /* В0: сглаживание MSAA в настройках игры: 0 выкл, >0 число выборок (1 — включено, сколько — не знаем), -1 не знаем.
     * При запуске — из профиля (только чтение); во время игры — замечено по текстурам (resources.c) */
    volatile LONG game_aa;
    int msaa_safe;          /* 1 — при MSAA мир рисуется в размере интерфейса (безопасный режим, запасной); 0 — в размере строки */
    int dpi_aware;          /* В1: объявить процесс понимающим DPI (dpi.c); 0 — не трогать */
    int hbao_radius;        /* Г2: номер RadiusToScreen в буфере HBAO+; -1 — по устройству буфера (по умолчанию), 0 — не трогать */
    int dof_radius;         /* Г4: радиус размытия глубины резкости x s^dof_radius (dof.c); 0 — не трогать */
    int all_monitors;       /* строки для всех мониторов, строка переносит игру на свой монитор (modes.c); 0 — только основной */
    int test_mon;           /* только тесты: test_monitor= задан — настоящие мониторы не смотреть */
    int test_m2w, test_m2h; /* только тесты: test_monitor2=WxH — второй монитор справа от основного */
    int d3d_debug;          /* 1 — слой отладки Direct3D, его ошибки в лог (d3dbg.c; тесты: UI4K_D3D_DEBUG=1) */
    int perf, perf_every;   /* Э1: режим замеров (perf.c); perf_every=N — строка в лог каждые N кадров (тесты) */
    volatile LONG origin;   /* Э2: кэши окон по происхождению («t:create» в exe); Ctrl+F7 — вкл/выкл в игре */
    volatile LONG cb_names; /* Э3: в буфере сцены править только места из имён переменных шейдеров; Ctrl+F6 */
    volatile LONG thick_lines; /* Э5: линии интерфейса толщиной в логический пиксель; Ctrl+F9 */
    int fonts;              /* Э6: чёткие страницы шрифтов из образцов ui4k_fonts2/4.dat (fontpages.c) */
    int ui_refresh;         /* смена масштаба на ходу: кэши окон прежнего масштаба сбросить, игра нарисует их заново */
    int keys;               /* ревизия v3, п. 2: Ctrl+F6/F7/F9/F10 — переключатели для сравнения; 0 (по умолчанию) — нет */
    UINT diag_on_w, diag_on_h; /* diag_on=WxH: снимок сам, когда игра создаёт кэш интерфейса этого логического размера */
    int box_step;           /* Э8: уменьшение 2:1 шейдером копирования игры (иконки инвентаря) — шаг в настоящих текселях */
    UINT diag_stk[8][2];    /* diag_stack=WxH,...: создание текстуры такого логического размера — в лог со стеком вызовов */
    int ndiag_stk;
    int diag_ring;          /* 20.11: «запись прошлого» — МБ памяти на последние кадры снимка; Ctrl+F11 — в ui4k_ring_N.txt */
} Config;
extern Config g_cfg;
extern WCHAR g_dir[MAX_PATH];                 /* папка DLL с завершающим '\' */
int dir_file(WCHAR *out, const WCHAR *name);  /* out[MAX_PATH] = g_dir + name; не помещается — "" и 0 */
extern WCHAR g_appdata[MAX_PATH];             /* loc.c: запасная папка пакета подписей (тесты: test_appdata=) */
void config_load(HMODULE self);               /* читает ui4k.ini, открывает лог */

void dpi_init(void);                          /* dpi.c (В1): до создания окон */

/* ---- log.c ---- */
void log_open(const WCHAR *path);
void ui4k_log(const char *fmt, ...);
#define LOG(...)  do { if (g_cfg.log >= 1) ui4k_log(__VA_ARGS__); } while (0)
#define LOGV(...) do { if (g_cfg.log >= 2) ui4k_log(__VA_ARGS__); } while (0)

/* ---- proxy.c (F02) ---- */
int ui4k_from_d3d(void *ret_addr);            /* адрес возврата внутри системной d3d11.dll */

/* ---- vt.c: патч слотов vtable (устройство, текстуры, swapchain; контексты — только обёртки) ---- */
typedef struct { void *orig[8]; int n; size_t off; int pin; void **pslot; void *porig; } VtHook;
void  vt_init(void);
void  vt_patch(void *obj, size_t slot_offset, void *hook, VtHook *h);       /* swapchain, фабрика, устройство */
void  vt_patch_obj(void *obj, size_t slot_offset, void *hook, VtHook *h);   /* текстуры, адаптеры, выходы */
void *vt_orig(void *obj, VtHook *h);
void  vt_suspend(void);   /* перед настоящим созданием устройства / swapchain: снять свои перехваты с общих таблиц */
void  vt_resume(void);    /* после него: вернуть */
int   vt_enter(void);     /* глубина вложенности перехваченного вызова в этом потоке */
void  vt_leave(void);
#define VT_MAX_DEPTH 32
#define VT_PATCH(obj, VtblType, field, hook, h) vt_patch((obj), offsetof(VtblType, field), (void *)(hook), (h))
#define VT_PATCH_OBJ(obj, VtblType, field, hook, h) vt_patch_obj((obj), offsetof(VtblType, field), (void *)(hook), (h))
/* вызов оригинала с защитой от круга перехватчиков: слишком глубоко — оригинал не зовём, игре ошибка */
#define VT_GUARDED(call) __extension__ ({ HRESULT hr_; if (vt_enter() > VT_MAX_DEPTH) hr_ = DXGI_ERROR_INVALID_CALL; \
                                          else hr_ = (call); vt_leave(); hr_; })
#define VT_ORIG(obj, h, FnType) ((FnType)vt_orig((obj), (h)))

/* ---- ctx.c (F06): COM-обёртки контекстов ----
 * Рантайм D3D11 сам переписывает слоты vtable контекстов, поэтому игра получает нашу обёртку: все методы через
 * asm-переходники (stubs.S) уходят в настоящий контекст, зарегистрированные — в наши функции.
 * Всё состояние контекста живёт в обёртке (CtxState) — без глобальных таблиц и поиска. */
#define MAX_VP D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
#define MAX_CBMAP 8
#define CB_TRK 4
#define CB_TRKF 512
#define DOF_W 2
#define DOF_F CB_TRKF   /* ревизия кода 4, п. 2: столько же, сколько копия cb.c (буфер сцены сейчас 156) */
#define BOX_W 2
#define BOX_F CB_TRKF
typedef struct CtxState {
    /* F07: то, что задала игра (логическое), и коэффициент привязанной цели (rk.num == 0 — цель не увеличена) */
    Ratio rk;
    UINT nvp, nsc;
    D3D11_VIEWPORT vp[MAX_VP];
    D3D11_RECT sc[MAX_VP];
    /* F08: открытые Map константных буферов, которые надо поправить на Unmap */
    struct { ID3D11Resource *r; float *p; UINT n; } cbm[MAX_CBMAP];
    /* F14/L8: логические копии CB с экранными константами — свои у каждого контекста (отложенные контексты пишут
     * в один буфер разное); applied — под какой коэффициент цели буфер переписан в этом контексте */
    struct { ID3D11Resource *r; UINT n; Ratio applied; float log[CB_TRKF]; } trk[CB_TRK];
    /* world.c: мир без HDR — что привязала игра (back buffer подменён целью мира) */
    ID3D11VertexShader *vs;          /* текущий VS (без ссылки: привязанный шейдер держит сам контекст) */
    int vs_ui;                       /* он — VS интерфейса: 1 да, 0 нет, -1 ещё не спрашивали (метка на шейдере) */
    int zon, hasdsv;                 /* тест глубины включён; привязана глубина */
    int bbbound, bbsub;              /* игра привязала back buffer; он подменён целью мира */
    UINT norig;
    ID3D11RenderTargetView *orig[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];   /* цели игры (со ссылками) */
    ID3D11DepthStencilView *odsv;
    /* Г4 (dof.c): текущий PS и последние записи буфера сцены в этом контексте (со ссылками) */
    ID3D11PixelShader *ps;           /* без ссылки, как vs */
    int ps_dof;                      /* он — шейдер размытия глубины резкости: 1 да, 0 нет, -1 спросить метку */
    int dof_adj, dof_next;           /* какой-то буфер сейчас переписан под размытие; куда писать новый */
    struct { ID3D11Resource *r; UINT n; int adj; float ldim; float f[DOF_F]; } dofw[DOF_W];
    /* Э8 (box.c): текущий PS — шейдер копирования с шагом (1 да, 0 нет, -1 спросить метку); копии буфера с числом шага */
    int ps_box, box_adj, box_next;
    struct { ID3D11Resource *r; UINT n; int adj; float pv[4]; float f[BOX_F]; } boxw[BOX_W];   /* pv — что вписано */
    /* lum.c: текущий вычислительный шейдер; cs_lum — он пересобран модом (1), нет (0), спросить метку (-1) */
    ID3D11ComputeShader *cs;
    int cs_lum;
    D3D11_PRIMITIVE_TOPOLOGY topo;   /* Э5: текущая топология (линии интерфейса утолщаются) */
    void *fp_live;                   /* Э6: в слоте 0 PS — живая страница шрифта (fontpages.c) */
} CtxState;

typedef struct CtxWrap {
    void **vtbl;                  /* +0  наша таблица */
    ID3D11DeviceContext *real;    /* +8  настоящий контекст (читают переходники) */
    volatile LONG ref;
    D3D11_DEVICE_CONTEXT_TYPE type;
    CtxState st;
} CtxWrap;

#define CTXW(c)  ((CtxWrap *)(c))
#define RC(c)    (CTXW(c)->real)
#define ORIGC(c, field, PFN) ((PFN)(RC(c)->lpVtbl->field))
#define CTX_HOOK(field, fn) ctx_hook(offsetof(ID3D11DeviceContext4Vtbl, field), (void *)(fn), #field)

/* Ревизия 20.11, Ф2: оверлеи внутри настоящего Present (RivaTuner, Discord, NVIDIA) рисуют через обёртку — их цели,
 * viewport, шейдеры ложатся в CtxState, — а состояние контекста возвращают мимо неё (SwapDeviceContextState). Простые поля
 * CtxState непосредственного контекста запоминаются до Present и возвращаются после: снова то, что поставила игра */
typedef struct {
    struct CtxWrap *w;
    Ratio rk;
    UINT nvp, nsc;
    D3D11_VIEWPORT vp[MAX_VP];
    D3D11_RECT sc[MAX_VP];
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11ComputeShader *cs;
    int vs_ui, zon, hasdsv, ps_dof, ps_box, cs_lum;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    void *fp_live;
} CtxPlain;
int  ctx_present_save(CtxPlain *p);       /* 1 — запомнено (держит ссылку на обёртку до ctx_present_restore) */
void ctx_present_restore(const CtxPlain *p);

int  ctx_hook(size_t slot_offset, void *fn, const char *name); /* до первой обёртки; 1 = ок */
ID3D11DeviceContext *ctx_wrap(ID3D11DeviceContext *real);    /* забирает ссылку real; одна обёртка на контекст */
int  ctx_is_wrap(const void *c);
ID3D11DeviceContext *ctx_real(ID3D11DeviceContext *c);       /* обёртка -> настоящий, иначе как есть */

/* ---- device.c ---- */
void device_hook(ID3D11Device *dev);

/* ---- screen.c: логический (игра) и физический (рендер) экран ---- */
typedef struct { LONG lw, lh, pw, ph; } Screen;  /* 0 — ещё неизвестен */
Screen screen_get(void);
void screen_set_logical(UINT lw, UINT lh);   /* физический = to_phys */
void screen_set_physical(UINT pw, UINT ph);  /* логический = size_to_log, если экран новый */
void screen_set_format(DXGI_FORMAT f);       /* F14: формат back buffer — текстуры этого семейства относятся к выходу */
int  screen_is_output_format(DXGI_FORMAT f);

/* ---- resources.c (F05) ---- */
typedef struct { UINT lw, lh; Ratio r; } ScaledInfo;  /* логический размер mip0 и коэффициент (интерфейс или сцена) */
int  res_scaled(ID3D11Resource *r, ScaledInfo *out);
int  view_scaled(ID3D11View *v, ScaledInfo *out);
void res_mark(ID3D11Texture2D *t, UINT lw, UINT lh, Ratio r);
Ratio res_ratio(ID3D11Resource *r);   /* коэффициент ресурса; {0,1} — не увеличен */
/* уровень mip увеличенной текстуры: размер, который видит игра (lw x lh), и настоящий (pw x ph) */
void res_mip_size(const ScaledInfo *si, UINT mip, UINT *lw, UINT *lh, UINT *pw, UINT *ph);
UINT res_edge(const ScaledInfo *si, UINT mip, int vertical, UINT x);   /* край области: логический -> настоящий */
void res_hook_device(ID3D11Device *dev);
void res_origin_init(void);   /* Э2: найти «t:create» в образе exe (один раз, при создании устройства) */
void res_origin_key(void);    /* Э2: Ctrl+F7 из Present — вкл/выкл для сравнения */

/* ---- swapchain.c (F04) ---- */
extern volatile DWORD g_cdsc_tid;
extern volatile DWORD g_present_tid;
/* вызов из потока, который сейчас внутри настоящего Present: рисуют оверлеи (RivaTuner, Steam, Discord), не игра */
static inline int in_present(void) { DWORD t = g_present_tid; return t && t == GetCurrentThreadId(); }
void sc_scale_desc(DXGI_SWAP_CHAIN_DESC *d);
void sc_hook(IDXGISwapChain *sc);
void sc_install_iat(void);

/* ---- viewport.c (F07) ---- */
void vp_install(void);                        /* регистрирует хуки контекста; до первой обёртки */
void vp_to_phys(Ratio k, D3D11_VIEWPORT *v);  /* для тестов: правило перевода */
void vp_shift(ID3D11DeviceContext *c, float dx, float dy);   /* Э5: физические viewport контекста со сдвигом */
void vp_apply(ID3D11DeviceContext *c);        /* Э5: вернуть viewport контекста (логические -> физические) */
void draw_key(void);                          /* Э5: Ctrl+F9 из Present — утолщение линий вкл/выкл */

/* ---- upload.c (В0): пиксели игры для увеличенной текстуры ----
 * 0 — не наш случай, передать как есть; 1 — передать пересчитанное (box, data, шаги заменены; tmp отдать upload_done);
 * -1 — вызов в D3D не передавать */
int  upload_prepare(ID3D11Resource *r, UINT sub, const D3D11_BOX **box, const void **data, UINT *rp, UINT *dp, D3D11_BOX *pb,
                    void **tmp, int quirk);
void upload_done(void *tmp);
int  upload_quirk(ID3D11DeviceContext *c, ID3D11Resource *r);   /* отложенный контекст без командных списков драйвера */

/* ---- cb.c (F08), copy.c (F09), input.c (F10), cursor.c (F11) ---- */
void cb_install(void);
void cb_ctx_forget(ID3D11DeviceContext *c);         /* cb.c: копии контекста больше не верны */
void cb_pre_draw(ID3D11DeviceContext *c);          /* F14: CB с экранными константами — под коэффициент цели */
void cb_key(void);                                  /* Э3: Ctrl+F6 из Present — места по именам / все числа */

/* ---- anchors.c: опоры мода узнаются у игры (её архив шейдеров), а не зашиты — устойчивость к обновлениям ---- */
extern WCHAR g_shaderpak[MAX_PATH];                 /* тесты: test_shader_pak= */
int  anchors_ui_vs(const void *code, SIZE_T n);     /* байткод — VS интерфейса по именам из кэша игры */
int  anchors_ui_learned(void);                      /* сколько отпечатков VS интерфейса узнано по именам */
int  anchors_scene_floats(void);                    /* размер буфера сцены из shader_constants.h игры; 0 — не узнали */
const unsigned *anchors_screen_slots(int *n);      /* Э3: экранные места буфера сцены по именам; NULL — не нашлись */
const unsigned char *anchors_screen_exact(void);    /* к ним: 1 — место только с точным значением (центр экрана) */
int  anchors_hbao(UINT floats);                     /* буфер такого размера создала библиотека HBAO+ (до неё — 84) */
void anchors_hook_device(ID3D11Device *dev);

/* ---- dof.c (Г4): радиус размытия глубины резкости ---- */
void  dof_hook_device(ID3D11Device *dev);
void  dof_install(void);                            /* хук PSSetShader; до первой обёртки */
float dof_ldim(const float *f, UINT n);             /* ширина картинки размытия в логическом буфере; 0 — не тот буфер */
void  dof_written(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n, float ldim);  /* что ушло в буфер */
void  dof_pre_draw(ID3D11DeviceContext *c);
void  dof_reset(ID3D11DeviceContext *c);            /* состояние контекста сброшено: PS не привязан */
void  dof_ctx_forget(ID3D11DeviceContext *c);       /* копии буферов больше не верны */
void  dof_overwritten(ID3D11DeviceContext *c, ID3D11Resource *r);  /* box.c переписал буфер: правки радиуса в нём нет */
void  dof_restore(ID3D11DeviceContext *c);          /* вернуть в буфер числа игры, если в нём правка радиуса */

/* ---- views.c (Э9): после смены масштаба на ходу — виды 3D игры в новом масштабе, иконки предметов заново ---- */
extern volatile LONG g_presented;                   /* был Present (смена масштаба уже на ходу) */
void  views_scale_changed(Ratio was, Ratio now);    /* из preset_apply: масштаб интерфейса сменился */

/* ---- pstag.c (вердикт 19.11, п. 1): живые шейдеры, которые узнал мод, — быстрый отсев в PSSetShader ---- */
#define PSSET_N 32
typedef struct { void *volatile a[PSSET_N]; volatile LONG over; } PsSet;   /* over — живых сверх набора */
void psset_add(PsSet *s, ID3D11PixelShader *ps, const GUID *guard, const char *what);   /* страж guard: уничтожен — адрес уходит */
int  psset_maybe(const PsSet *s, const void *ps);   /* 1 — похож (спросить метку); 0 — точно не он */

/* ---- box.c (Э8): шаг уменьшения в шейдере копирования игры (иконки инвентаря) ---- */
int   box_on(void);
void  box_hook_device(ID3D11Device *dev);
int   box_ps_hint(ID3D11PixelShader *ps);            /* из PSSetShader: -1 похож (спросить метку), 0 нет */
void  box_written(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n);   /* что ушло в буфер */
void  box_overwritten(ID3D11DeviceContext *c, ID3D11Resource *r);   /* dof.c переписал буфер */
void  box_pre_draw(ID3D11DeviceContext *c);
void  box_restore(ID3D11DeviceContext *c);          /* вернуть в буфер числа игры, если в нём правка шага */
void  box_ctx_forget(ID3D11DeviceContext *c);
void draw_install(void);                    /* хуки рисования: F14 и диагностика */
void diag_draw(ID3D11DeviceContext *c, const char *what, UINT a, UINT b, int compute);
void copy_install(void);
void copy_tick(void);                                 /* Present: временные текстуры, не нужные 2 с, освободить */
HBITMAP cursor_bmp_scale(HBITMAP src, Ratio k);  /* увеличение картинки курсора */
void cursor_install(void);                        /* IAT: курсор и GDI-заставка */
void input_install(void);                     /* IAT user32 exe */
void input_watch_window(HWND h);              /* обёртка оконной процедуры */
int  ui4k_hotkey(int vk, int force);          /* Ctrl+vk нажаты, а впереди окно игры; force=0 — ещё и keys=1 */
void mon_track(HWND h, const char *why);      /* modes.c: окно игры (h != NULL — запомнить) и его монитор */
void mon_track_msg(HWND h);                   /* окно h сдвинулось — если это окно игры, проверить монитор */
int  mon_misplaced(HWND *h, POINT *to);       /* текущая строка — для монитора другого размера: куда перенести окно */

/* ---- modes.c (F03) ---- */
void modes_fit(void);                               /* k под родной экран; до установки хуков */
long modes_native_width(void);                      /* ширина родного режима монитора */
BOOL modes_enum(LPCWSTR dev, DWORD mode, DEVMODEW *dm);  /* логический список EnumDisplaySettingsW */
void modes_hook_factory(IDXGIFactory *f);           /* DXGI: адаптеры, выходы, списки режимов */
int  from_exe(void *ret_addr);                      /* вызов из exe игры */

/* ---- diag.c: снимок кадра в лог (diag=1) ---- */
extern volatile LONG g_cap;                         /* 1 = идёт снимок */
void loc_add(const char *mode, const char *ru, const char *en); /* loc.c: подпись строки "ШxВ" (UTF-8) */
void presets_build(void);                           /* modes.c, L8: строки «мир + масштаб» */
int presets_on(void);
int msaa_safe(void);                                /* modes.c, В0: MSAA включён — мир в размере интерфейса */
void msaa_seen(UINT samples);                       /* resources.c -> modes.c: игра создала многовыборочную экранную цель */
void msaa_swapchain(UINT samples, int fresh);       /* число выборок нового swapchain; fresh — создан сменой настроек */
void msaa_profile(int aa);                          /* config.c -> modes.c: значение из профиля игры */
void scene_set_base(Ratio r);                       /* мир без строк списка (render=...) — до безопасного режима */
void preset_refresh(void);                          /* состояние MSAA сменилось: мир текущей строки заново */
int  config_poll_profile(void);                     /* config.c: HDR и MSAA из профиля заново (игра меняет их на ходу) */
int  config_profile_mode(UINT *w, UINT *h);         /* строка разрешения из профиля игры; 0 — не знаем */
/* ---- d3dbg.c: слой отладки Direct3D (AMD / Intel: неверные вызовы, которые прощает NVIDIA) ---- */
UINT d3dbg_flags(UINT flags);                       /* флаги создания устройства (+ DEBUG при d3d_debug=1) */
int  d3dbg_retry(HRESULT hr, UINT asked);           /* слоя нет в системе: 1 — создать заново без него */
void d3dbg_device(ID3D11Device *d);                 /* устройство создано: очередь сообщений слоя */
void d3dbg_drain(ID3D11Device *d);                  /* сообщения слоя этого устройства — в лог */
int  d3dbg_from_sdk(void *ra);                      /* вызов изнутри слоя отладки */
void world_hdr_changed(void);                       /* world.c: g_cfg.game_hdr сменился */
void cursor_refresh(void);                    /* cursor.c: после смены масштаба — курсор нового размера */
int preset_apply(UINT lw, UINT lh);                 /* строка выбрана игрой -> масштаб и мир */
int preset_startup(UINT lw, UINT lh);               /* первый swap chain наименьшей строкой — масштаб строки профиля */
void loc_install(void);                             /* loc.c: свой пакет перевода в localizations */
HRESULT res_create_raw(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, ID3D11Texture2D **out);
HRESULT res_create_raw_init(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                            ID3D11Texture2D **out);
int copy_stretch(ID3D11DeviceContext *real, ID3D11Resource *dst, Ratio k, UINT x, UINT y, ID3D11Resource *src,
                 const D3D11_BOX *box);
int copy_resolve(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub, ID3D11Resource *src, UINT ssub, DXGI_FORMAT f,
                 int own);
/* world.c: мир без HDR в своей цели, интерфейс — в back buffer */
void world_install(void);                           /* хуки контекста; до первой обёртки */
void world_hook_device(ID3D11Device *dev);          /* узнаём VS интерфейса */
void world_set_bb(ID3D11Texture2D *bb);             /* swapchain: текущий back buffer */
ID3D11RenderTargetView *const *world_rtv(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                         ID3D11DepthStencilView *dsv, ID3D11RenderTargetView **tmp, void *ret);
void world_pre_draw(ID3D11DeviceContext *c);
ID3D11Resource *world_read(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src);   /* чтение back buffer -> цель мира */
void world_present(void);
void world_frame_start(void);                      /* после Present: следующий кадр начинается с мира */
void world_release_bb(void);                        /* до ResizeBuffers */
void world_reset(ID3D11DeviceContext *c);
int world_hdr_off(void);                           /* HDR выключен (настройки игры или замечено) */
void diag_view_note(ID3D11DeviceContext *c, const char *what, ID3D11View *v);
void diag_install(void);                            /* хуки рисования; до первой обёртки */
void lum_hook_device(ID3D11Device *dev);            /* lum.c: шейдеры автоэкспозиции — из исходника игры */
void lum_install(void);                              /* lum.c: хук CSSetShader */
int  lum_pre_dispatch(ID3D11DeviceContext *c, ID3D11Buffer **prev);   /* 1 — свой буфер привязан, после — lum_post_dispatch */
void lum_post_dispatch(ID3D11DeviceContext *c, ID3D11Buffer *prev);
int  lum_patch(const char *src, size_t n, char **out, size_t *on);     /* правка исходника (тесты) */
void diag_hook_device(ID3D11Device *dev);           /* разбор байткода шейдеров: ld / sample / resinfo */
void diag_present(void);                            /* из Present: Ctrl+F11, начало и конец снимка */
void diag_on_create(UINT w, UINT h);                /* diag_on=WxH: создан кэш этого размера — снимок с этого места */
void diag_stack(const char *tag);                   /* стек вызовов (rva exe) в лог — разведка */
/* ---- perf.c: режим замеров (perf=1, план v3 Э1) ---- */
extern volatile LONG g_perf_cen;                    /* перепись в этом кадре (раз в 64 кадра) */
void perf_present_pre(IDXGISwapChain *sc);          /* перед настоящим Present */
void perf_present_post(void);                       /* после него */
void perf_world_copy(ID3D11DeviceContext *real, int after);   /* world.c: перенос мира своей целью */
void perf_draw(ID3D11DeviceContext *c);             /* draw.c: после world_pre_draw */
int  world_ui_vs(ID3D11DeviceContext *c);           /* world.c: VS контекста — интерфейса */
int  world_ui_draw(ID3D11DeviceContext *c);         /* world.c: рисование интерфейса в экран (непосредственный) */
void diag_copy(ID3D11DeviceContext *c, const char *what, ID3D11Resource *dst, ID3D11Resource *src);
void diag_note(ID3D11DeviceContext *c, const char *fmt, ...);
void diag_cb_raw(ID3D11DeviceContext *c, ID3D11Resource *r, const float *f, UINT n);   /* снимок: прочий буфер констант */

/* ---- fontpages.c (Э6): чёткие буквы шрифтов — свои ячейки букв ---- */
void fontpages_install(void);                       /* хук привязки страниц; до первой обёртки */
void fontpages_hook_device(ID3D11Device *dev);      /* виды рабочих страниц игры */
void fontpages_upload(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp);
void fontpages_copied(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src);   /* после CopyResource */
int  fontpages_vb_wanted(ID3D11DeviceContext *c);   /* заливки этого контекста копировать (непосредственный, есть страницы) */
void fontpages_vb_update(ID3D11Resource *r, const D3D11_BUFFER_DESC *d, const D3D11_BOX *box, const void *data);   /* копия вершин */
void fontpages_vb_touch(ID3D11Resource *r);         /* буфер записан иначе (Copy*) — копия вершин не годится */
void fontpages_tick(void);                          /* Present: освободить атласы, отпущенные 2 с назад; Ctrl+F10 */
int  fontpages_draw(ID3D11DeviceContext *c, UINT n, UINT s);   /* Draw при живой странице в слоте 0; 1 — нарисовано */

/* ---- iat.c, overlay.c ---- */
int iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig);
int overlay_call(void *ret_addr);            /* вызов из оверлея Steam: ему настоящие размеры */
const char *caller_name(void *ret_addr, char *buf, int n);   /* имя модуля по адресу возврата — для лога */
