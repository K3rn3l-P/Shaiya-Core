#pragma once

namespace shaiya
{
    struct CMob;
    struct CUser;
    struct GameCharMoveIncoming;
    struct GameCharAttackUserIncoming;
    struct GameCharAttackMobIncoming;
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

    // --- AntiDupe (item bound) ---

    /// True se l'item slot va bloccato perché bound (ItemInfo.reqOg >= 1:
    /// AccountBound/CharacterBound) e il toggle [AntiDupe] BlockBoundItems è attivo.
    /// Fa tutti i null-check internamente. Usato dai detour anti-dupe in utilities.cpp
    /// (drop / trade / battle / player market / guild warehouse). Player WH ESCLUSO.
    bool block_bound_item(void* item);
}
