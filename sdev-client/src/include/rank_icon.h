#pragma once
#include <array>
#include <cstdint>

// rank_icon.h — interfaccia pubblica del sistema rank icon esteso.
// Questo modulo e' il proprietario esclusivo dello stato rank icon.
// imgui_layer_character_visual_debug.cpp legge da qui per debug/tuning.
// ui_image.cpp hook leggono/scrivono qui per il rendering.

namespace rank_icon
{
    // -----------------------------------------------------------------------
    //  Stato pubblico — accessibile da ui_image.cpp e dal pannello debug
    // -----------------------------------------------------------------------

    // 1 = extended rank attivo, 0 = sistema nativo.
    // int32_t invece di bool per compatibilita' con inline ASM (cmp dword ptr).
    extern int32_t g_enabled;

    // killLv del personaggio in rendering (aggiornato da naked_0x44EBED ogni frame).
    extern int32_t g_last_kills;

    // Colonna UV selezionata da compute_icon: 0=sinistra (U=0.0), 1=destra (U=0.5).
    extern int32_t g_u_col;

    // Tabella 62 soglie kills per logical icon 0-61.
    // Layout texture 64x1024 (2 col x 16 righe, icone 32px, UV 1/32):
    //   logical  0-15 → top SX  (row 0-15,  U=0.0)   16 icone
    //   logical 16-30 → top DX  (row 0-14,  U=0.5)   15 icone
    //   logical 31-46 → bot SX  (row 16-31, U=0.0)   16 icone
    //   logical 47-61 → bot DX  (row 16-30, U=0.5)   15 icone
    extern std::array<int32_t, 62> g_thresholds;

    // -----------------------------------------------------------------------
    //  Funzioni pubbliche
    // -----------------------------------------------------------------------

    // Calcola uvRow e setta g_u_col in base a g_last_kills.
    // Chiamata da naked_0x44EC4D in ui_image.cpp.
    // Se g_enabled=0: ritorna nativeBl invariato (comportamento nativo).
    int32_t compute_icon(int32_t native_bl);

    // Auto-init: carica assets/ui/grade2_extended.png e abilita al primo frame stabile.
    // Idempotente dopo il primo successo. Chiamata ogni frame da ui_image.cpp.
    void tick();

    // Disabilita sistema esteso e ripristina texture originale.
    void disable();

    // Forza ricaricamento da disco (per debug). Ripristina orig + rilascia texture + tick() ricarica.
    void force_reload();

    // Device reset D3D9: rilascia la texture (tick() ricarica al frame successivo).
    void on_device_reset();
}
