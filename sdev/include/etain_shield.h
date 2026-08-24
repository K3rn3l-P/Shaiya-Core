#pragma once

namespace shaiya
{
    struct CMob;
    struct CUser;
    struct GameCharMoveIncoming;
    struct GameCharAttackUserIncoming;
    struct GameCharAttackMobIncoming;
    struct GameCharSkillUseIncoming;
    struct GameItemUseOnTargetIncoming;
    struct GameItemUseIncoming;
}

namespace etain_shield
{
    // --- AntiSpeedHack ---

    /// Validates a 0x501 movement packet (speed + move-while-attack checks).
    /// Returns true to allow, false to drop.
    bool validate_movement(shaiya::CUser* user, shaiya::GameCharMoveIncoming* packet);

    /// Resets all per-user tracking state.
    /// Call on teleport, zone change, or any server-initiated position change.
    void reset_tracking(shaiya::CUser* user);

    /// Disconnessione reale della sessione live (closesocket via FUN_004ec760,
    /// sotto la critical section del manager utenti). Verificato su ps_game.exe.
    /// Settare CUser::closeBySession=1 a mano NON disconnette — usare questo.
    void kick_user(shaiya::CUser* user);

    // --- AntiRangeHack ---

    /// PVE range pre-check (0x458000 detour).  Returns 1=allow, 0=block.
    int validate_pve_range(shaiya::CUser* user, shaiya::CMob* mob, int skillRange);

    /// PVP range pre-check (0x457F50 detour).  Returns 1=allow, 0=block.
    int validate_pvp_range(shaiya::CUser* attacker, shaiya::CUser* target, int skillRange);

    /// Basic attack on player (0x502 packet).  Returns 1=allow, 0=drop.
    int validate_attack_user(shaiya::CUser* user, shaiya::GameCharAttackUserIncoming* packet);

    /// Basic attack on mob (0x503 packet).  Returns 1=allow, 0=drop.
    int validate_attack_mob(shaiya::CUser* user, shaiya::GameCharAttackMobIncoming* packet);

    // --- AntiCutting v2 (skillcut server-authoritative) ---

    /// Skill-use packet (0x511).  Mirror del gate client 250ms (0x7c49f8/0x44b331 in game.exe,
    /// aggirabile col patch je->jmp) — qui è server-authoritative, il client non può bypassarlo.
    /// Returns 1=allow, 0=drop (drop solo se cuttingLogOnly=false).
    int validate_skill_use(shaiya::CUser* user, shaiya::GameCharSkillUseIncoming* packet);

    // --- AntiCutting: correlazione item "a canale" (indagine, solo log) ---

    /// Item-use-su-bersaglio (0x557). Logga SOLO se l'item è PartyMemberSummon/PartyMemberMoveTo
    /// (item con canale differito lato server — sospettati di interagire con la recovery skill,
    /// vedi indagine skillcut). Nessun enforce: puramente diagnostico per correlare i timestamp
    /// con gli eventi anticutting esistenti. No-op se l'item non è uno dei due monitorati.
    void log_channeled_item_use(shaiya::CUser* user, shaiya::GameItemUseOnTargetIncoming* packet);

    /// Variante senza bersaglio esplicito (0x50A) — usata da PartyMemberSummon (target
    /// implicito = self/party). Stessa logica di log_channeled_item_use.
    void log_channeled_item_use_self(shaiya::CUser* user, shaiya::GameItemUseIncoming* packet);

    // --- AntiDupe (item bound) ---

    /// True se l'item slot va bloccato perché bound (ItemInfo.reqOg >= 1:
    /// AccountBound/CharacterBound) e il toggle [AntiDupe] BlockBoundItems è attivo.
    /// Fa tutti i null-check internamente. Usato dai detour anti-dupe in utilities.cpp
    /// (drop / trade / battle / player market / guild warehouse). Player WH ESCLUSO.
    bool block_bound_item(void* item);
}
