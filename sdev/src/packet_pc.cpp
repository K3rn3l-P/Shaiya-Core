#include <util/util.h>
#include <util/ini/ini.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <windows.h>
#include <shaiya/include/common/ItemEffect.h>
#include <shaiya/include/common/ItemSlot.h>
#include <shaiya/include/network/game/incoming/0500.h>
#include <shaiya/include/network/game/outgoing/0200.h>
#include "include/main.h"
#include "include/etain_shield.h"
#include "include/security_logger.h"
#include "include/shaiya/CItem.h"
#include "include/shaiya/CObject.h"
#include "include/shaiya/CUser.h"
#include "include/shaiya/CWorld.h"
#include "include/shaiya/CZone.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/NetworkHelper.h"
#include "include/shaiya/UserHelper.h"
using namespace shaiya;

// ---------------------------------------------------------------------------
// Packet flood guard — file-scope state, accessed from packet_pc and hook init
// ---------------------------------------------------------------------------
namespace
{
    struct FloodState { DWORD windowStart; uint32_t count; };
    static std::unordered_map<uint32_t, FloodState> g_flood;
    static CRITICAL_SECTION g_flood_cs;
    static constexpr uint32_t kFloodLimit = 300;  // packets/s threshold
}

namespace packet_pc
{
    void check_packet_flood(CUser* user)
    {
        if (!user || !user->billingId) return;

        DWORD now = GetTickCount();
        EnterCriticalSection(&g_flood_cs);
        auto& st = g_flood[user->billingId];
        if (now - st.windowStart >= 1000) { st.windowStart = now; st.count = 0; }
        ++st.count;
        bool first_over = (st.count == kFloodLimit + 1);
        LeaveCriticalSection(&g_flood_cs);

        if (first_over)
        {
            char detail[48];
            std::snprintf(detail, sizeof(detail), "pkt/s>%u bid=%u", kFloodLimit, user->billingId);
            security_logger::log("server", user->username.data(), "PACKET_FLOOD", detail);
        }
    }

    bool move_to_pending_position(CUser* user, bool& changedMap)
    {
        changedMap = user->mapId != user->moveMapId;

        if (user->mapId != user->moveMapId)
        {
            CWorld::ZoneLeaveUserMove(user, user->moveMapId, user->movePos.x, user->movePos.y, user->movePos.z);

            GameUserSetMapPosOutgoing outgoing{};
            outgoing.objectId = user->id;
            outgoing.mapId = user->moveMapId;
            outgoing.x = user->movePos.x;
            outgoing.y = user->movePos.y;
            outgoing.z = user->movePos.z;
            NetworkHelper::Send(user, &outgoing, sizeof(GameUserSetMapPosOutgoing));

            // EtainShield: reset speed tracking after zone change
            etain_shield::reset_tracking(user);
            return true;
        }

        if (!user->zone)
            return false;

        if (!CZone::MoveUser(user->zone, user, user->movePos.x, user->movePos.y, user->movePos.z))
            return false;

        GameUserSetMapPosOutgoing outgoing{};
        outgoing.objectId = user->id;
        outgoing.mapId = user->moveMapId;
        outgoing.x = user->movePos.x;
        outgoing.y = user->movePos.y;
        outgoing.z = user->movePos.z;
        CObject::SendView(user, &outgoing, sizeof(GameUserSetMapPosOutgoing));

        // EtainShield: reset speed tracking after teleport
        etain_shield::reset_tracking(user);
        return true;
    }

    /// <summary>
    /// Handles incoming 0x55A packets.
    /// </summary>
    void handler_0x55A(CUser* user, GameTownMoveScrollIncoming* incoming)
    {
        if (user->status == UserStatus::Death)
            return;

        if (!incoming->bag || incoming->bag > user->bagsUnlocked)
            return;

        if (incoming->slot >= ItemSlotCount)
            return;

        auto& item = user->inventory[incoming->bag][incoming->slot];
        if (!item)
            return;

        if (item->info->effect != ItemEffect::TownMoveScroll)
            return;

        if (incoming->gateIndex > 2)
            return;

        user->savePosUseBag = incoming->bag;
        user->savePosUseSlot = incoming->slot;
        user->savePosUseIndex = incoming->gateIndex;

        CUser::CancelActionExc(user);
        MyShop::Ended(&user->myShop);
        CUser::ItemUse(user, incoming->bag, incoming->slot, user->id, 0);
    }

    void town_move_scroll_hook(CUser* user)
    {
        // Battleground button movement reuses the same 5s cast/update path as
        // town scrolls, but it is not backed by a consumable item.
        if (!user->savePosUseBag && !user->savePosUseSlot)
        {
            bool changedMap = false;
            move_to_pending_position(user, changedMap);
            return;
        }

        auto& item = user->inventory[user->savePosUseBag][user->savePosUseSlot];
        if (!item)
            return;

        if (item->info->realType != RealType::Consumable)
            return;

        if (item->info->effect != ItemEffect::TownMoveScroll)
            return;

        bool changedMap = false;
        if (move_to_pending_position(user, changedMap))
            CUser::ItemUseNSend(user, user->savePosUseBag, user->savePosUseSlot, changedMap);
    }

    // -----------------------------------------------------------------------
    // 0x0A01 — Security tamper report (client → server, ingame)
    // -----------------------------------------------------------------------
    #pragma pack(push, 1)
    struct SecurityReportIncoming { uint16_t size; uint16_t opcode; uint8_t flags; };
    #pragma pack(pop)

    // ANTI_TAMPER kick policy — letta da GMSecurity.ini [ANTI_TAMPER], LE STESSE chiavi
    // di security_report.php (LogOnly + KickOnDebugger/CheatProc/DllInject/MemPatch).
    // Così la decisione di kick lato gioco == quella del PHP /kickuid (rispetta LogOnly).
    // Cache con refresh ogni 30s per non leggere il file ad ogni pacchetto 0x0A01.
    static bool tamper_should_kick(uint8_t flags)
    {
        static DWORD s_lastLoad   = 0;
        static bool  s_logOnly     = true;
        static bool  s_kDebugger   = false;
        static bool  s_kCheatProc  = false;
        static bool  s_kDllInject  = false;
        static bool  s_kMemPatch   = false;

        DWORD now = GetTickCount();
        if (s_lastLoad == 0 || (now - s_lastLoad) > 30000)
        {
            const wchar_t* ini = L"C:\\ShaiyaServer\\PSM_Client\\Bin\\Data\\GMSecurity.ini";
            s_logOnly    = util::ini::get_value(L"ANTI_TAMPER", L"LogOnly",         1, ini) != 0;
            s_kDebugger  = util::ini::get_value(L"ANTI_TAMPER", L"KickOnDebugger",  0, ini) != 0;
            s_kCheatProc = util::ini::get_value(L"ANTI_TAMPER", L"KickOnCheatProc", 0, ini) != 0;
            s_kDllInject = util::ini::get_value(L"ANTI_TAMPER", L"KickOnDllInject", 0, ini) != 0;
            s_kMemPatch  = util::ini::get_value(L"ANTI_TAMPER", L"KickOnMemPatch",  0, ini) != 0;
            s_lastLoad   = now;
        }

        if (s_logOnly || flags == 0) return false;
        if ((flags & 0x02) && s_kDebugger)  return true;  // DEBUGGER
        if ((flags & 0x08) && s_kCheatProc) return true;  // CHEAT_PROC
        if ((flags & 0x01) && s_kDllInject) return true;  // DLL_INJECT
        if ((flags & 0x04) && s_kMemPatch)  return true;  // MEM_PATCH
        return false;
    }

    void handler_0x0A01(CUser* user, SecurityReportIncoming* pkt)
    {
        if (!user || !pkt) return;
        // Bounds check: scarta pacchetti troncati prima di leggere ->flags (offset 4).
        if (pkt->size < sizeof(SecurityReportIncoming)) return;
        const uint8_t f = pkt->flags;
        char detail[128];
        std::snprintf(detail, sizeof(detail),
            "flags=0x%02X [%s%s%s%s]",
            f,
            (f & 0x01) ? "DLL_INJECT " : "",
            (f & 0x02) ? "DEBUGGER "   : "",
            (f & 0x04) ? "MEM_PATCH "  : "",
            (f & 0x08) ? "CHEAT_PROC"  : "");
        security_logger::log("client", user->username.data(), "TAMPER_REPORT", detail);

        // Kick lato gioco — additivo a /kickuid del PHP (defense-in-depth, kick istantaneo
        // senza giro SQL). Stessa policy [ANTI_TAMPER] del PHP, quindi rispetta LogOnly.
        if (tamper_should_kick(f))
            etain_shield::kick_user(user);
    }

    // Nota: weapon skin usa l'opcode 0x83A (gruppo 0x8xx) gestito in
    // packet_gem::handler — gli opcode 0x0A0x sono scartati dal router top-level
    // del server (categoria opcode&0xFF00 inesistente).

    /// <summary>
    /// EtainShield: validates 0x501 movement packet before the native handler.
    /// Returns 1 (allow) or 0 (drop).
    /// ecx = user, ebp = packet at call site.
    /// </summary>
    int validate_move_packet(CUser* user, GameCharMoveIncoming* packet)
    {
        return etain_shield::validate_movement(user, packet) ? 1 : 0;
    }

    /// <summary>
    /// EtainShield: validates 0x502 basic attack on player.
    /// Returns 1 (allow) or 0 (drop).
    /// </summary>
    int validate_attack_user_packet(CUser* user, GameCharAttackUserIncoming* packet)
    {
        return etain_shield::validate_attack_user(user, packet);
    }

    /// <summary>
    /// EtainShield: validates 0x503 basic attack on mob.
    /// Returns 1 (allow) or 0 (drop).
    /// </summary>
    int validate_attack_mob_packet(CUser* user, GameCharAttackMobIncoming* packet)
    {
        return etain_shield::validate_attack_mob(user, packet);
    }

    /// <summary>
    /// EtainShield AntiCutting v2: validates 0x511 skill-use cast (gate 250ms server-side,
    /// mirror del check client aggirabile — vedi etain_shield::validate_skill_use).
    /// Returns 1 (allow) or 0 (drop).
    /// </summary>
    int validate_skill_use_packet(CUser* user, GameCharSkillUseIncoming* packet)
    {
        return etain_shield::validate_skill_use(user, packet);
    }

    /// <summary>
    /// EtainShield: indagine skillcut (solo log, nessun enforce) — logga se l'item usato
    /// su bersaglio (0x557) è PartyMemberSummon/PartyMemberMoveTo. Vedi
    /// etain_shield::log_channeled_item_use per il dettaglio dell'ipotesi.
    /// </summary>
    void log_item_use_on_target_packet(CUser* user, GameItemUseOnTargetIncoming* packet)
    {
        etain_shield::log_channeled_item_use(user, packet);
    }

    /// <summary>
    /// EtainShield: indagine skillcut — variante item-use SENZA bersaglio esplicito (0x50A),
    /// usata da PartyMemberSummon (somma l'intero party, target implicito = self).
    /// </summary>
    void log_item_use_packet(CUser* user, GameItemUseIncoming* packet)
    {
        etain_shield::log_channeled_item_use_self(user, packet);
    }
}


unsigned u0x4784DB = 0x4784DB;
unsigned u0x479155 = 0x479155;
void __declspec(naked) naked_0x4784D6()
{
    __asm
    {
        pushad
        push ecx  // user (CUser*)
        call packet_pc::check_packet_flood
        add esp,0x4
        popad

        add eax,-0x501

        // EtainShield: intercept opcode 0x501 (movement) — eax == 0
        test eax,eax
        jz case_0x501

        // EtainShield: intercept 0x502 (attack user) — eax == 1
        cmp eax,0x1
        je case_0x502

        // EtainShield: intercept 0x503 (attack mob) — eax == 2
        cmp eax,0x2
        je case_0x503

        // EtainShield AntiCutting v2: intercept 0x511 (cast skill) — eax == 0x10
        cmp eax,0x10
        je case_0x511

        // EtainShield: indagine skillcut, item-use senza bersaglio 0x50A — eax == 0x9
        cmp eax,0x9
        je case_0x50A

        // EtainShield: indagine skillcut, item-use-su-bersaglio 0x557 — eax == 0x56
        cmp eax,0x56
        je case_0x557

        cmp eax,0x59
        je case_0x55A

        // 0x0A01 - 0x501 = 0x500 — tamper report from client
        cmp eax,0x500
        je case_0x0A01

        jmp u0x4784DB

        // --- 0x501: Movement ---
        case_0x501:
        pushad

        push ebp // packet (GameCharMoveIncoming*)
        push ecx // user (CUser*)
        call packet_pc::validate_move_packet
        add esp,0x8
        test eax,eax
        jz drop_0x501

        popad
        xor eax,eax
        jmp u0x4784DB

        drop_0x501:
        popad
        jmp u0x479155

        // --- 0x502: Basic attack on player ---
        case_0x502:
        pushad

        push ebp // packet (GameCharAttackUserIncoming*)
        push ecx // user (CUser*)
        call packet_pc::validate_attack_user_packet
        add esp,0x8
        test eax,eax
        jz drop_0x502

        popad
        mov eax,0x1
        jmp u0x4784DB

        drop_0x502:
        popad
        jmp u0x479155

        // --- 0x503: Basic attack on mob ---
        case_0x503:
        pushad

        push ebp // packet (GameCharAttackMobIncoming*)
        push ecx // user (CUser*)
        call packet_pc::validate_attack_mob_packet
        add esp,0x8
        test eax,eax
        jz drop_0x503

        popad
        mov eax,0x2
        jmp u0x4784DB

        drop_0x503:
        popad
        jmp u0x479155

        // --- 0x511: Skill use (cast) — AntiCutting v2 gate ---
        case_0x511:
        pushad

        push ebp // packet (GameCharSkillUseIncoming*)
        push ecx // user (CUser*)
        call packet_pc::validate_skill_use_packet
        add esp,0x8
        test eax,eax
        jz drop_0x511

        popad
        mov eax,0x10
        jmp u0x4784DB

        drop_0x511:
        popad
        jmp u0x479155

        // --- 0x50A: Item use (no target) — indagine skillcut, SOLO log, mai drop ---
        case_0x50A:
        pushad

        push ebp // packet (GameItemUseIncoming*)
        push ecx // user (CUser*)
        call packet_pc::log_item_use_packet
        add esp,0x8

        popad
        mov eax,0x9
        jmp u0x4784DB

        // --- 0x557: Item use on target — indagine skillcut, SOLO log, mai drop ---
        case_0x557:
        pushad

        push ebp // packet (GameItemUseOnTargetIncoming*)
        push ecx // user (CUser*)
        call packet_pc::log_item_use_on_target_packet
        add esp,0x8

        popad
        mov eax,0x56
        jmp u0x4784DB

        // --- 0x55A: Town move scroll ---
        case_0x55A:
        pushad

        push ebp // packet
        push ecx // user
        call packet_pc::handler_0x55A
        add esp,0x8

        popad

        jmp u0x479155

        // --- 0x0A01: Client tamper report ---
        case_0x0A01:
        pushad

        push ebp // packet (SecurityReportIncoming*)
        push ecx // user (CUser*)
        call packet_pc::handler_0x0A01
        add esp,0x8

        popad

        jmp u0x479155
    }
}

unsigned u0x49DDC8 = 0x49DDC8;
unsigned u0x49DEB5 = 0x49DEB5;
unsigned u0x49E8D1 = 0x49E8D1;
void __declspec(naked) naked_0x49DDBF()
{
    __asm
    {
        cmp eax,0x7
        je town_move_scroll

        // original
        cmp eax,0x1
        jne _0x49DEB5
        jmp u0x49DDC8

        town_move_scroll:
        pushad

        push edi // user
        call packet_pc::town_move_scroll_hook
        add esp,0x4

        popad

        jmp u0x49E8D1

        _0x49DEB5:
        jmp u0x49DEB5
    }
}

void hook::packet_pc()
{
    InitializeCriticalSection(&g_flood_cs);

    // CUser::PacketPC
    util::detour((void*)0x4784D6, naked_0x4784D6, 5);
    // CUser::UpdateResetPosition
    util::detour((void*)0x49DDBF, naked_0x49DDBF, 9);
}
