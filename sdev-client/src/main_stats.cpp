#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <util/util.h>
#include <shaiya/include/network/game/incoming/0200.h>
#include "include/main.h"
#include "include/config.h"
#include "include/shaiya/CButton.h"
#include "include/shaiya/CNetwork.h"
#include "include/shaiya/Static.h"
#include "include/shaiya/CWindow.h"
using namespace shaiya;

namespace
{
    constexpr int kButtonX = 38;
    constexpr int kButtonY = 78;
    constexpr int kCustomUiButtonYOffset = -22;
    constexpr int kButtonWidth = 165;
    constexpr int kButtonHeight = 47;

    bool g_buttonInitialized = false;
    bool g_mouseWasDown = false;
    bool g_buttonPressStartedInside = false;
    DWORD g_lastBattlegroundConfigWarningTick = 0;
    CButton g_battlefieldButton{};
    D2D_POINT_2U g_anchor{};

    int get_button_y_offset()
    {
        return config::load_custom_ui() == 1 ? kCustomUiButtonYOffset : 0;
    }

    void init_battlefield_button()
    {
        if (g_buttonInitialized)
            return;

        CButton::Init(&g_battlefieldButton);
        CButton::Create(
            &g_battlefieldButton, 0, 0,
            kButtonX, kButtonY, 256, 64, 165, 47, 0,
            "main_stats_pvp_button.png", 256, 256,
            1,
            0.0F, 1.0F, 0.0F, 0.25F,
            0.0F, 1.0F, 0.25F, 0.5F,
            0.0F, 1.0F, 0.5F, 0.75F,
            0.0F, 1.0F, 0.5F, 0.75F,
            0.0F, 1.0F, 0.75F, 1.0F,
            0);
        g_buttonInitialized = true;
    }

    bool is_cursor_over_button()
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
            return false;

        auto hwnd = g_var->hwnd;
        if (!hwnd || GetForegroundWindow() != hwnd || !ScreenToClient(hwnd, &cursor))
            return false;

        auto x = static_cast<int>(g_anchor.x) + kButtonX;
        auto y = static_cast<int>(g_anchor.y) + kButtonY + get_button_y_offset();
        return cursor.x >= x && cursor.x <= x + kButtonWidth &&
            cursor.y >= y && cursor.y <= y + kButtonHeight;
    }

    void show_battleground_panel_unavailable_warning()
    {
        auto now = GetTickCount();
        if (g_lastBattlegroundConfigWarningTick != 0 && now - g_lastBattlegroundConfigWarningTick < 3000)
            return;

        g_lastBattlegroundConfigWarningTick = now;
        MessageBoxA(
            g_var->hwnd,
            "Battleground panel could not be opened.\n\n"
            "BattleFieldMoveInfo_Client.ini was not found or contains no valid entries.\n"
            "The file must exist beside Game.exe or inside the client's Data folder.\n\n"
            "Also make sure the server/client battleground INI files use real map IDs and coordinates.",
            "Battleground",
            MB_OK | MB_ICONWARNING);
    }

    void handle_battlefield_button_click()
    {
        if (is_battleground_panel_available())
        {
            toggle_battleground_panel();
            return;
        }

        show_battleground_panel_unavailable_warning();
    }

    void draw_battlefield_button(CWindow* window)
    {
        tick_client_welcome_sysmsg();
        init_battlefield_button();

        g_anchor = window->pos;

        auto mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        auto cursorOverButton = is_cursor_over_button();

        // Update hover state so CButton::Draw renders the hover frame.
        g_battlefieldButton.mouseEnter = cursorOverButton;

        CButton::Draw(&g_battlefieldButton, g_anchor.x, g_anchor.y + get_button_y_offset());

        if (mouseDown && !g_mouseWasDown)
            g_buttonPressStartedInside = cursorOverButton;

        if (!mouseDown && g_mouseWasDown && g_buttonPressStartedInside && cursorOverButton)
            handle_battlefield_button_click();

        if (!mouseDown)
            g_buttonPressStartedInside = false;

        g_mouseWasDown = mouseDown;
    }

    unsigned u0x532A2A = 0x532A2A;
    void __declspec(naked) naked_0x532A23()
    {
        __asm
        {
            pushad
            push esi
            call draw_battlefield_button
            add esp, 0x4
            popad

            mov ecx, [esp + 0x108]
            jmp u0x532A2A
        }
    }

    unsigned u0x5320F3 = 0x5320F3;
    void __declspec(naked) naked_0x5320E6()
    {
        __asm
        {
            pushad
            call close_battleground_panel
            popad

            mov eax, [esi + 0x40]
            xor edi, edi
            jmp u0x5320F3
        }
    }
}

void hook::main_stats()
{
    // Battleground buttons.
    // Draws an independent battlefield button over the existing main stats UI.
    // This intentionally does not expand or rewrite CStatusMiniBar layout, so it
    // stays compatible with the EP4 UI support already used by this client.
    util::detour((void*)0x532A23, naked_0x532A23, 7);
    util::detour((void*)0x5320E6, naked_0x5320E6, 5);
}
