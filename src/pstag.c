/* Вердикт 19.11, п. 1: адреса пиксельных шейдеров, которые узнал мод (dof.c — размытие глубины резкости, box.c — копия
 * с шагом), — быстрый отсев в PSSetShader, чтобы не спрашивать метку шейдера на каждом рисовании (~2 000 за кадр).
 * Раньше счётчик считал созданные шейдеры: после N-го создания (если игра пересоздаёт шейдеры — смена настроек, карта)
 * отсев выключался насовсем, и метку спрашивали на каждом рисовании. Теперь в наборе только живые: у шейдера в private
 * data страж, D3D отпускает его, когда шейдер уничтожен, — адрес уходит из набора. Живых больше, чем мест (в архиве
 * игры таких шейдеров 16 и 2), — метку спрашиваем всегда, пока лишние не уйдут. */
#include "common.h"

typedef struct { IUnknownVtbl *vt; volatile LONG ref; PsSet *set; void *ps; } PsGuard;

static HRESULT STDMETHODCALLTYPE g_qi(IUnknown *u, REFIID id, void **o)
{
    if (IsEqualIID(id, &IID_IUnknown)) { *o = u; u->lpVtbl->AddRef(u); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE g_addref(IUnknown *u) { return (ULONG)InterlockedIncrement(&((PsGuard *)u)->ref); }
static ULONG STDMETHODCALLTYPE g_release(IUnknown *u)
{
    PsGuard *g = (PsGuard *)u;
    LONG n = InterlockedDecrement(&g->ref);
    if (n) return (ULONG)n;
    int found = 0;
    for (int i = 0; i < PSSET_N && !found; i++)
        found = InterlockedCompareExchangePointer(&g->set->a[i], NULL, g->ps) == g->ps;
    if (!found) InterlockedDecrement(&g->set->over);   /* он был сверх набора */
    HeapFree(GetProcessHeap(), 0, g);
    return 0;
}
static IUnknownVtbl g_vt = {g_qi, g_addref, g_release};

void psset_add(PsSet *s, ID3D11PixelShader *ps, const GUID *guard, const char *what)
{
    if (!s || !ps) return;
    int put = 0;
    for (int i = 0; i < PSSET_N && !put; i++)
        put = InterlockedCompareExchangePointer(&s->a[i], ps, NULL) == NULL;
    if (!put && InterlockedIncrement(&s->over) == 1) {
        static volatile LONG said;
        if (InterlockedIncrement(&said) <= 4)
            LOG("%s: more than %d such shaders alive at once - their tag is asked at every draw until some are released", what,
                PSSET_N);
    }
    PsGuard *g = (PsGuard *)HeapAlloc(GetProcessHeap(), 0, sizeof *g);
    if (!g) {   /* без стража адрес не уйдёт из набора сам — отсев выключить: метку спрашивать всегда */
        InterlockedIncrement(&s->over);
        return;
    }
    g->vt = &g_vt; g->ref = 1; g->set = s; g->ps = ps;
    /* приставить не вышло — отсев выключить (его место в наборе освободит Release ниже) */
    if (FAILED(ID3D11PixelShader_SetPrivateDataInterface(ps, guard, (IUnknown *)g))) InterlockedIncrement(&s->over);
    IUnknown_Release((IUnknown *)g);
}

int psset_maybe(const PsSet *s, const void *ps)
{
    if (!s || !ps) return 0;
    if (s->over > 0) return 1;
    for (int i = 0; i < PSSET_N; i++)
        if (s->a[i] == ps) return 1;
    return 0;
}
