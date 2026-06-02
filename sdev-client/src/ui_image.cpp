#include "include/rank_icon.h"
#include <util/util.h>
#include "include/main.h"
#include "include/imgui_layer_internal.h"

namespace
{
    unsigned u0x44EC64 = 0x0044EC64;
    void __declspec(naked) naked_0x44EC5C()
    {
        __asm
        {
            mov eax,dword ptr [esp+0x28]
            add eax,3
            fild dword ptr [esp+0x30]
            jmp u0x44EC64
        }
    }

    unsigned u0x44ECC3 = 0x0044ECC3;
    void __declspec(naked) naked_0x44ECBC()
    {
        __asm
        {
            mov ecx,dword ptr [esp+0x30]
            add ecx,5
            fstp dword ptr [esp]
            jmp u0x44ECC3
        }
    }

    // Override killLv (pannello debug "Edit Rank") — stato rimane in imgui_layer
    int32_t __cdecl apply_kill_lv_override(int32_t original)
    {
        if (!imgui_layer::g_killLvOverrideEnabled)
            return original;
        return imgui_layer::g_killLvOverrideValue;
    }

    // Hook 0x44EBED — displaces 6 bytes (MCP verified):
    //   mov ecx,[esp+0x20] / xor bl,bl
    // Salva kills DOPO override cosi' Override killLv funziona con extended.
    unsigned u0x44EBF3 = 0x0044EBF3;
    void __declspec(naked) naked_0x44EBED()
    {
        __asm
        {
            mov     ecx, dword ptr [esp+0x20]
            xor     bl, bl
            push    ecx
            call    apply_kill_lv_override
            add     esp, 4
            mov     ecx, eax
            mov     dword ptr [rank_icon::g_last_kills], ecx
            jmp     u0x44EBF3
        }
    }

    // Shim cdecl per chiamare rank_icon::compute_icon dall'inline ASM.
    // Necessario perche' MSVC inline ASM non gestisce namespace :: nel CALL.
    static int32_t __cdecl _rank_compute(int32_t bl)
    {
        return rank_icon::compute_icon(bl);
    }

    // Hook 0x44EC4D — displaces 7 bytes (MCP verified):
    //   MOVZX EAX,BL / MOV [ESP+0xC],EAX
    // Setta colonna U in [ESP+0x8] (→ [ESP+0x18] dopo SUB ESP 0x10 → FLD a 0x44EC8A):
    //   0x00000000 = 0.0f = colonna sinistra
    //   0x3F000000 = 0.5f = colonna destra (float verificato MCP: 0x748160=0.5f)
    unsigned u0x44EC54 = 0x0044EC54;
    void __declspec(naked) naked_0x44EC4D()
    {
        __asm
        {
            movzx   eax, bl
            mov     dword ptr [esp+0xc], eax

            push    eax
            call    _rank_compute
            add     esp, 4
            mov     dword ptr [esp+0xc], eax

            cmp     dword ptr [rank_icon::g_u_col], 0
            je      SHORT col_left
            mov     dword ptr [esp+0x8], 0x3F000000   // 0.5f = colonna destra
            jmp     SHORT col_done
            col_left:
            mov     dword ptr [esp+0x8], 0             // 0.0f = colonna sinistra
            col_done:
            jmp     u0x44EC54
        }
    }

    // Hook 0x44EC64 — displaces 6 bytes: FLD double ptr [0x0074A9C0].
    // Extended ON  → UV 1/32 (1024px / 32 = 32px per icona, no doppio)
    // Extended OFF → UV 1/16 nativa
    static const double s_uvScale16 = 1.0 / 16.0;
    static const double s_uvScale32 = 1.0 / 32.0;
    unsigned u0x44EC6A = 0x0044EC6A;
    void __declspec(naked) naked_0x44EC64()
    {
        __asm
        {
            cmp     dword ptr [rank_icon::g_enabled], 0
            je      SHORT uv_off
            fld     qword ptr [s_uvScale32]
            jmp     u0x44EC6A
            uv_off:
            fld     qword ptr [s_uvScale16]
            jmp     u0x44EC6A
        }
    }
}

void hook::ui_image()
{
    util::detour((void*)0x0044EC5C, naked_0x44EC5C, 8);
    util::detour((void*)0x0044ECBC, naked_0x44ECBC, 7);
    util::detour((void*)0x0044EBED, naked_0x44EBED, 6);
    util::detour((void*)0x0044EC4D, naked_0x44EC4D, 7);
    util::detour((void*)0x0044EC64, naked_0x44EC64, 6);
}
