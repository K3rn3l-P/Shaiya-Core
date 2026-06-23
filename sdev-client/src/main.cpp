#include "include/main.h"

void Main()
{
    hook::compute_game_hash();   // PRIMA DI TUTTO: SHA256(.text) pulita, zero detour installati
    hook::sah_decrypt_init();
    hook::anti_tamper();
    hook::command();
    hook::imgui_layer();
    hook::item_icon();
    hook::map_editor();
    hook::name_color();
    hook::packet();
    hook::patch();
    hook::quick_slot();
    hook::raid();
    hook::select_screen();
    hook::title();
    hook::vehicle();
    hook::window();
    init_discord_rpc();
}
