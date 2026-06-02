#include "include/rank_icon.h"
#include "include/imgui_layer_internal.h"

// rank_icon.cpp — owner del sistema rank icon esteso.
// Contiene: stato, logica di mapping, auto-init texture.
//
// Requisito: grade2_extended.png (64x1024) in:
//   <game_dir>/assets/ui/grade2_extended.png
//
// Visibilita': FUN_0044ebe0 e' chiamata per ogni personaggio visibile a schermo
// → le icone estese appaiono su tutti i giocatori nel client locale.

namespace rank_icon
{
    // -----------------------------------------------------------------------
    //  Definizione variabili pubbliche
    // -----------------------------------------------------------------------

    int32_t g_enabled    = 0;
    int32_t g_last_kills = 0;
    int32_t g_u_col      = 0;

    std::array<int32_t, 62> g_thresholds = {
        // Logical 0-15: top SX (kills 0-200000, soglie native rank 0-15)
        0, 50, 300, 1000, 5000, 10000, 20000, 30000,
        40000, 50000, 70000, 90000, 110000, 130000, 150000, 200000,
        // Logical 16-30: top DX (kills 250000-1000000, soglie native rank 16-30)
        250000, 300000, 350000, 400000, 450000, 500000,
        550000, 600000, 650000, 700000, 750000, 800000, 850000, 900000, 1000000,
        // Logical 31-46: bot SX (kills >1M, server esteso)
        1100000, 1200000, 1400000, 1600000, 1800000, 2000000,
        2200000, 2400000, 2600000, 2800000, 3000000, 3200000, 3400000, 3600000, 3800000, 4000000,
        // Logical 47-61: bot DX (kills >4M)
        4500000, 5000000, 5500000, 6000000, 6500000, 7000000,
        7500000, 8000000, 8500000, 9000000, 9500000, 10000000, 11000000, 12000000, 13000000
    };

    // -----------------------------------------------------------------------
    //  Stato privato
    // -----------------------------------------------------------------------

    static LPDIRECT3DTEXTURE9 s_tex        = nullptr;
    static LPDIRECT3DTEXTURE9 s_orig_tex   = nullptr;
    static bool               s_file_tried = false; // tentato lettura PNG (one-shot)
    static bool               s_applied    = false; // texture sostituita in pvpGradeImage

    static constexpr const char* kTexturePath = "assets/ui/grade2_extended.png";

    // -----------------------------------------------------------------------
    //  Implementazione
    // -----------------------------------------------------------------------

    int32_t compute_icon(int32_t native_bl)
    {
        if (!g_enabled)
        {
            g_u_col = (native_bl > 15) ? 1 : 0;
            return native_bl;
        }

        int logical = 0;
        for (int i = 61; i >= 0; i--)
            if (g_last_kills >= g_thresholds[i]) { logical = i; break; }

        int uvRow, uCol;
        if      (logical < 16) { uvRow = logical;       uCol = 0; } // top SX
        else if (logical < 31) { uvRow = logical - 16;  uCol = 1; } // top DX
        else if (logical < 47) { uvRow = logical - 15;  uCol = 0; } // bot SX
        else                   { uvRow = logical - 31;  uCol = 1; } // bot DX

        g_u_col = uCol;
        return uvRow;
    }

    void tick()
    {
        // Fase 1: carica PNG → GPU texture non appena device disponibile.
        // Avviene durante loading screen — nessun requisito scena.
        if (!s_tex && !s_file_tried && imgui_layer::g_device)
        {
            s_file_tried = true;
            std::vector<char> buf;
            if (imgui_layer::read_asset_image_bytes(kTexturePath, buf, nullptr))
                s_tex = imgui_layer::create_texture_from_image_memory(
                    imgui_layer::g_device, buf.data(), (UINT)buf.size());
        }

        if (s_applied) return;
        if (!s_tex) return;

        // Fase 2: applica appena il gioco ha inizializzato pvpGradeImage.
        // Il gioco carica grade2.tga durante il loading screen (state=5) →
        // sostituiamo prima che il player veda la mappa → nessun glitch.
        if (!g_pWorldMgr || !g_pWorldMgr->pvpGradeImage.texture) return;

        if (!s_orig_tex)
            s_orig_tex = g_pWorldMgr->pvpGradeImage.texture;
        g_pWorldMgr->pvpGradeImage.texture = s_tex;
        g_enabled = 1;
        s_applied = true;
    }

    void disable()
    {
        g_enabled = 0;
        if (g_pWorldMgr && s_orig_tex)
            g_pWorldMgr->pvpGradeImage.texture = s_orig_tex;
    }

    void force_reload()
    {
        disable();
        if (s_tex) { s_tex->Release(); s_tex = nullptr; }
        s_orig_tex   = nullptr;
        s_file_tried = false;
        s_applied    = false;
    }

    void on_device_reset()
    {
        if (s_tex) { s_tex->Release(); s_tex = nullptr; }
        s_orig_tex   = nullptr;
        s_file_tried = false;
        s_applied    = false;
        // g_enabled non azzerato: tick() ricarica al frame successivo
    }
}
