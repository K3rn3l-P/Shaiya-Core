// ===========================================================================
// itemmall.cpp — Item-mall preview: mostra mount e weapon skin nel paperdoll
//
// Port da Shaiya-Client-Ep6 (src/itemmall.cpp). CLIENT-ONLY: agisce solo sulla
// preview 3D dell'item mall (struct preview base [esi+0xE34C]).
//
// Weapon skin: item type 151 → mappa l'arma impugnata (heldType 1..15 @0x90E3FD)
// + faction (@0x22AA816) via g_mapLight/g_mapFury → weapon type 45-65 →
// scrive type/id nello slot weapon (preview +0x1AD/+0x1BE).
//
// Hook (game.exe, boundary verificati):
//   0x4F4224 (5) weapon_skin_map      0x4F50C1 (7) skip char view tab1
//   0x4F419E (7) show pet tab1         0x4F426C (6) show mount tab1
//   0x4F4319 (6) new costume refresh   patch 0x4F4210 = 0xEB
// ===========================================================================
#include <util/util.h>
#include "include/main.h"

// ===== Addresses =====
#define ADDR_HOOK_TYPEWRITE      0x004F4224
#define ADDR_HOOK_RETN           5
#define RET1_JL                  0x004F4170
#define RET1_END                 0x004F42F8

#define ADDR_HELDTYPE            0x0090E3FD  // byte: 1..15
#define ADDR_FACTION             0x022AA816  // byte: 0=Light, 1=Fury
#define ADDR_COSTYPE             0x0090E605  // byte
#define ADDR_COSID               0x0090E606  // byte

// ===== Preview layout =====
#define OFF_PREVIEW_BASE  0xE34C
#define OFF_TYPE          0x01A8
#define OFF_ID            0x01B9
#define OFF_WPN_TYPE      0x01AD
#define OFF_WPN_ID        0x01BE

// ===== Mapping tables (index = 1..15) =====
__declspec(align(1)) static unsigned char g_mapLight[16] =
{ 0, 45,46,47,48,49,51,53,55,57,58,64,60,62,64,65 };
__declspec(align(1)) static unsigned char g_mapFury[16] =
{ 0, 45,46,47,48,49,52,54,56,57,58,59,61,63,59,65 };

__declspec(naked) void weapon_skin_map()
{
    __asm {

        movzx edx, byte ptr[esp + 0x30]                 // DL = type

        cmp   dl, 151
        jne   after_force

        // Keep ID=X before pushes
        mov   al, byte ptr[esp + 0x18]                  // AL = X
        movzx edi, al                                   // EDI = X

        push  ebx
        push  esi

        // heldType := [ADDR_HELDTYPE]  (1..15)
        mov   eax, ADDR_HELDTYPE
        mov   bl, byte ptr[eax]
        cmp   bl, 1
        jb    restore
        cmp   bl, 15
        ja    restore
        movzx edx, bl                                   // index

        // faction := [ADDR_FACTION] (0=Light/1=Fury)
        mov   eax, ADDR_FACTION
        mov   al, byte ptr[eax]

        // mapped TYPE -> DL
        test  al, al
        jnz   use_fury
        lea   ecx, g_mapLight
        mov   dl, byte ptr[ecx + edx]
        jmp   have_type
        use_fury :
        lea   ecx, g_mapFury
            mov   dl, byte ptr[ecx + edx]
            have_type :
            // Write mapped TYPE/ID to weapon-skin slot
            mov   ecx, dword ptr[esi + OFF_PREVIEW_BASE]
            mov   eax, edi                                  // AL = X
            mov   byte ptr[ecx + OFF_WPN_TYPE], dl
            mov   byte ptr[ecx + OFF_WPN_ID], al

            restore :
        pop   esi
            pop   ebx

            after_force :
        mov ecx, dword ptr[esi + OFF_PREVIEW_BASE]
            movzx eax, bl                              // slot index
            mov   byte ptr[ecx + eax + OFF_TYPE], dl   // TYPE -> [1A8+slot]
            movzx edx, byte ptr[esp + 0x18]            // DL = typeId
            mov   byte ptr[ecx + eax + OFF_ID], dl     // ID   -> [1B9+slot]

            // loop/counters (original)
            mov   byte ptr[esp + 0x17], 1
            mov   eax, dword ptr[esp + 0x1C]
            inc   eax
            add   ebp, 4
            cmp   eax, 18
            mov   dword ptr[esp + 0x1C], eax
            jl    short GO_JL_NORMAL
            mov   eax, RET1_END
            jmp   eax
            GO_JL_NORMAL :
        mov   eax, RET1_JL
            jmp   eax
    }
}


#define ADDR_HOOK_NEW_COS      0x4F4319
#define REFRESH_CALL_189F0     0x418BB0
#define REFRESH_CONT_5AE4      0x4F4324
#define ADDR_HOOK_NEW_COS_LEN  6

#define SRC_COS_ID_0090E2FA    0x90E2FA   // AL = *(byte*)0x0090E2FA
#define SRC_COS_TYPE_0090E2F9  0x90E2F9   // BL = *(byte*)0x0090E2F9


__declspec(naked) void tramp_newcos()
{
    __asm {

        pushfd
        pushad

        // base = [esi + 0xE34C]
        mov   edx, dword ptr[esi + OFF_PREVIEW_BASE]


        mov   eax, SRC_COS_ID_0090E2FA
        mov   al, byte ptr[eax]
        test  al, al
        jz    skip_write_id
        mov   byte ptr[edx + OFF_ID], al          // [+1B9]
        skip_write_id :


        mov   eax, SRC_COS_TYPE_0090E2F9
            mov   bl, byte ptr[eax]
            test  bl, bl
            jz    done_write
            mov   byte ptr[edx + OFF_TYPE], bl        // [+1A8]
            done_write :


        popad
            popfd


            mov   ecx, dword ptr[esi + OFF_PREVIEW_BASE]
            mov   eax, REFRESH_CALL_189F0
            call  eax
            mov   eax, REFRESH_CONT_5AE4
            jmp   eax
    }
}


#define ADDR_SKIP_CHAR_PATCH    0x4F50C1
#define LEN_SKIP_CHAR_PATCH     7
#define ADDR_SKIP_ELSE_JMP      0x4F50D3
#define ADDR_SKIP_DO_JMP        0x4F50D8
#define ADDR_SKIP_CALL          0x4F38D0

__declspec(naked) void tramp_skip_char_view_tab1_v2()
{
    __asm {
        cmp dword ptr[ebp + 0x2F10], 1
        mov ecx, ebp
        je  do_part
        cmp dword ptr[ebp + 0x2F10], 5
        mov ecx, ebp
        je  do_part

        // jmp 004F50D3 (register kirletmeden)
        push ADDR_SKIP_ELSE_JMP
        ret

        do_part :
        // call 004F38D0
        mov eax, ADDR_SKIP_CALL
            call eax

            // jmp 004F50D8
            push ADDR_SKIP_DO_JMP
            ret
    }
}


#define ADDR_SHOW_PET_PATCH     0x4F419E
#define LEN_SHOW_PET_PATCH      7
#define ADDR_SHOW_PET_JE_TGT    0x4F41A7
#define ADDR_SHOW_PET_ELSE_JMP  0x4F4203


__declspec(naked) void tramp_show_pet_tab1_v2()
{
    __asm {
        cmp dword ptr[esi + 0x2F10], 1
        je  do_pet
        cmp dword ptr[esi + 0x2F10], 5
        je  do_pet

        // jmp 004F4203
        push ADDR_SHOW_PET_ELSE_JMP
        ret

        do_pet :
        // jmp 004F41A7
        push ADDR_SHOW_PET_JE_TGT
            ret
    }
}


#define ADDR_SHOW_MOUNT_PATCH   0x4F426C
#define LEN_SHOW_MOUNT_PATCH    6
#define ADDR_MOUNT_CALL         0x43A060
#define ADDR_MOUNT_SUCCESS_JMP  0x4F428C
#define ADDR_MOUNT_TAB5_JMP     0x4F4272

__declspec(naked) void tramp_show_mount_tab1_v2()
{
    __asm {
        // original first line
        mov[esi + 0xE350], eax

        // Tab==1 -> load_mount
        cmp dword ptr[esi + 0x2F10], 1
        je  load_mount
        // Tab==5 -> 004F4272
        cmp dword ptr[esi + 0x2F10], 5
        je  tab5_path


        load_mount :
        mov ecx, [edi + 0x90]
            xor edx, edx
            push ecx
            mov byte ptr[esp + 0x2C], 0xFF   // -1
            push edx
            mov ecx, eax


            mov eax, ADDR_MOUNT_CALL
            call eax

            // jmp 004F428C
            push ADDR_MOUNT_SUCCESS_JMP
            ret

            tab5_path :
        // jmp 004F4272
        push ADDR_MOUNT_TAB5_JMP
            ret
    }
}


void hook::itemmall()
{
    util::write_memory((void*)0x004F4210, 0xEB, 1);
    util::detour((void*)ADDR_HOOK_TYPEWRITE, weapon_skin_map, ADDR_HOOK_RETN);
    util::detour((void*)ADDR_SKIP_CHAR_PATCH, tramp_skip_char_view_tab1_v2, LEN_SKIP_CHAR_PATCH);
    util::detour((void*)ADDR_SHOW_PET_PATCH, tramp_show_pet_tab1_v2, LEN_SHOW_PET_PATCH);
    util::detour((void*)ADDR_SHOW_MOUNT_PATCH, tramp_show_mount_tab1_v2, LEN_SHOW_MOUNT_PATCH);
    util::detour((void*)ADDR_HOOK_NEW_COS, tramp_newcos, ADDR_HOOK_NEW_COS_LEN);
}
