#pragma once
#include <cstdint>

namespace shaiya
{
    struct CUser;
}

void Main();

namespace weapon_skin
{
    struct Skin { uint8_t type; uint8_t typeId; };
    // Skin attiva per l'utente (nullptr se nessuna). Key = user->id (in-memory, Fase 1).
    const Skin* get(shaiya::CUser* user);
    void set(shaiya::CUser* user, uint8_t type, uint8_t typeId);
    void clear(shaiya::CUser* user);
    // Invia 0x0A03 al client con la skin corrente (per render locale).
    void send_update(shaiya::CUser* user);
    // Invia la whitelist al client (0x0A04) all'enter-world (autorità centralizzata).
    void send_whitelist(shaiya::CUser* user);
    // Applica/rimuove la skin + broadcast + 0x0A03 (da packet_gem::handler, opcode 0x83A).
    void apply(shaiya::CUser* user, uint8_t type, uint8_t typeId);
    // Carica la skin persistita (enter-world) e la riapplica, previa RIVALIDAZIONE
    // (whitelist + classe/fazione). DB trattato come non fidato.
    void load_persisted(shaiya::CUser* user);
    // Item-lock (B): true se l'item è l'arma-istanza in uso come skin → l'anti-dupe
    // (block_bound_item) nega drop/sell/trade. Chiamata con CItem* (void*).
    bool is_item_locked(void* item);
    // Logout: rilascia il lock del personaggio.
    void on_player_leave(shaiya::CUser* user);
}
extern "C" __declspec(dllexport) void DllExport();

namespace packet_gem
{
    int use_item_ability_transfer(shaiya::CUser* user, int bag, int slot);
}

namespace shaiya { struct GameTeleportMoveIncoming; }

namespace packet_main_interface
{
    void handler_0x838(shaiya::CUser* user);
    void handler_0x839(shaiya::CUser* user, shaiya::GameTeleportMoveIncoming* packet);
}

namespace hook
{
    void etain_shield();
    void item_effect();
    void utilities();
    void packet_character();
    void packet_exchange();
    void packet_gem();
    void packet_market();
    void packet_myshop();
    void packet_party();
    void packet_pc();
    void packet_quest();
    void raid_150();
    void packet_reward_item();
    void on_enter_world();
    void packet_shop();
    void user_equipment();
    void user_skill();
    void user_shape();
    void user_status();
    void world_thread();
    void connection_guard();
}
