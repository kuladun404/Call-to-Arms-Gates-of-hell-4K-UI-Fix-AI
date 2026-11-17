/* F11 (часть, нужная F04/F05): оверлей Steam (GameOverlayRenderer64.dll) рисует через то же устройство и сам
 * масштабируется по DPI — ему нужны настоящие размеры. Его вызовы узнаём по адресу возврата. */
#include "common.h"

int overlay_call(void *ra)
{
    static BYTE *volatile base;
    static SIZE_T size;
    static volatile DWORD last;
    if (!base) {
        DWORD now = GetTickCount();
        if (last && now - last < 2000) return 0;  /* GetModuleHandle не чаще раза в 2 с, пока оверлея нет */
        last = now ? now : 1;
        HMODULE m = GetModuleHandleW(L"GameOverlayRenderer64.dll");
        if (!m) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
        base = (BYTE *)m;
        LOG("Steam overlay at %p (+0x%zx): its D3D calls pass unscaled", base, size);
    }
    return (BYTE *)ra >= base && (BYTE *)ra < base + size;
}
