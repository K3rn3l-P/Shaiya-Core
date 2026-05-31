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

    int32_t __cdecl apply_kill_lv_override(int32_t original)
    {
        if (!imgui_layer::g_killLvOverrideEnabled)
            return original;
        return imgui_layer::g_killLvOverrideValue;
    }

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
            mov     dword ptr [imgui_layer::g_lastRenderedKills], ecx
            jmp     u0x44EBF3
        }
    }

    // Mappa logical icon 0-61 → (uvRow, uCol).
    // Texture 64x1024, 2 colonne x 16 righe, icone 32px:
    //   logical  0-15 → top SX  (U=0.0, row 0-15)
    //   logical 16-30 → top DX  (U=0.5, row 0-14)
    //   logical 31-46 → bot SX  (U=0.0, row 16-31)
    //   logical 47-61 → bot DX  (U=0.5, row 16-30)
    // Scrive g_extendedRankUCol (0=sx, 1=dx), ritorna uvRow.
    int32_t __cdecl compute_extended_rank_icon(int32_t nativeBl)
    {
        if (!imgui_layer::g_extendedRankEnabled)
        {
            imgui_layer::g_extendedRankUCol = (nativeBl > 15) ? 1 : 0;
            return nativeBl;
        }
        int32_t kills = imgui_layer::g_lastRenderedKills;
        const auto& t = imgui_layer::g_extendedThresholdTable;
        int logical = 0;
        for (int i = 61; i >= 0; i--)
        {
            if (kills >= t[i]) { logical = i; break; }
        }
        int uvRow, uCol;
        if      (logical < 16) { uvRow = logical;       uCol = 0; } // top SX, row 0-15
        else if (logical < 31) { uvRow = logical - 16;  uCol = 1; } // top DX, row 0-14
        else if (logical < 47) { uvRow = logical - 15;  uCol = 0; } // bot SX, row 16-31
        else                   { uvRow = logical - 31;  uCol = 1; } // bot DX, row 16-30
        imgui_layer::g_extendedRankUCol = uCol;
        return uvRow;
    }

    // Hook 0x44EC4D — displaces 7 bytes (MCP verified).
    // Setta U_col in [ESP+0x8] (diventa [ESP+0x18] dopo SUB ESP 0x10):
    //   0x00000000 = 0.0f = colonna sinistra
    //   0x3F000000 = 0.5f = colonna destra
    unsigned u0x44EC54 = 0x0044EC54;
    void __declspec(naked) naked_0x44EC4D()
    {
        __asm
        {
            movzx   eax, bl
            mov     dword ptr [esp+0xc], eax

            push    eax
            call    compute_extended_rank_icon
            add     esp, 4
            mov     dword ptr [esp+0xc], eax

            // Applica U_col: 0.0f (sx) o 0.5f (dx) in [ESP+0x8]
            cmp     dword ptr [imgui_layer::g_extendedRankUCol], 0
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
    // Extended ON → UV 1/32 (1024px / 32 = 32px per icona).
    // Extended OFF → UV 1/16 nativa.
    static const double s_uvScale16 = 1.0 / 16.0;
    static const double s_uvScale32 = 1.0 / 32.0;
    unsigned u0x44EC6A = 0x0044EC6A;
    void __declspec(naked) naked_0x44EC64()
    {
        __asm
        {
            cmp     dword ptr [imgui_layer::g_extendedRankEnabled], 0
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
