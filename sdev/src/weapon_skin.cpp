// ===========================================================================
// weapon_skin.cpp — Weapon skin server-authoritative (Fase 1, in-memory)
//
// Memorizza per ogni personaggio (key = user->charId, stabile) un override arma
// {type, typeId}. La sostituzione nell'aspetto broadcast avviene in
// user_shape.cpp; qui solo lo store + l'invio 0x83B al client (render locale).
// NB opcode: server→client 0x83B (update) / 0x83C (whitelist); client→server 0x83A.
// NON usare 0x0A03/0x0A04: collidono con handler nativi trade del client.
// Persistenza DB = Fase 2.
// ===========================================================================
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <windows.h>
#include <shaiya/include/common/ItemSlot.h>
#include "include/main.h"
#include "include/security_logger.h"
#include "include/weapon_skin_db.h"
#include "include/shaiya/CGameData.h"
#include "include/shaiya/CItem.h"
#include "include/shaiya/CUser.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/NetworkHelper.h"
using namespace shaiya;

namespace weapon_skin
{
    namespace
    {
        std::unordered_map<uint32_t, Skin> g_skins;

        // Item-lock (approccio B): l'arma-istanza usata come skin non è droppabile/
        // vendibile/tradabile (protetta dall'anti-dupe esistente via block_bound_item).
        // Chiave = CItem.uniqueId (0x38), stabile per-istanza → segue l'item nei move,
        // protegge solo QUELLA copia (i duplicati restano liberi). Accesso da thread
        // diversi (item paths di più utenti) → protetto da mutex.
        std::unordered_set<uint64_t>          g_lockedUids;       // uid bloccati (tutti gli utenti)
        std::unordered_map<uint32_t, uint64_t> g_charLockedUid;   // charId → uid bloccato
        std::mutex                            g_lockMtx;

        // Trova in inventario (bag 0..5) il primo item {type,typeId}; ritorna il suo
        // uniqueId (0 se assente).
        uint64_t find_inventory_item_uid(CUser* user, uint8_t type, uint8_t typeId)
        {
            if (!user)
                return 0;
            for (auto& bag : user->inventory)
                for (auto* item : bag)
                    if (item && item->type == type && item->typeId == typeId)
                        return item->uniqueId;
            return 0;
        }

        // Whitelist server-authoritative {type,typeId} → key (type<<8)|typeId.
        // Caricata da Data\WeaponSkins.ini (stessa cartella di GMSecurity.ini).
        // Refresh ogni 30s: l'utente edita il file senza riavviare ps_game.
        std::unordered_set<uint16_t> g_allowed;

        constexpr const char* kWhitelistIni =
            "C:\\ShaiyaServer\\PSM_Client\\Bin\\Data\\WeaponSkins.ini";

        void load_whitelist()
        {
            static DWORD s_lastLoad = 0;
            DWORD now = GetTickCount();
            if (s_lastLoad != 0 && (now - s_lastLoad) <= 30000)
                return;
            s_lastLoad = now;

            g_allowed.clear();
            FILE* f = nullptr;
            if (fopen_s(&f, kWhitelistIni, "r") != 0 || !f)
                return;

            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                char* p = line;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == ';' || *p == '#' || *p == '[' || *p == '\0' || *p == '\r' || *p == '\n')
                    continue;

                int type = 0, typeId = 0;
                if (sscanf_s(p, "%d , %d", &type, &typeId) == 2
                    && type > 0 && type < 256 && typeId > 0 && typeId < 256)
                {
                    g_allowed.insert(static_cast<uint16_t>((type << 8) | typeId));
                }
            }
            std::fclose(f);
        }

        bool is_allowed(uint8_t type, uint8_t typeId)
        {
            load_whitelist();
            return g_allowed.count(static_cast<uint16_t>((type << 8) | typeId)) != 0;
        }

        // Replica ESATTA del validator equip nativo di ps_game (FUN_00468370): una skin
        // è un'arma reale e dev'essere usabile da razza+classe del giocatore, come lo slot
        // arma nativo. Skin non impugnabile dalla classe → nessuna animazione di corsa →
        // personaggio invisibile in run. Offset ItemInfo verificati (stride 0xA8): country
        // @0x26, class flags @0x27+job. Fazione via user->family (razza, 0x12e), NON
        // user->country. Fail-closed se ItemInfo assente.
        bool usable_by(CUser* user, uint8_t type, uint8_t typeId)
        {
            const auto* info =
                reinterpret_cast<const uint8_t*>(CGameData::GetItemInfo(type, typeId));

            if (!info)
                return false;

            uint8_t fam = static_cast<uint8_t>(user->family);
            uint8_t job = static_cast<uint8_t>(user->job);

            // Fazione/razza: itemInfo.country (Faction) vs user->family (valori raw come
            // nel nativo: Human=0, Elf=1, Vail=2, Deatheater=3).
            switch (info[0x26])
            {
            case 0: if (fam != 0) return false; break;             // Human
            case 1: if (fam != 1) return false; break;             // Elf
            case 2: if (fam != 0 && fam != 1) return false; break; // Light (Human/Elf)
            case 3: if (fam != 3) return false; break;             // Deatheater
            case 4: if (fam != 2) return false; break;             // Vail
            case 5: if (fam != 3 && fam != 2) return false; break; // Fury (Deatheater/Vail)
            default: break;                                        // Neutral → tutte
            }

            // Classe: itemInfo[0x27 + job] != 0 (fighter..priest = job 0..5).
            if (job > 5)
                return false;
            return info[0x27 + job] != 0;
        }

        #pragma pack(push, 1)
        struct WeaponSkinOutgoing
        {
            uint16_t opcode{ 0x083B };
            uint8_t  type{};
            uint8_t  typeId{};
        };
        #pragma pack(pop)

        // Broadcast di zona della vista equip di uno slot (FUN_00492c00 → pacchetto 0x507),
        // proprietario incluso. È DETOURATA da user_shape::hook_send_equip_view, che per il
        // weapon slot sostituisce l'arma con la skin SE il giocatore ne ha una attiva.
        //
        // Serve richiamarla dopo ogni apply/clear della skin: il client, per il player
        // locale, ricava l'arma REALE proprio da questo pacchetto. Con la skin attiva il
        // pacchetto porta la skin, quindi il client non può conoscere l'arma reale da solo;
        // al clear questo re-invio gli consegna l'arma reale e il modello torna corretto
        // anche dopo un cambio arma avvenuto mentre la skin era attiva.
        typedef void(__fastcall* SendEquipViewFn)(void* user, void* edx, int slot);

        void send_weapon_equip_view(CUser* user)
        {
            reinterpret_cast<SendEquipViewFn>(0x492C00)(user, nullptr, ItemSlot::Weapon);
        }
    }

    const Skin* get(CUser* user)
    {
        if (!user)
            return nullptr;

        auto it = g_skins.find(user->charId);
        if (it == g_skins.end() || it->second.type == 0)
            return nullptr;

        return &it->second;
    }

    void set(CUser* user, uint8_t type, uint8_t typeId)
    {
        if (!user)
            return;

        g_skins[user->charId] = Skin{ type, typeId };
    }

    void clear(CUser* user)
    {
        if (!user)
            return;

        g_skins.erase(user->charId);
    }

    // Blocca l'item-istanza usata come skin (l'arma corrispondente alla skin attiva),
    // sbloccando quella precedente del char. Chiamato su apply-set e load_persisted.
    void lock_skin_source(CUser* user)
    {
        if (!user)
            return;
        const Skin* s = get(user);
        uint64_t newUid = s ? find_inventory_item_uid(user, s->type, s->typeId) : 0;

        std::lock_guard<std::mutex> lk(g_lockMtx);
        auto it = g_charLockedUid.find(user->charId);
        if (it != g_charLockedUid.end())
        {
            g_lockedUids.erase(it->second);
            g_charLockedUid.erase(it);
        }
        if (newUid != 0)
        {
            g_lockedUids.insert(newUid);
            g_charLockedUid[user->charId] = newUid;
        }
    }

    void unlock_skin_source(CUser* user)
    {
        if (!user)
            return;
        std::lock_guard<std::mutex> lk(g_lockMtx);
        auto it = g_charLockedUid.find(user->charId);
        if (it != g_charLockedUid.end())
        {
            g_lockedUids.erase(it->second);
            g_charLockedUid.erase(it);
        }
    }

    // Chiamata dall'anti-dupe (etain_shield::block_bound_item) su drop/trade/battle/
    // market/guild-WH: true → item bloccato (in uso come weapon skin) → azione negata.
    bool is_item_locked(void* item)
    {
        if (!item)
            return false;
        uint64_t uid = *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(item) + 0x38);
        if (uid == 0)
            return false;
        std::lock_guard<std::mutex> lk(g_lockMtx);
        return g_lockedUids.count(uid) != 0;
    }

    // Logout: rimuove il lock del char (pulizia del set; gli item vengono liberati).
    void on_player_leave(CUser* user)
    {
        unlock_skin_source(user);
    }

    void send_update(CUser* user)
    {
        if (!user)
            return;

        WeaponSkinOutgoing out{};
        if (const auto* skin = get(user))
        {
            out.type = skin->type;
            out.typeId = skin->typeId;
        }

        NetworkHelper::Send(user, &out, sizeof(out));
    }

    // Notifica al client che la skin è stata rifiutata (0x83D) → "Cannot equip".
    void send_deny(CUser* user)
    {
        if (!user)
            return;
        uint16_t out = 0x083D;
        NetworkHelper::Send(user, &out, sizeof(out));
    }

    // Invia la whitelist al client (0x83C) all'enter-world: TUTTE le skin (il pannello
    // admin le mostra tutte), ognuna con un flag `usable` per la classe del giocatore.
    // Payload = [u16 opcode][u8 count][(u8 type, u8 typeId, u8 usable) * count].
    void send_whitelist(CUser* user)
    {
        if (!user)
            return;

        load_whitelist();

        #pragma pack(push, 1)
        struct { uint16_t opcode; uint8_t count; uint8_t data[510]; } out{};
        #pragma pack(pop)
        out.opcode = 0x083C;

        int n = 0;
        for (uint16_t key : g_allowed)
        {
            if (n >= 170)   // data[510] / 3 byte per entry
                break;
            uint8_t t   = static_cast<uint8_t>(key >> 8);
            uint8_t tid = static_cast<uint8_t>(key & 0xFF);
            // TUTTE le skin (il pannello admin le mostra tutte). Per ognuna il flag
            // `usable` = se il giocatore può equipaggiarla (classe/fazione) → il client
            // le colora di conseguenza.
            out.data[n * 3]     = t;
            out.data[n * 3 + 1] = tid;
            out.data[n * 3 + 2] = usable_by(user, t, tid) ? 1 : 0;
            ++n;
        }
        out.count = static_cast<uint8_t>(n);

        NetworkHelper::Send(user, &out, 3 + n * 3);
    }

    // Applica/rimuove la skin (chiamato da packet_gem::handler, opcode 0x83A).
    // type==0 = rimuove. Skin valida solo se in whitelist server (WeaponSkins.ini):
    // autorità centralizzata, necessaria per i futuri cosmetici a pagamento.
    void apply(CUser* user, uint8_t type, uint8_t typeId)
    {
        if (!user)
            return;

        // NB: nessun gate admin qui. Le skin sono equipaggiabili da TUTTI i giocatori
        // (whitelist + usabilità classe/fazione). L'autorizzazione riguarda solo il
        // comando /wskin (admin-only, gate client). Il futuro shop aggiungerà un check
        // ownership/entitlement per charId a questo punto.
        if (type == 0)
        {
            clear(user);
            unlock_skin_source(user);                   // sblocca l'arma-skin
            weapon_skin_db::save(user->charId, 0, 0);   // persiste "nessuna skin"
        }
        else if (is_allowed(type, typeId))
        {
            // Fix 2 — la skin dev'essere usabile da classe+fazione (come lo slot arma
            // reale), altrimenti invisibilità in corsa per categoria arma incompatibile.
            if (!usable_by(user, type, typeId))
            {
                char dbg[64];
                std::snprintf(dbg, sizeof(dbg), "DENY class/faction type=%u typeId=%u", type, typeId);
                security_logger::log("client", user->username.data(), "WSKIN", dbg);
                send_deny(user);   // 0x83D → "Cannot equip"
                return;
            }
            // Possesso-item obbligatorio: la skin si equipaggia SOLO se l'item skin è
            // fisicamente in inventario (autorità server, anti-tamper: il client potrebbe
            // inviare 0x83A senza possedere l'item). Coerente con "indossi = possiedi".
            if (find_inventory_item_uid(user, type, typeId) == 0)
            {
                char dbg[64];
                std::snprintf(dbg, sizeof(dbg), "DENY no-item type=%u typeId=%u", type, typeId);
                security_logger::log("client", user->username.data(), "WSKIN", dbg);
                send_deny(user);   // 0x83D → "Cannot equip"
                return;
            }
            set(user, type, typeId);
            lock_skin_source(user);                             // blocca l'arma-skin (no drop/sell/trade)
            weapon_skin_db::save(user->charId, type, typeId);   // persiste la skin
        }
        else
        {
            // Skin non in whitelist: rifiuta (possibile client tampering).
            char dbg[48];
            std::snprintf(dbg, sizeof(dbg), "DENY type=%u typeId=%u", type, typeId);
            security_logger::log("client", user->username.data(), "WSKIN", dbg);
            send_deny(user);   // 0x83D → "Cannot equip"
            return;
        }

        CUser::SendUserShape(user);     // gli altri vedono la skin
        send_update(user);              // 0x83B → render locale
        // 0x507 weapon slot: con skin = skin, senza skin = arma REALE. È l'unica fonte
        // autoritativa dell'arma reale per il client → al clear ripristina il modello
        // corretto anche dopo un cambio arma fatto con la skin attiva.
        send_weapon_equip_view(user);
    }

    // Carica la skin persistita per il personaggio (enter-world) e la riapplica.
    // SICUREZZA / robustezza: il DB è trattato come NON fidato → la skin salvata viene
    // RIVALIDATA (whitelist + classe/fazione) come in apply(). Se non più valida (whitelist
    // cambiata, flag classe DB cambiati, o riga manomessa) → NON applicare e ripulire la
    // persistenza, così non si ripropone ad ogni login e non causa invisibilità.
    // charId è server-side (fidato): il client non può forzare quale skin caricare.
    void load_persisted(CUser* user)
    {
        if (!user)
            return;

        uint8_t type = 0, typeId = 0;
        bool haveSkin = weapon_skin_db::load(user->charId, type, typeId) && type != 0;

        if (haveSkin
            && (!is_allowed(type, typeId)
                || !usable_by(user, type, typeId)
                || find_inventory_item_uid(user, type, typeId) == 0))
        {
            // Skin non più valida (whitelist/flag classe cambiati, riga manomessa) o item
            // skin non più posseduto → pulisce la persistenza e non applica nulla.
            weapon_skin_db::save(user->charId, 0, 0);
            haveSkin = false;
        }

        if (haveSkin)
        {
            set(user, type, typeId);
            lock_skin_source(user);   // ri-blocca l'arma-skin al login
        }

        // Dichiara SEMPRE lo stato skin al client (0x83B), anche senza skin (type=0):
        // clear esplicito all'enter-world → azzera lo stato client stale dopo un cambio
        // personaggio (character screen senza chiudere il gioco). Osservatori via shape.
        send_update(user);
    }
}
