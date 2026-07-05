// ===========================================================================
// weapon_skin.cpp (client) — render locale della weapon skin
//
// Store client-side della skin {type,typeId} ricevuta dal server (0x0A03).
// Gli ALTRI giocatori vedono la skin già dal pacchetto shape 0x303 (il server
// sostituisce l'arma). Il GIOCATORE LOCALE invece costruisce il proprio modello
// arma da CCharacter.equipment.type[5]/typeId[5] in FUN_004156E0: qui si fa un
// wrapper che, solo per il player locale e solo durante il build del modello,
// scambia type/typeId[5] con la skin e li ripristina subito dopo (le stat reali
// NON vengono toccate — lo swap dura solo la chiamata).
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <util/util.h>
#include <external/imgui/imgui.h>
#include "include/main.h"

// Renderer icona item del client (imgui_layer_items.cpp): icon → sheet DDS → UV → AddImage.
namespace imgui_layer { void draw_item_icon_at(ImDrawList*, const ImVec2&, const ImVec2&, int, int); }

namespace
{
    constexpr uintptr_t kFunBuildWeaponModel = 0x4156E0; // FUN_004156E0
    constexpr uintptr_t kLocalCharIdAddr = 0x90E2F4; // CPlayerData->charId
    constexpr unsigned  kCharIdOffset = 0x34;     // CCharacter->id
    constexpr unsigned  kEquipWeaponType = 0x1AD;    // equipment.type[5]
    constexpr unsigned  kEquipWeaponTypeId = 0x1BE;    // equipment.typeId[5]

    // Puntatore all'oggetto-finestra inventario (disegna inven.png). Catturato
    // dall'hook su FUN_005195f0 (mouse proc inventario). Layout window UI:
    //   +0x04 = X, +0x08 = Y, +0x20 = enabled/open, +0x7c4/+0x7c8 = pos slot weapon (float).
    uintptr_t g_invWindowPtr = 0;

    // Posizione del box skin relativa allo slot weapon (condivisa box imgui + icona nativa).
    constexpr int   kSkinSlotOffX = -39; // a sinistra dello slot weapon (pitch casella nativa)
    constexpr int   kSkinSlotOffY = 0;
    constexpr float kSkinSlotSize = 33.0f;

    // Array item inventario (verificato in FUN_005195f0): cella stride 0x34, bag*0x18+slot.
    //   type   = *(u8*)(0x90e2f9 + (slot + bag*0x18)*0x34)
    //   typeId = *(u8*)(0x90e2fa + (slot + bag*0x18)*0x34)
    // Held item in drag: slot=*(u8*)(window+0xfc), bag=*(u8*)(window+0xfd); 0xff = niente.
    constexpr uintptr_t kInvItemType = 0x90E2F9;
    constexpr uintptr_t kInvItemTypeId = 0x90E2FA;
    constexpr unsigned  kInvCellStride = 0x34;
    constexpr unsigned  kInvBagStride = 0x18;
    constexpr unsigned  kWinHeldSlot = 0xFC;
    constexpr unsigned  kWinHeldBag = 0xFD;
    constexpr uintptr_t kLocalUserPtr = 0x904AA0; // g_pWorldMgr->user (CCharacter*)

    // Stato held tra frame per rilevare il drop (transizione held -> 0xff).
    bool    g_wasHolding = false;
    uint8_t g_lastHeldSlot = 0xFF;
    uint8_t g_lastHeldBag = 0;

    void rebuild_local_weapon_model()
    {
        auto* user = *reinterpret_cast<void**>(kLocalUserPtr);
        if (user)
            reinterpret_cast<void(__fastcall*)(void*)>(kFunBuildWeaponModel)(user);
    }

    // Array equip GLOBALE del player locale (verificato in FUN_0059f060): il gioco vi
    // scrive type/typeId di ogni equip per il player locale, e FUN_004903xx/FUN_004f73xx
    // li ricopiano in CCharacter+0x1a8 ad ogni (ri)costruzione del modello. È la fonte
    // AUTORITATIVA che la selezione animazione (corsa) legge. Lo slot weapon = +5.
    //   type   = *(u8*)(0x914484 + slot)   weapon → 0x914489
    //   typeId = *(u8*)(0x914495 + slot)   weapon → 0x91449A
    constexpr uintptr_t kGlobalEquipType = 0x914484;
    constexpr uintptr_t kGlobalEquipTypeId = 0x914495;
    constexpr unsigned  kWeaponSlot = 5; // ItemSlot::Weapon

    // Arma REALE attualmente equipaggiata dal player locale. Catturata alla PRIMA
    // attivazione di una skin (e aggiornata al cambio arma reale via hook_equip_visual),
    // per poterla ripristinare al clear. g_skinActive evita di ricatturare una skin
    // come "reale" durante uno skin→skin (causava la persistenza dell'invisibilità).
    uint8_t g_realType = 0;
    uint8_t g_realTypeId = 0;
    bool    g_realValid = false;
    bool    g_skinActive = false;

    uint8_t global_weapon_type() { return *reinterpret_cast<uint8_t*>(kGlobalEquipType + kWeaponSlot); }
    uint8_t global_weapon_typeId() { return *reinterpret_cast<uint8_t*>(kGlobalEquipTypeId + kWeaponSlot); }

    // Scrive type/typeId dell'arma nel player locale: sia CCharacter+0x1ad/+0x1be
    // (modello) sia l'array GLOBALE 0x914489/0x91449A (animazione + rebuild). Entrambi
    // sono campi VISIVI — le stat reali vengono dal server/inventario, non da qui.
    void write_local_weapon(uint8_t type, uint8_t typeId)
    {
        auto* user = *reinterpret_cast<void**>(kLocalUserPtr);
        if (user)
        {
            *(static_cast<uint8_t*>(user) + kEquipWeaponType) = type;
            *(static_cast<uint8_t*>(user) + kEquipWeaponTypeId) = typeId;
        }
        *reinterpret_cast<uint8_t*>(kGlobalEquipType + kWeaponSlot) = type;
        *reinterpret_cast<uint8_t*>(kGlobalEquipTypeId + kWeaponSlot) = typeId;
    }

    // Hook su FUN_0059f060 = handler nativo del cambio-equip (guidato dal pacchetto
    // 0x507): legge packet [objectId@+2][slot@+6][type@+7][typeId@+8], costruisce il
    // modello arma reale e lo memorizza. È il punto che mostrava l'arma reale al
    // cambio arma. Dopo l'originale, se è il player locale che equipaggia un'arma
    // (slot 5) e una skin è attiva → riapplichiamo la skin NELLO STESSO frame
    // (l'arma reale non viene mai renderizzata = nessun flicker) + re-invio al
    // server (così anche gli altri ricevono lo shape 0x303 skinnato).
    constexpr uintptr_t kFunEquipVisual = 0x59F060;
    constexpr unsigned  kEquipSlotWeapon = 5; // ItemSlot::Weapon

    // FUN_0059f060 è __stdcall (RET 4): callee pulisce l'argomento sullo stack.
    typedef void(__stdcall* EquipVisualFn)(int packet);
    EquipVisualFn g_origEquipVisual = nullptr;

    void __stdcall hook_equip_visual(int packet)
    {
        g_origEquipVisual(packet); // il gioco scrive l'arma REALE in char+globale e builda

        if (!packet || !weapon_skin::is_set())
            return;

        auto* p = reinterpret_cast<const uint8_t*>(packet);
        uint32_t id = *reinterpret_cast<const uint32_t*>(packet + 2);
        uint8_t slot = p[6];
        uint8_t type = p[7];
        if (id == *reinterpret_cast<uint32_t*>(kLocalCharIdAddr)
            && slot == kEquipSlotWeapon && type != 0)
        {
            // Il player locale ha equipaggiato una nuova arma REALE mentre la skin è
            // attiva: aggiorno l'arma reale memorizzata (per il restore al clear) e
            // ri-applico la skin su modello + globale, poi ricostruisco (no flicker).
            g_realType = type;
            g_realTypeId = p[8];
            g_realValid = true;
            write_local_weapon(weapon_skin::skin_type(), weapon_skin::skin_type_id());
            rebuild_local_weapon_model();
        }
    }

    void build_equip_trampoline()
    {
        auto* tramp = static_cast<BYTE*>(VirtualAlloc(
            nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp)
            return;

        std::memcpy(tramp, reinterpret_cast<void*>(kFunEquipVisual), 7);
        tramp[7] = 0xE9; // jmp rel32
        auto rel = static_cast<int32_t>((kFunEquipVisual + 7) -
            (reinterpret_cast<uintptr_t>(tramp) + 12));
        std::memcpy(tramp + 8, &rel, 4);

        g_origEquipVisual = reinterpret_cast<EquipVisualFn>(tramp);
    }

    // -----------------------------------------------------------------------
    // Equip divert: tasto destro su un item skin-whitelist NON lo equipaggia
    // nello slot weapon → lo applica come SKIN. Hook su FUN_004e88a0 (validatore
    // equip, choke point dei 3 path: right-click 0x519125, drag 0x518020,
    // quick-slot 0x4fec80). __thiscall(this=ECX, bag=[ESP+4], slot=[ESP+8]),
    // RET 8, ritorna 1=equipaggiabile / 0=no. Item whitelist → applico skin +
    // ritorno 0 (blocca equip, item resta in borsa). Il messaggio nativo
    // "non puoi equipaggiare" (0xe5) viene soppresso dal flag sotto.
    // -----------------------------------------------------------------------
    bool g_skinDivertSuppressMsg = false;

    // Ritorna 1 se l'item (bag,slot) è una skin whitelist → applico skin e blocco equip.
    int __cdecl on_equip_validate(unsigned bag, unsigned slot)
    {
        unsigned idx = ((slot & 0xff) + (bag & 0xff) * kInvBagStride) * kInvCellStride;
        uint8_t t = *reinterpret_cast<const uint8_t*>(kInvItemType + idx);
        uint8_t tid = *reinterpret_cast<const uint8_t*>(kInvItemTypeId + idx);
        if (t != 0 && weapon_skin::is_allowed_skin(t, tid))
        {
            weapon_skin::request_set(t, tid);
            g_skinDivertSuppressMsg = true;
            return 1; // blocca l'equip nativo
        }
        return 0;
    }

    // Displaced: MOVZX EAX, byte ptr [ESP+4] (5 byte) → continua a 0x4e88a5.
    unsigned u0x4E88A5 = 0x4E88A5;
    void __declspec(naked) hook_equip_validate()
    {
        __asm
        {
            pushad
            movzx eax, byte ptr[esp + 36]   // bag  (orig [esp+4])
            movzx edx, byte ptr[esp + 40]   // slot (orig [esp+8])
            push edx
            push eax
            call on_equip_validate
            add esp, 8
            test al, al
            popad
            jnz do_block
            movzx eax, byte ptr[esp + 4]    // replica istruzione displaced
            jmp u0x4E88A5
            do_block :
            xor eax, eax                     // ritorna 0 (non equipaggiabile)
                ret 8                            // __thiscall: pulisce bag+slot
        }
    }

    // Soppressione messaggio "non puoi equipaggiare" (0xe5) subito dopo un divert.
    // FUN_00423150 __cdecl(chatType,messageId,color). Displaced: PUSH ESI ;
    // MOV ESI,[ESP+0x10] (5 byte) → continua a 0x423155.
    unsigned u0x423155 = 0x423155;
    void __declspec(naked) hook_sysmsg()
    {
        __asm
        {
            cmp g_skinDivertSuppressMsg, 0
            je sysmsg_pass
            mov eax, [esp + 8]               // messageId
            cmp eax, 0xe5
            jne sysmsg_pass
            mov g_skinDivertSuppressMsg, 0   // consuma il flag + sopprime
            ret
            sysmsg_pass :
            push esi                          // replica istruzioni displaced
                mov esi, [esp + 0x10]
                jmp u0x423155
        }
    }

    typedef void(__fastcall* BuildWeaponModelFn)(void* character);
    BuildWeaponModelFn g_origBuildWeaponModel = nullptr;

    void __fastcall hook_build_weapon_model(void* character)
    {
        // La skin va scritta in modo PERSISTENTE sia in CCharacter+0x1ad (modello) sia
        // nell'array GLOBALE 0x914489/0x91449A (animazione + ricopiatura in char ai
        // reload). FUN_004156E0 costruisce il MODELLO da type[5], ma la selezione
        // dell'ANIMAZIONE (corsa) legge il globale. Se non riscriviamo anche il globale,
        // il modello è la skin ma la corsa usa il rig dell'arma reale → mismatch →
        // personaggio invisibile in corsa. type[5]/globale sono solo ASPETTO (le stat
        // reali vengono dal server/inventario). Qui ri-applichiamo la skin ad ogni
        // rebuild (es. dopo un reload zona che ha ripristinato il globale all'arma reale),
        // ma solo se un'arma è effettivamente equipaggiata (globale weapon != 0).
        if (character && weapon_skin::is_set())
        {
            auto localId = *reinterpret_cast<uint32_t*>(kLocalCharIdAddr);
            auto charId = *reinterpret_cast<uint32_t*>(static_cast<char*>(character) + kCharIdOffset);
            if (charId == localId && global_weapon_type() != 0)
                write_local_weapon(weapon_skin::skin_type(), weapon_skin::skin_type_id());
        }

        g_origBuildWeaponModel(character);
    }

    // Trampolino: 7 byte displaced (PUSH -1 ; PUSH 0x72c7cc) + jmp a entry+7.
    void build_trampoline()
    {
        auto* tramp = static_cast<BYTE*>(VirtualAlloc(
            nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!tramp)
            return;

        std::memcpy(tramp, reinterpret_cast<void*>(kFunBuildWeaponModel), 7);
        tramp[7] = 0xE9; // jmp rel32
        auto rel = static_cast<int32_t>((kFunBuildWeaponModel + 7) -
            (reinterpret_cast<uintptr_t>(tramp) + 12));
        std::memcpy(tramp + 8, &rel, 4);

        g_origBuildWeaponModel = reinterpret_cast<BuildWeaponModelFn>(tramp);
    }

    // Cattura il puntatore finestra inventario (ECX=this) all'entry di FUN_005195f0.
    // Displaced: PUSH -1 (2) + PUSH 0x7357ec (5) = 7 byte → continua a 0x5195f7.
    unsigned u0x5195F7 = 0x5195F7;
    void __declspec(naked) capture_inv_window()
    {
        __asm
        {
            mov g_invWindowPtr, ecx
            push - 1
            push 0x7357ec
            jmp u0x5195F7
        }
    }
}

namespace weapon_skin
{
    namespace
    {
        uint8_t g_type = 0;
        uint8_t g_typeId = 0;

        // Whitelist skin dal server (0x83C). Contiene TUTTE le skin (il pannello admin
        // le mostra tutte); `usable` = se il personaggio locale può equipaggiarla
        // (classe/fazione, calcolato dal server) → usato per colorarle nel pannello.
        struct AllowedSkin { uint8_t type; uint8_t typeId; bool usable; };
        std::vector<AllowedSkin> g_allowedSkins;
    }

    bool is_set() { return g_type != 0; }
    uint8_t skin_type() { return g_type; }
    uint8_t skin_type_id() { return g_typeId; }

    void set_local(uint8_t type, uint8_t typeId)
    {
        g_type = type;
        g_typeId = typeId;
    }

    // Applica il modello arma locale senza toccare la rete. type!=0 → imposta la
    // skin (il build hook la persiste in type[5]). type==0 → ripristina l'arma reale
    // memorizzata in type[5] PRIMA del rebuild (altrimenti type[5] resterebbe = skin
    // e l'arma vera non tornerebbe). Idempotente: usato sia dal path locale sia dal
    // reply server 0x0A03.
    void apply_local_visual(uint8_t type, uint8_t typeId)
    {
        if (type == 0)
        {
            // clear: ripristina l'arma reale (modello + globale) PRIMA del rebuild,
            // altrimenti resterebbe la skin e l'arma vera non tornerebbe (corsa
            // invisibile finché non si cambia arma).
            if (g_realValid)
                write_local_weapon(g_realType, g_realTypeId);
            set_local(0, 0);
            g_skinActive = false;
        }
        else
        {
            // set/replace: alla PRIMA attivazione cattura l'arma reale dal globale
            // (autoritativo). Su skin→skin NON ricatturare (altrimenti salverei la skin
            // precedente come "reale" → al clear resterei invisibile).
            if (!g_skinActive)
            {
                g_realType = global_weapon_type();
                g_realTypeId = global_weapon_typeId();
                g_realValid = true;
                g_skinActive = true;
            }
            set_local(type, typeId);
            write_local_weapon(type, typeId);
        }
        rebuild_local_weapon_model();
    }

    // Invia la richiesta skin al server (0x83A) via window proc (contesto rete
    // corretto). SERVER-AUTHORITATIVE: NON applichiamo nulla in locale qui. Il server
    // valida (admin + whitelist + classe/fazione), ribroadcast lo shape (gli altri
    // vedono) e risponde 0x83B → apply_local_visual (render locale). Se il server
    // rifiuta non arriva 0x83B → niente skin, nemmeno in locale. Così una skin non
    // consentita (classe/fazione sbagliata, non-admin) non è visibile in alcun modo.
    void request_set(uint8_t type, uint8_t typeId)
    {
        // Server-authoritative: invia e basta. Il server valida (whitelist + classe/
        // fazione); se rifiuta risponde 0x83D → il client mostra "Cannot equip".
        HWND hwnd = window::get_game_hwnd();
        if (hwnd && IsWindow(hwnd))
            SendMessageA(hwnd, kClientWeaponSkinMsg, type, typeId);
    }

    void request_clear()
    {
        request_set(0, 0);
    }

    bool is_allowed_skin(uint8_t type, uint8_t typeId)
    {
        for (const auto& s : g_allowedSkins)
            if (s.type == type && s.typeId == typeId)
                return true;
        return false;
    }

    // True se la skin è equipaggiabile dal personaggio locale (classe/fazione).
    bool is_usable_skin(uint8_t type, uint8_t typeId)
    {
        for (const auto& s : g_allowedSkins)
            if (s.type == type && s.typeId == typeId)
                return s.usable;
        return false;
    }

    int allowed_skin_count()
    {
        return static_cast<int>(g_allowedSkins.size());
    }

    bool allowed_skin_get(int index, uint8_t* outType, uint8_t* outTypeId, bool* outUsable)
    {
        if (index < 0 || index >= static_cast<int>(g_allowedSkins.size()))
            return false;
        if (outType)   *outType = g_allowedSkins[index].type;
        if (outTypeId) *outTypeId = g_allowedSkins[index].typeId;
        if (outUsable) *outUsable = g_allowedSkins[index].usable;
        return true;
    }

    // Sostituisce la whitelist con quella inviata dal server (0x83C, all'enter-world).
    // Contiene TUTTE le skin; triple = [type, typeId, usable] * count. `usable` dice se
    // il personaggio locale può equipaggiarla (calcolato server-side). Il server è
    // l'unica autorità: nessun WeaponSkins.ini lato client.
    void set_allowed_skins_from_server(const uint8_t* triples, int count)
    {
        if (!triples || count <= 0)
            return;
        g_allowedSkins.clear();
        for (int i = 0; i < count; ++i)
            g_allowedSkins.push_back({ triples[i * 3], triples[i * 3 + 1],
                                       triples[i * 3 + 2] != 0 });
    }

    // Geometria screen-space del box skin, condivisa tra input (imgui, qui) e
    // render nativo (item_icon.cpp draw_skin_slot_native). false = inventario
    // chiuso o window non ancora catturata.
    bool skin_slot_geometry(float* outBx, float* outBy, float* outSz)
    {
        if (!g_invWindowPtr)
            return false;
        auto* w = reinterpret_cast<const uint8_t*>(g_invWindowPtr);
        if (*reinterpret_cast<const int*>(w + 0x20) == 0)
            return false; // inventario chiuso

        int   x0 = *reinterpret_cast<const int*>(w + 0x04);
        int   y0 = *reinterpret_cast<const int*>(w + 0x08);
        float wx = *reinterpret_cast<const float*>(w + 0x7C4); // slot weapon X (window-rel)
        float wy = *reinterpret_cast<const float*>(w + 0x7C8); // slot weapon Y (window-rel)

        if (outSz) *outSz = kSkinSlotSize;
        if (outBx) *outBx = static_cast<float>(x0) + wx + static_cast<float>(kSkinSlotOffX);
        if (outBy) *outBy = static_cast<float>(y0) + wy + static_cast<float>(kSkinSlotOffY);
        return true;
    }

    // Contesto UI dell'inventario per il renderer icona nativo (0x4B7240): è window+0x30
    // (stesso `LEA ECX,[ESI+0x30]` usato dal gioco per disegnare le celle in FUN_00518110).
    // Ricavato dal solo puntatore-finestra → NON dipende dalle celle item, così il box
    // skin si disegna anche in un bag vuoto.
    void* inventory_ui_ctx()
    {
        if (!g_invWindowPtr)
            return nullptr;
        return reinterpret_cast<void*>(g_invWindowPtr + 0x30);
    }

    // Box agganciato all'inventario (inven.png) — a sinistra dello slot weapon.
    // Chiamato ogni frame dall'imgui layer. Gestisce SOLO l'INPUT (drag-to-set,
    // right-click clear, hint). Il DISEGNO del box (sfondo/icona/bordo) è nel
    // render NATIVO dell'inventario (item_icon.cpp) per rispettare lo z-order:
    // altrimenti l'imgui foreground disegna sempre SOPRA ogni finestra nativa.
    void draw_skin_slot()
    {
        float bx, by, sz;
        if (!skin_slot_geometry(&bx, &by, &sz))
        {
            g_wasHolding = false;
            return;
        }

        auto* w = reinterpret_cast<const uint8_t*>(g_invWindowPtr);

        // --- Drag-to-set: rileva il drop di un item sull'area del box ---------
        // Cache dello slot/bag tenuto in mano finché held != 0xff. Quando il
        // codice nativo resetta a 0xff (drop processato) e il mouse è sul box,
        // leggo l'item dalla cache (drop fuori da slot validi = no-op nativo,
        // l'item resta in inventario) e applico la skin. Niente hook nativo.
        uint8_t heldSlot = *reinterpret_cast<const uint8_t*>(w + kWinHeldSlot);
        uint8_t heldBag = *reinterpret_cast<const uint8_t*>(w + kWinHeldBag);
        bool holding = (heldSlot != 0xFF);

        if (g_wasHolding && !holding && g_lastHeldSlot != 0xFF)
        {
            ImVec2 m = ImGui::GetMousePos();
            if (m.x >= bx && m.x <= bx + sz && m.y >= by && m.y <= by + sz)
            {
                unsigned idx = (g_lastHeldSlot + g_lastHeldBag * kInvBagStride) * kInvCellStride;
                uint8_t t = *reinterpret_cast<const uint8_t*>(kInvItemType + idx);
                uint8_t tid = *reinterpret_cast<const uint8_t*>(kInvItemTypeId + idx);
                // Solo item skin (in whitelist). Il server valida classe/fazione e, se
                // rifiuta, risponde 0x83D → "Cannot equip".
                if (t != 0 && weapon_skin::is_allowed_skin(t, tid))
                    weapon_skin::request_set(t, tid);
            }
        }

        if (holding)
        {
            g_lastHeldSlot = heldSlot;
            g_lastHeldBag = heldBag;
        }
        g_wasHolding = holding;

        // --- Mouse sul box: hover + tasto destro per rimuovere la skin --------
        ImVec2 mouse = ImGui::GetMousePos();
        bool overBox = mouse.x >= bx && mouse.x <= bx + sz && mouse.y >= by && mouse.y <= by + sz;
        if (overBox && is_set() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            weapon_skin::request_clear();

        // NOTA: sfondo box + icona skin + bordo sono disegnati nel render NATIVO
        // dell'inventario (item_icon.cpp draw_skin_slot_native) → z-order corretto.
        // Qui resta solo l'hint testuale su hover (tooltip transitorio).
        if (overBox)
        {
            auto* dl = ImGui::GetForegroundDrawList();
            const char* hint = is_set() ? "Right-click: remove skin"
                : "Drag a skin weapon here";
            dl->AddText(ImVec2(bx, by + sz + 2.0f), IM_COL32(220, 220, 120, 255), hint);
        }
    }
}

void hook::weapon_skin()
{
    // La whitelist skin arriva SOLO dal server (0x83C, enter-world), già filtrata per
    // classe/fazione. Nessun WeaponSkins.ini lato client.
    build_trampoline();
    if (g_origBuildWeaponModel)
        util::detour((void*)kFunBuildWeaponModel, hook_build_weapon_model, 7);
    // hook equip-visual (cambio arma senza flicker + sync altri)
    build_equip_trampoline();
    if (g_origEquipVisual)
        util::detour((void*)kFunEquipVisual, hook_equip_visual, 7);
    // cattura puntatore finestra inventario (per agganciare il box display)
    util::detour((void*)0x5195F0, capture_inv_window, 7);
    // tasto destro su skin whitelist → applica skin invece di equipaggiare
    util::detour((void*)0x4E88A0, hook_equip_validate, 5);
    // sopprime il msg nativo "non puoi equipaggiare" subito dopo un divert
    util::detour((void*)0x423150, hook_sysmsg, 5);
}