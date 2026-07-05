#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace shaiya
{
    struct Unknown;
}

void Main();
extern "C" __declspec(dllexport) void DllExport();
void init_discord_rpc();
inline constexpr unsigned kClientRouletteListWindowMessage = 0x8000 + 0x4A4;
inline constexpr unsigned kClientRouletteRollWindowMessage = 0x8000 + 0x4A3;
inline constexpr unsigned kClientEmojiTokenWindowMessage = 0x8000 + 0x4A5;
inline constexpr unsigned kClientGMCommandWindowMessage = 0x8000 + 0x4A6;
inline constexpr unsigned kClientTeleportListWindowMessage = 0x8000 + 0x4A7;
inline constexpr unsigned kClientTeleportMoveWindowMessage = 0x8000 + 0x4A8;
inline constexpr unsigned kClientTamperReportMsg = 0x8000 + 0x4A9;
inline constexpr unsigned kClientWeaponSkinMsg = 0x8000 + 0x4AA;
void queue_client_sysmsg(int chatType, int messageNumber);
void flush_client_sysmsg_queue();
void tick_client_welcome_sysmsg();
void ensure_client_sysmsg_dispatch_ready();
bool is_client_sysmsg_dispatch_ready();
void open_remote_npc_shortcut(int index);
void sync_textbox_utf8_display(void* textBox);
bool append_utf8_textbox_wchar(void* textBox, wchar_t wideChar);
bool append_utf8_textbox_text(void* textBox, const char* utf8);
bool set_utf8_textbox_text(void* textBox, const char* utf8);
bool backspace_utf8_textbox_char(void* textBox);
bool handle_imgui_layer_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);
bool play_custom_eft_from_ui(const char* fileName, bool loop, unsigned loopIntervalMs);
void stop_custom_eft_from_ui();
bool is_custom_eft_loop_enabled_from_ui();
unsigned get_custom_eft_loop_interval_from_ui();
const char* get_custom_eft_name_from_ui();

// Reset del guard "box skin gia disegnato" — chiamare 1 volta per frame dall'imgui layer,
// PRIMA del render nativo del frame successivo. Il box skin viene disegnato nel render
// nativo dell'inventario (item_icon.cpp) per rispettare lo z-order delle finestre native.
void skin_box_frame_reset();

namespace hook
{
    void compute_game_hash();   // SHA256(.text) PRIMA dei detour — chiamare per primo in Main()
    void auto_login();          // legge+azzera SDEV_LOGIN_USER/PW (auto-login in-process)
    void sah_decrypt_init();
    void sah_decrypt_teardown();
    void anti_tamper();
    void command();
    void imgui_layer();
    void item_icon();
    void itemmall();            // item-mall preview: mount + weapon skin (client-only)
    void map_editor();
    void name_color();
    void packet();
    void patch();
    void quick_slot();
    void raid();
    void recreation_viewer();   // allarga banda Y hover slot recreation (0x4C42BF, +0x20 -> +0x88)
    void select_screen();
    void title();
    void weapon_skin();         // render locale weapon skin (override modello arma)
    void unicode();
    void vehicle();
    void window();
}

namespace hwid
{
    // Restituisce l'HWID stabile dall'Updater (%LOCALAPPDATA%\Shaiya_Duff_Updater\device.id).
    // Stringa 32-char hex; vuota se file assente o sig assente.
    const char* get();
    // Restituisce la firma grezza da device.id.sig (64 hex chars), o "" se assente.
    // La firma NON è verificata client-side — verifica avviene solo in sdev-login.
    const char* get_sig();
    // true se device.id.sig era presente (sig inviata al server per verifica HMAC)
    bool is_signed();
    // true se device.id.sig era assente (HWID vuoto, HWID_MISSING lato server)
    bool is_tampered();
}

namespace anti_tamper
{
    // Bitmask accumulativo dei check anti-tamper
    //   bit0=DLL_INJECT  bit1=DEBUGGER  bit2=MEM_PATCH  bit3=CHEAT_PROC
    uint8_t get_tamper_flags();
    // Check sincrono eseguito al momento del login — cattura debugger attaccato dopo t=0.
    void check_now();
}

namespace window
{
    // HWND della finestra di gioco subclassata — valido dopo il primo frame.
    HWND get_game_hwnd();
}

namespace sec_token
{
    // Token di sessione sicurezza (64 hex chars) ricevuto dall'updater via SDEV_SEC_TOKEN.
    // Letto e cancellato dall'env a DLL_PROCESS_ATTACH. Usato da anti_tamper per HTTP report.
    // Vuoto se il gioco è stato lanciato senza updater (bypass).
    const char* get();
}

namespace auto_login
{
    // Auto-login in-process: tick() ogni frame (poll login screen + inietta creds +
    // applica patch), on_login_sent() ripristina i patch dopo il send 0xA101.
    void init();
    void tick();
    void on_login_sent();
}

// Chiamata dall'hook ASM del send 0xA101 (packet.cpp) per ripristinare i patch.
extern "C" void auto_login_after_send();

namespace weapon_skin
{
    // Store client-side della skin attiva (ricevuta dal server via 0x0A03).
    bool is_set();
    uint8_t skin_type();
    uint8_t skin_type_id();
    void set_local(uint8_t type, uint8_t typeId);
    // Applica il modello arma locale: type!=0 imposta la skin (persistente in type[5]),
    // type==0 ripristina l'arma reale memorizzata. NON invia nulla al server.
    void apply_local_visual(uint8_t type, uint8_t typeId);
    // Invia la richiesta skin al server (server-authoritative). Il server valida; se
    // rifiuta risponde 0x83D → il client mostra "Cannot equip".
    void request_set(uint8_t type, uint8_t typeId);
    void request_clear();
    void draw_skin_slot();   // INPUT del box skin (drag-to-set, right-click clear, hint) — imgui, ogni frame
    // Geometria screen-space del box skin (bx,by,sz). false = inventario chiuso/non pronto.
    // Condivisa tra input imgui (draw_skin_slot) e render nativo (draw_skin_slot_native).
    bool skin_slot_geometry(float* outBx, float* outBy, float* outSz);
    // Contesto UI dell'inventario (window+0x30) per il renderer icona nativo (0x4B7240).
    // nullptr se la finestra non è ancora stata catturata. Indipendente dalle celle item.
    void* inventory_ui_ctx();

    // Whitelist skin dal server (0x83C, enter-world). Contiene TUTTE le skin; `usable`
    // dice se il personaggio locale può equipaggiarla. Nessun WeaponSkins.ini client.
    void set_allowed_skins_from_server(const uint8_t* triples, int count);
    bool is_allowed_skin(uint8_t type, uint8_t typeId);
    bool is_usable_skin(uint8_t type, uint8_t typeId);
    int  allowed_skin_count();
    bool allowed_skin_get(int index, uint8_t* outType, uint8_t* outTypeId, bool* outUsable);
}

namespace name_color
{
    void reset_helmet_eft_state();
    void disable_enchant_eft();
    void remove_char_enchant_eft_tracking(void* user);
}

inline int g_showCostumes = false;
inline int g_showPets = false;
inline int g_showWings = false;
inline int g_showEffects = false;
inline int g_showMobEffects = false;
inline int g_fpsBoost = false;
inline int g_showTitles = true;
inline int g_showNameColors = true;
inline int g_showPlayerHpBar = true;
inline float g_cameraLimit = 30.0f;
inline shaiya::Unknown* g_uiRoot1 = nullptr;
inline shaiya::Unknown* g_uiRoot2 = nullptr;

int dispatch_native_gm_command(void* chatWindow, char* text);
bool is_native_gm_command_catalogued(const char* text);
