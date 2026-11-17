/* AMD / Intel (17.11): отладочная проверка Direct3D — слой отладки Windows (компонент «Графические средства»).
 * Драйверы NVIDIA прощают часть неверных вызовов (размеры, форматы, привязки), а драйверы AMD и Intel на них могут
 * падать. Слой отладки проверяет каждый вызов на любой видеокарте и сообщает о таких ошибках.
 * d3d_debug=1 в ui4k.ini (тесты: переменная окружения UI4K_D3D_DEBUG=1): устройство игры создаётся с проверкой, её
 * ошибки и предупреждения — в ui4k.log (по 3 строки на вид сообщения, всего до 300). Компонента нет — устройство
 * без проверки, игра идёт как обычно, в логе строка об этом. Только для поиска ошибок: проверка замедляет игру. */
#include "common.h"
#include <d3d11sdklayers.h>

static volatile LONG g_on;   /* устройство создано с проверкой (очередь сообщений не держим: она держала бы устройство) */
static SRWLOCK g_lk = SRWLOCK_INIT;
static BYTE *g_sdk_base;
static SIZE_T g_sdk_size;

UINT d3dbg_flags(UINT flags)
{
    return g_cfg.d3d_debug ? flags | D3D11_CREATE_DEVICE_DEBUG : flags;
}

/* создание с проверкой не вышло, потому что слоя отладки в системе нет: 1 — создать заново без неё */
int d3dbg_retry(HRESULT hr, UINT asked)
{
    if (!(asked & D3D11_CREATE_DEVICE_DEBUG) || hr != DXGI_ERROR_SDK_COMPONENT_MISSING) return 0;
    static volatile LONG said;
    if (!InterlockedExchange(&said, 1))
        LOG("d3d debug: the Direct3D debug layer is not installed (Windows optional feature \"Graphics Tools\") - device "
            "created without it");
    return 1;
}

/* вызов изнутри слоя отладки (он проверяет настоящие размеры, как сама D3D) */
int d3dbg_from_sdk(void *ra)
{
    return g_sdk_base && (BYTE *)ra >= g_sdk_base && (BYTE *)ra < g_sdk_base + g_sdk_size;
}

void d3dbg_device(ID3D11Device *d)
{
    if (!g_cfg.d3d_debug || !d) return;
    ID3D11InfoQueue *q = NULL;
    if (FAILED(ID3D11Device_QueryInterface(d, &IID_ID3D11InfoQueue, (void **)&q)) || !q) return;   /* устройство без проверки */
    HMODULE m = GetModuleHandleW(L"d3d11_3SDKLayers.dll");
    if (!m) m = GetModuleHandleW(L"D3D11SDKLayers.dll");
    if (m) {
        g_sdk_size = ((IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew))->OptionalHeader.SizeOfImage;
        g_sdk_base = (BYTE *)m;
    }
    ID3D11InfoQueue_SetBreakOnSeverity(q, D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    ID3D11InfoQueue_SetBreakOnSeverity(q, D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
    ID3D11InfoQueue_SetBreakOnSeverity(q, D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
    D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE};
    D3D11_INFO_QUEUE_FILTER f;
    ZeroMemory(&f, sizeof f);
    f.DenyList.NumSeverities = 2;
    f.DenyList.pSeverityList = deny;
    ID3D11InfoQueue_AddStorageFilterEntries(q, &f);
    ID3D11InfoQueue_SetMessageCountLimit(q, 4096);
    ID3D11InfoQueue_Release(q);
    if (InterlockedExchange(&g_on, 1)) return;
    LOG("d3d debug: Direct3D debug layer is on (%ls) - its errors and warnings go to this log", m ? L"SDK layers loaded" : L"?");
}

/* накопленные сообщения устройства d — в лог. Зовётся на Present и при освобождении контекста */
void d3dbg_drain(ID3D11Device *d)
{
    if (!g_on || !d) return;
    ID3D11InfoQueue *q = NULL;
    if (FAILED(ID3D11Device_QueryInterface(d, &IID_ID3D11InfoQueue, (void **)&q)) || !q) return;
    if (!ID3D11InfoQueue_GetNumStoredMessages(q)) { ID3D11InfoQueue_Release(q); return; }
    static struct { int id; int n; } seen[256];
    static int nseen, lines;
    static LONG total[5];
    static const char *const sev[] = {"CORRUPTION", "ERROR", "WARNING", "INFO", "MESSAGE"};
    AcquireSRWLockExclusive(&g_lk);
    UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(q);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(ID3D11InfoQueue_GetMessage(q, i, NULL, &len)) || len < sizeof(D3D11_MESSAGE) || len > 65536) continue;
        D3D11_MESSAGE *msg = (D3D11_MESSAGE *)HeapAlloc(GetProcessHeap(), 0, len);
        if (!msg) break;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(q, i, msg, &len))) {
            int s = (int)msg->Severity >= 0 && (int)msg->Severity < 5 ? (int)msg->Severity : 4;
            total[s]++;
            int k = 0;
            while (k < nseen && seen[k].id != (int)msg->ID) k++;
            if (k == nseen && nseen < 256) { seen[nseen].id = (int)msg->ID; seen[nseen].n = 0; nseen++; }
            if (k < 256 && seen[k].n++ < 3 && lines < 300) {
                lines++;
                LOG("d3d debug: %s #%d: %.*s", sev[s], (int)msg->ID, (int)(msg->DescriptionByteLength ? msg->DescriptionByteLength - 1 : 0),
                    msg->pDescription ? msg->pDescription : "");
            }
        }
        HeapFree(GetProcessHeap(), 0, msg);
    }
    ID3D11InfoQueue_ClearStoredMessages(q);
    static LONG last;
    LONG sum = total[0] + total[1] + total[2];
    if (sum != last && lines >= 300) LOG("d3d debug: %ld corruption, %ld errors, %ld warnings so far (further lines are not logged)",
                                         total[0], total[1], total[2]);
    last = sum;
    ReleaseSRWLockExclusive(&g_lk);
    ID3D11InfoQueue_Release(q);
}
