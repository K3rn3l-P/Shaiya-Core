// Recreation Viewer — allarga la banda Y del hit-test hover sullo slot recreation
// così l'armatura/preview si mostra su un'area verticale più ampia (32px -> 136px).
//
// Sito hook: game.exe 0x4C42BF, dentro FUN_004c4f90 (render UI recreation).
// Bytes originali (7) verificati con Ghidra MCP su game.exe corrente:
//     004c42bf: ADD EAX,0x20    (83 C0 20, 3 bytes)   ; bottom = top + 0x20
//     004c42c2: CMP EDX,EAX     (3B D0,    2 bytes)   ; mouse Y (EDX = [0x7c3c10]) vs bottom
//     004c42c4: JG  0x004c4336  (7F xx,    2 bytes)   ; sotto bottom -> skip
// Continuazione: 0x4C42C6 (dentro banda). Skip target: 0x4C4336.
// Il hook rimpiazza l'immediato 0x20 con 0x88 (imm32) mantenendo la stessa logica.
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <util/util.h>
#include "include/main.h"

unsigned u0x4C42C6 = 0x4C42C6;
unsigned u0x4C4336 = 0x4C4336;

__declspec(naked) void naked_0x4C42BF()
{
    __asm
    {
        add eax, 0x88
        cmp edx, eax
        jg  _skip
        jmp u0x4C42C6
    _skip:
        jmp u0x4C4336
    }
}

void hook::recreation_viewer()
{
    util::detour((void*)0x4C42BF, naked_0x4C42BF, 7);
}
