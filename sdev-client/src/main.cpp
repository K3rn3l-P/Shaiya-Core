#include "include/main.h"

void Main()
{
    hook::compute_game_hash();   // PRIMA DI TUTTO: SHA256(.text) pulita, zero detour installati
    hook::auto_login();          // legge+azzera SDEV_LOGIN_USER/PW subito dall'env
    hook::sah_decrypt_init();
    hook::anti_tamper();
    hook::command();
    hook::imgui_layer();
    hook::item_icon();
    hook::itemmall();
    hook::weapon_skin();
    hook::map_editor();
    hook::name_color();
    hook::packet();
    hook::patch();
    hook::quick_slot();
    hook::raid();
    hook::recreation_viewer();
    hook::select_screen();
    hook::title();
    hook::vehicle();
    hook::window();
    init_discord_rpc();
}
