#include "include/imgui_layer_internal.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <shaiya/include/network/game/incoming/0200.h>
#include "include/shaiya/CNetwork.h"

void open_remote_npc_shortcut(int index);

namespace imgui_layer {

    const char* get_imgui_ini_path()
    {
        return config::ini_path().c_str();
    }

    const char* get_game_ini_path()
    {
        return g_var->iniFileName.data();
    }

    bool has_ini_path(const char* path)
    {
        return path && path[0] != '\0';
    }

    bool same_ini_path(const char* lhs, const char* rhs)
    {
        return has_ini_path(lhs) && has_ini_path(rhs) && _stricmp(lhs, rhs) == 0;
    }

    bool read_profile_string_any(const char* section, const char* key, char* buffer, DWORD bufferSize)
    {
        if (!buffer || bufferSize == 0)
            return false;

        buffer[0] = '\0';
        auto primaryPath = get_imgui_ini_path();
        if (has_ini_path(primaryPath))
        {
            GetPrivateProfileStringA(section, key, "", buffer, bufferSize, primaryPath);
            if (buffer[0] != '\0')
                return true;
        }

        auto gamePath = get_game_ini_path();
        if (has_ini_path(gamePath) && !same_ini_path(primaryPath, gamePath))
        {
            GetPrivateProfileStringA(section, key, "", buffer, bufferSize, gamePath);
            if (buffer[0] != '\0')
                return true;
        }

        return false;
    }

    void write_profile_string_all(const char* section, const char* key, const char* value)
    {
        auto primaryPath = get_imgui_ini_path();
        if (has_ini_path(primaryPath))
        {
            WritePrivateProfileStringA(section, key, value, primaryPath);
            WritePrivateProfileStringA(nullptr, nullptr, nullptr, primaryPath);
        }

        auto gamePath = get_game_ini_path();
        if (has_ini_path(gamePath) && !same_ini_path(primaryPath, gamePath))
        {
            WritePrivateProfileStringA(section, key, value, gamePath);
            WritePrivateProfileStringA(nullptr, nullptr, nullptr, gamePath);
        }
    }

    float read_imgui_float(const char* key, float fallback)
    {
        char buffer[64]{};
        if (!read_profile_string_any(kImguiIniSection, key, buffer, sizeof(buffer)))
            return fallback;

        return static_cast<float>(std::atof(buffer));
    }

    void write_imgui_float(const char* key, float value)
    {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.1f", value);
        write_profile_string_all(kImguiIniSection, key, buffer);
    }

    bool read_imgui_bool(const char* key, bool fallback)
    {
        char buffer[16]{};
        if (!read_profile_string_any(kImguiIniSection, key, buffer, sizeof(buffer)))
            strcpy_s(buffer, fallback ? "TRUE" : "FALSE");

        return _stricmp(buffer, "TRUE") == 0
            || std::strcmp(buffer, "1") == 0
            || _stricmp(buffer, "ON") == 0;
    }

    void write_imgui_bool(const char* key, bool value)
    {
        write_profile_string_all(kImguiIniSection, key, value ? "TRUE" : "FALSE");
    }

    void load_imgui_settings()
    {
        if (g_imguiSettingsLoaded)
            return;

        g_panelPosition.x = read_imgui_float(kPanelPosXKey, g_panelPosition.x);
        g_panelPosition.y = read_imgui_float(kPanelPosYKey, g_panelPosition.y);
        g_panelScale = std::clamp(read_imgui_float(kPanelScaleKey, g_panelScale), 0.55f, 1.15f);
        g_emojiButtonPosition.x = read_imgui_float(kEmojiBtnXKey, g_emojiButtonPosition.x);
        g_emojiButtonPosition.y = read_imgui_float(kEmojiBtnYKey, g_emojiButtonPosition.y);
        g_rewardButtonPosition.x = read_imgui_float(kRewardBtnXKey, g_rewardButtonPosition.x);
        g_rewardButtonPosition.y = read_imgui_float(kRewardBtnYKey, g_rewardButtonPosition.y);
        g_rewardBarPosition.x = read_imgui_float(kRewardBarXKey, -1.0f);
        g_rewardBarPosition.y = read_imgui_float(kRewardBarYKey, -1.0f);
        g_rouletteButtonPosition.x = read_imgui_float(kRouletteBtnXKey, g_rouletteButtonPosition.x);
        g_rouletteButtonPosition.y = read_imgui_float(kRouletteBtnYKey, g_rouletteButtonPosition.y);
        g_settingsButtonPosition.x = read_imgui_float(kSettingsBtnXKey, g_settingsButtonPosition.x);
        g_settingsButtonPosition.y = read_imgui_float(kSettingsBtnYKey, g_settingsButtonPosition.y);
        g_settingsPanelPosition.x = read_imgui_float(kSettingsPanelXKey, -1.0f);
        g_settingsPanelPosition.y = read_imgui_float(kSettingsPanelYKey, -1.0f);
        g_npcButtonPosition.x = read_imgui_float(kNpcBtnXKey, g_npcButtonPosition.x);
        g_npcButtonPosition.y = read_imgui_float(kNpcBtnYKey, g_npcButtonPosition.y);
        g_npcPanelPosition.x = read_imgui_float(kNpcPanelXKey, -1.0f);
        g_npcPanelPosition.y = read_imgui_float(kNpcPanelYKey, -1.0f);
        g_battlegroundPanelPosition.x = read_imgui_float(kBattlegroundPanelXKey, -1.0f);
        g_battlegroundPanelPosition.y = read_imgui_float(kBattlegroundPanelYKey, -1.0f);
        g_rewardAutoClaimEnabled = read_imgui_bool(kRewardAutoClaimKey, false);
        g_ingameChatActive = read_imgui_bool(kIngameChatActiveKey, false);
        g_imguiSettingsLoaded = true;
    }

    void save_imgui_settings()
    {
        write_imgui_float(kPanelPosXKey, g_panelPosition.x);
        write_imgui_float(kPanelPosYKey, g_panelPosition.y);
        write_imgui_float(kPanelScaleKey, g_panelScale);
        write_imgui_float(kEmojiBtnXKey, g_emojiButtonPosition.x);
        write_imgui_float(kEmojiBtnYKey, g_emojiButtonPosition.y);
        write_imgui_float(kRewardBtnXKey, g_rewardButtonPosition.x);
        write_imgui_float(kRewardBtnYKey, g_rewardButtonPosition.y);
        write_imgui_float(kRewardBarXKey, g_rewardBarPosition.x);
        write_imgui_float(kRewardBarYKey, g_rewardBarPosition.y);
        write_imgui_float(kRouletteBtnXKey, g_rouletteButtonPosition.x);
        write_imgui_float(kRouletteBtnYKey, g_rouletteButtonPosition.y);
        write_imgui_float(kSettingsBtnXKey, g_settingsButtonPosition.x);
        write_imgui_float(kSettingsBtnYKey, g_settingsButtonPosition.y);
        write_imgui_float(kSettingsPanelXKey, g_settingsPanelPosition.x);
        write_imgui_float(kSettingsPanelYKey, g_settingsPanelPosition.y);
        write_imgui_float(kNpcBtnXKey, g_npcButtonPosition.x);
        write_imgui_float(kNpcBtnYKey, g_npcButtonPosition.y);
        write_imgui_float(kNpcPanelXKey, g_npcPanelPosition.x);
        write_imgui_float(kNpcPanelYKey, g_npcPanelPosition.y);
        write_imgui_float(kBattlegroundPanelXKey, g_battlegroundPanelPosition.x);
        write_imgui_float(kBattlegroundPanelYKey, g_battlegroundPanelPosition.y);
        write_imgui_bool(kRewardAutoClaimKey, g_rewardAutoClaimEnabled);
        write_imgui_bool(kIngameChatActiveKey, g_ingameChatActive);
        g_imguiSettingsDirty = false;
        g_lastPanelSaveTick = GetTickCount();
    }

    void mark_imgui_settings_dirty()
    {
        g_imguiSettingsDirty = true;
        g_imguiSettingsDirtyTick = GetTickCount();
    }

    void save_imgui_settings_if_dirty(DWORD debounceMs)
    {
        if (!g_imguiSettingsDirty)
            return;

        auto now = GetTickCount();
        if (debounceMs != 0 && now - g_imguiSettingsDirtyTick < debounceMs)
            return;

        if (debounceMs != 0 && g_lastPanelSaveTick != 0 && now - g_lastPanelSaveTick < debounceMs)
            return;

        save_imgui_settings();
    }

    void run_settings_command(const char* text)
    {
        if (!text || !text[0])
            return;

        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%s", text);
        command_handler(buffer);
    }

    static bool draw_settings_toggle_row(const char* label, bool enabled, const char* onCommand, const char* offCommand)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(118.0f);

        auto color = enabled
            ? ImVec4(0.30f, 0.64f, 0.32f, 1.0f)
            : ImVec4(0.58f, 0.22f, 0.22f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            enabled ? ImVec4(0.38f, 0.74f, 0.40f, 1.0f) : ImVec4(0.70f, 0.28f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            enabled ? ImVec4(0.24f, 0.52f, 0.26f, 1.0f) : ImVec4(0.46f, 0.16f, 0.16f, 1.0f));

        char buttonLabel[64]{};
        std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##settings_%s", enabled ? "ON" : "OFF", label);
        bool clicked = ImGui::Button(buttonLabel, ImVec2(56.0f, 22.0f));
        ImGui::PopStyleColor(3);

        if (clicked)
        {
            run_settings_command(enabled ? offCommand : onCommand);
            release_imgui_capture();
        }

        return clicked;
    }

    void draw_settings_panel()
    {
        if (!g_showSettingsPanel)
            return;

        auto& io = ImGui::GetIO();
        if (g_settingsPanelPosition.x < 0.0f || g_settingsPanelPosition.y < 0.0f)
        {
            auto panelPos = ImVec2(g_settingsButtonPosition.x + kSettingsButtonSize.x + 4.0f,
                g_settingsButtonPosition.y - 90.0f);
            g_settingsPanelPosition = clamp_window_position(panelPos, kSettingsPanelSize, io.DisplaySize);
            mark_imgui_settings_dirty();
        }
        else
        {
            g_settingsPanelPosition = clamp_window_position(g_settingsPanelPosition, kSettingsPanelSize, io.DisplaySize);
        }

        ImGui::SetNextWindowPos(g_settingsPanelPosition, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(kSettingsPanelSize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.94f);

        bool panelOpen = g_showSettingsPanel;
        if (ImGui::Begin(
            "Quick Settings",
            &panelOpen,
            ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus))
        {
            auto pos = ImGui::GetWindowPos();
            auto size = ImGui::GetWindowSize();
            remember_rect(g_settingsPanelRect, pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (std::fabs(pos.x - g_settingsPanelPosition.x) >= 0.5f
                || std::fabs(pos.y - g_settingsPanelPosition.y) >= 0.5f)
            {
                g_settingsPanelPosition = pos;
                mark_imgui_settings_dirty();
            }

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
            draw_settings_toggle_row("Wings", g_showWings != 0, "/wings on", "/wings off");
            draw_settings_toggle_row("Pets", g_showPets != 0, "/pets on", "/pets off");
            draw_settings_toggle_row("Costumes", g_showCostumes != 0, "/costumes on", "/costumes off");
            draw_settings_toggle_row("Titles", g_showTitles != 0, "/titles on", "/titles off");
            draw_settings_toggle_row("Colours", g_showNameColors != 0, "/colour on", "/colour off");
            draw_settings_toggle_row("FPS Boost", g_fpsBoost != 0, "/fpsboost on", "/fpsboost off");
            draw_settings_toggle_row("Effects", g_showEffects != 0, "/effects on", "/effects off");
            ImGui::PopStyleVar();
        }
        ImGui::End();

        g_showSettingsPanel = panelOpen;
    }

    void draw_settings_button_overlay()
    {
        auto& io = ImGui::GetIO();
        if (!is_overlay_display_usable(io.DisplaySize))
            return;

        auto buttonSize = kSettingsButtonSize;
        g_settingsButtonPosition = clamp_window_position(g_settingsButtonPosition, buttonSize, io.DisplaySize);

        ImGui::SetNextWindowPos(g_settingsButtonPosition, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(
            "##settings_button_overlay",
            nullptr,
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::InvisibleButton("##settings_toggle", buttonSize);

        auto min = ImGui::GetItemRectMin();
        auto max = ImGui::GetItemRectMax();
        remember_rect(g_settingsButtonRect, min, max);

        auto hovered = ImGui::IsItemHovered() || is_cursor_in_rect(g_settingsButtonRect);
        auto mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        auto mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            g_draggingSettingsButton = true;
            g_draggedSettingsButton = false;
            g_settingsButtonDragOffset = ImVec2(
                io.MousePos.x - g_settingsButtonPosition.x,
                io.MousePos.y - g_settingsButtonPosition.y);
        }

        if (g_draggingSettingsButton && mouseDown)
        {
            auto newPos = ImVec2(
                io.MousePos.x - g_settingsButtonDragOffset.x,
                io.MousePos.y - g_settingsButtonDragOffset.y);
            auto clamped = clamp_window_position(newPos, buttonSize, io.DisplaySize);
            if (std::fabs(clamped.x - g_settingsButtonPosition.x) > 1.0f
                || std::fabs(clamped.y - g_settingsButtonPosition.y) > 1.0f)
            {
                g_draggedSettingsButton = true;
                g_settingsButtonPosition = clamped;
            }
        }

        if (mouseReleased && g_draggingSettingsButton)
        {
            if (g_draggedSettingsButton)
                mark_imgui_settings_dirty();
            else if (hovered)
                g_showSettingsPanel = !g_showSettingsPanel;

            g_draggingSettingsButton = false;
            g_draggedSettingsButton = false;
            release_imgui_capture();
        }

        auto drawList = ImGui::GetWindowDrawList();
        auto* texture = load_settings_icon_texture();
        auto tint = IM_COL32(255, 255, 255, hovered ? 255 : 230);
        if (texture)
        {
            drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max,
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        }
        else
        {
            draw_icon_button_fallback(drawList, min, max, tint, IM_COL32(26, 31, 36, 225), "S");
        }

        if (hovered)
            drawList->AddRectFilled(min, max, IM_COL32(255, 255, 255, 28), 4.0f);

        if (hovered)
        {
            ImGui::SetNextWindowPos(ImVec2(min.x - 2.0f, min.y - 24.0f), ImGuiCond_Always);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(g_showSettingsPanel ? "Hide settings" : "Show settings");
            ImGui::EndTooltip();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    static bool draw_npc_panel_button(const char* label, int shortcutIndex)
    {
        if (ImGui::Button(label, ImVec2(-1.0f, 24.0f)))
        {
            open_remote_npc_shortcut(shortcutIndex);
            release_imgui_capture();
            return true;
        }

        return false;
    }

    void draw_npc_panel()
    {
        if (!g_showNpcPanel)
            return;

        auto& io = ImGui::GetIO();
        if (g_npcPanelPosition.x < 0.0f || g_npcPanelPosition.y < 0.0f)
        {
            auto panelPos = ImVec2(g_npcButtonPosition.x + kNpcButtonSize.x + 4.0f,
                g_npcButtonPosition.y - 90.0f);
            g_npcPanelPosition = clamp_window_position(panelPos, kNpcPanelSize, io.DisplaySize);
            mark_imgui_settings_dirty();
        }
        else
        {
            g_npcPanelPosition = clamp_window_position(g_npcPanelPosition, kNpcPanelSize, io.DisplaySize);
        }

        ImGui::SetNextWindowPos(g_npcPanelPosition, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(kNpcPanelSize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.94f);

        bool panelOpen = g_showNpcPanel;
        if (ImGui::Begin(
            "Remote NPC",
            &panelOpen,
            ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus))
        {
            auto pos = ImGui::GetWindowPos();
            auto size = ImGui::GetWindowSize();
            remember_rect(g_npcPanelRect, pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (std::fabs(pos.x - g_npcPanelPosition.x) >= 0.5f
                || std::fabs(pos.y - g_npcPanelPosition.y) >= 0.5f)
            {
                g_npcPanelPosition = pos;
                mark_imgui_settings_dirty();
            }

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 7.0f));
            draw_npc_panel_button("Market", 0);
            draw_npc_panel_button("Blacksmith", 1);
            draw_npc_panel_button("RR Blacksmith", 2);
            draw_npc_panel_button("Vet Manager", 3);
            draw_npc_panel_button("Bank", 4);
            draw_npc_panel_button("Guild Manager", 5);
            ImGui::PopStyleVar();
        }
        ImGui::End();

        g_showNpcPanel = panelOpen;
    }

    void draw_npc_button_overlay()
    {
        auto& io = ImGui::GetIO();
        if (!is_overlay_display_usable(io.DisplaySize))
            return;

        auto buttonSize = kNpcButtonSize;
        g_npcButtonPosition = clamp_window_position(g_npcButtonPosition, buttonSize, io.DisplaySize);

        ImGui::SetNextWindowPos(g_npcButtonPosition, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(
            "##npc_button_overlay",
            nullptr,
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::InvisibleButton("##npc_toggle", buttonSize);

        auto min = ImGui::GetItemRectMin();
        auto max = ImGui::GetItemRectMax();
        remember_rect(g_npcButtonRect, min, max);

        auto hovered = ImGui::IsItemHovered() || is_cursor_in_rect(g_npcButtonRect);
        auto mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        auto mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            g_draggingNpcButton = true;
            g_draggedNpcButton = false;
            g_npcButtonDragOffset = ImVec2(
                io.MousePos.x - g_npcButtonPosition.x,
                io.MousePos.y - g_npcButtonPosition.y);
        }

        if (g_draggingNpcButton && mouseDown)
        {
            auto newPos = ImVec2(
                io.MousePos.x - g_npcButtonDragOffset.x,
                io.MousePos.y - g_npcButtonDragOffset.y);
            auto clamped = clamp_window_position(newPos, buttonSize, io.DisplaySize);
            if (std::fabs(clamped.x - g_npcButtonPosition.x) > 1.0f
                || std::fabs(clamped.y - g_npcButtonPosition.y) > 1.0f)
            {
                g_draggedNpcButton = true;
                g_npcButtonPosition = clamped;
            }
        }

        if (mouseReleased && g_draggingNpcButton)
        {
            if (g_draggedNpcButton)
                mark_imgui_settings_dirty();
            else if (hovered)
                g_showNpcPanel = !g_showNpcPanel;

            g_draggingNpcButton = false;
            g_draggedNpcButton = false;
            release_imgui_capture();
        }

        auto drawList = ImGui::GetWindowDrawList();
        auto* texture = load_npc_icon_texture();
        auto tint = IM_COL32(255, 255, 255, hovered ? 255 : 230);
        if (texture)
        {
            drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max,
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        }
        else
        {
            draw_icon_button_fallback(drawList, min, max, tint, IM_COL32(28, 28, 24, 225), "N");
        }

        if (hovered)
            drawList->AddRectFilled(min, max, IM_COL32(255, 255, 255, 28), 4.0f);

        if (hovered)
        {
            ImGui::SetNextWindowPos(ImVec2(min.x - 2.0f, min.y - 24.0f), ImGuiCond_Always);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(g_showNpcPanel ? "Hide NPCs" : "Show NPCs");
            ImGui::EndTooltip();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    namespace
    {
        struct BattlegroundIniFile
        {
            std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
            bool valid = false;
        };

        std::filesystem::path get_client_root_path()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(count);
            return std::filesystem::path(buffer).remove_filename();
        }

        std::string to_lower_copy(std::string value)
        {
            for (auto& ch : value)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return value;
        }

        std::string trim_copy(std::string value)
        {
            auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };

            while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());

            while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
                value.pop_back();

            return value;
        }

        bool sah_read_u32(const std::vector<char>& data, std::size_t& offset, uint32_t& value)
        {
            if (offset + 4 > data.size())
                return false;

            std::memcpy(&value, data.data() + offset, 4);
            offset += 4;
            return true;
        }

        bool sah_read_u64(const std::vector<char>& data, std::size_t& offset, uint64_t& value)
        {
            if (offset + 8 > data.size())
                return false;

            std::memcpy(&value, data.data() + offset, 8);
            offset += 8;
            return true;
        }

        bool sah_read_string(const std::vector<char>& data, std::size_t& offset, std::string& value)
        {
            uint32_t length = 0;
            if (!sah_read_u32(data, offset, length) || offset + length > data.size())
                return false;

            value.assign(data.data() + offset, data.data() + offset + length);
            while (!value.empty() && value.back() == '\0')
                value.pop_back();

            offset += length;
            return true;
        }

        bool find_file_in_sah_directory(
            const std::vector<char>& data,
            std::size_t& offset,
            const std::string& parentPath,
            const std::string& targetLower,
            uint64_t& fileOffset,
            uint64_t& fileSize)
        {
            std::string name;
            if (!sah_read_string(data, offset, name))
                return false;

            auto path = parentPath;
            if (!name.empty())
                path = path.empty() ? name : path + "\\" + name;

            uint32_t fileCount = 0;
            if (!sah_read_u32(data, offset, fileCount))
                return false;

            for (uint32_t i = 0; i < fileCount; ++i)
            {
                std::string fileName;
                uint64_t currentOffset = 0;
                uint64_t currentSize = 0;
                if (!sah_read_string(data, offset, fileName)
                    || !sah_read_u64(data, offset, currentOffset)
                    || !sah_read_u64(data, offset, currentSize))
                    return false;

                auto fullPath = path.empty() ? fileName : path + "\\" + fileName;
                auto fullPathLower = to_lower_copy(fullPath);
                if (fullPathLower == targetLower
                    || fullPathLower == std::string("data\\") + targetLower
                    || (!path.empty() && to_lower_copy(fileName) == targetLower))
                {
                    fileOffset = currentOffset;
                    fileSize = currentSize;
                    return true;
                }
            }

            uint32_t dirCount = 0;
            if (!sah_read_u32(data, offset, dirCount))
                return false;

            for (uint32_t i = 0; i < dirCount; ++i)
            {
                if (find_file_in_sah_directory(data, offset, path, targetLower, fileOffset, fileSize))
                    return true;
            }

            return false;
        }

        bool read_battleground_client_ini_from_saf(std::string& contents)
        {
            auto root = get_client_root_path();
            auto sahPath = root / "data.sah";
            auto safPath = root / "data.saf";

            std::ifstream sahStream(sahPath, std::ios::binary);
            if (!sahStream)
                return false;

            std::vector<char> sahData(
                (std::istreambuf_iterator<char>(sahStream)),
                std::istreambuf_iterator<char>());
            if (sahData.size() <= 0x34 || std::memcmp(sahData.data(), "SAH", 3) != 0)
                return false;

            constexpr auto kTarget = "battlefieldmoveinfo_client.ini";
            auto offset = std::size_t{ 0x34 };
            uint64_t fileOffset = 0;
            uint64_t fileSize = 0;
            if (!find_file_in_sah_directory(sahData, offset, "", kTarget, fileOffset, fileSize))
                return false;

            if (fileSize == 0 || fileSize > static_cast<uint64_t>(INT_MAX))
                return false;

            std::ifstream safStream(safPath, std::ios::binary);
            if (!safStream)
                return false;

            safStream.seekg(static_cast<std::streamoff>(fileOffset), std::ios::beg);
            if (!safStream)
                return false;

            std::vector<char> fileData(static_cast<std::size_t>(fileSize), 0);
            safStream.read(fileData.data(), static_cast<std::streamsize>(fileData.size()));
            if (safStream.gcount() != static_cast<std::streamsize>(fileData.size()))
                return false;

            contents.assign(fileData.begin(), fileData.end());
            return true;
        }

        BattlegroundIniFile parse_battleground_ini(const std::string& raw)
        {
            BattlegroundIniFile ini{};
            if (raw.empty())
                return ini;

            std::string normalized = raw;
            if (normalized.size() >= 3
                && static_cast<unsigned char>(normalized[0]) == 0xEF
                && static_cast<unsigned char>(normalized[1]) == 0xBB
                && static_cast<unsigned char>(normalized[2]) == 0xBF)
            {
                normalized.erase(0, 3);
            }

            std::istringstream stream(normalized);
            std::string line;
            std::string currentSection;
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();

                line = trim_copy(line);
                if (line.empty() || line[0] == ';' || line[0] == '#')
                    continue;

                if (line.front() == '[' && line.back() == ']')
                {
                    currentSection = trim_copy(line.substr(1, line.size() - 2));
                    continue;
                }

                auto equals = line.find('=');
                if (equals == std::string::npos || currentSection.empty())
                    continue;

                auto key = trim_copy(line.substr(0, equals));
                auto value = trim_copy(line.substr(equals + 1));
                ini.sections[currentSection][key] = value;
            }

            ini.valid = !ini.sections.empty();
            return ini;
        }

        int ini_get_int(const BattlegroundIniFile& ini, const std::string& section, const std::string& key, int fallback)
        {
            auto secIt = ini.sections.find(section);
            if (secIt == ini.sections.end())
                return fallback;

            auto keyIt = secIt->second.find(key);
            if (keyIt == secIt->second.end() || keyIt->second.empty())
                return fallback;

            try
            {
                return std::stoi(keyIt->second);
            }
            catch (...)
            {
                return fallback;
            }
        }

        std::string ini_get_string(const BattlegroundIniFile& ini, const std::string& section, const std::string& key)
        {
            auto secIt = ini.sections.find(section);
            if (secIt == ini.sections.end())
                return {};

            auto keyIt = secIt->second.find(key);
            if (keyIt == secIt->second.end())
                return {};

            return keyIt->second;
        }

        std::filesystem::path find_battleground_client_ini_path()
        {
            auto root = get_client_root_path();
            const std::filesystem::path candidates[]
            {
                root / L"BattleFieldMoveInfo_Client.ini",
                root / L"Data" / L"BattleFieldMoveInfo_Client.ini",
                root / L"data" / L"BattleFieldMoveInfo_Client.ini",
            };

            for (const auto& path : candidates)
            {
                if (std::filesystem::exists(path))
                    return path;
            }

            return {};
        }

        std::string wide_to_utf8(const std::wstring& wide)
        {
            if (wide.empty())
                return {};

            auto size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return {};

            std::string result(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        std::string read_battleground_ini_string(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key)
        {
            std::wstring buffer(256, L'\0');
            auto count = GetPrivateProfileStringW(section, key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
            buffer.resize(count);
            return wide_to_utf8(buffer);
        }

        void ensure_battleground_entries_loaded()
        {
            if (g_battlegroundEntriesLoaded)
                return;

            g_battlegroundEntriesLoaded = true;
            g_battlegroundEntries.clear();

            auto path = find_battleground_client_ini_path();
            BattlegroundIniFile memoryIni{};
            bool useMemoryIni = false;

            if (path.empty())
            {
                std::string fileContents;
                if (!read_battleground_client_ini_from_saf(fileContents))
                    return;

                memoryIni = parse_battleground_ini(fileContents);
                if (!memoryIni.valid)
                    return;
                useMemoryIni = true;
            }

            auto count = useMemoryIni
                ? ini_get_int(memoryIni, "BATTLEFIELD_INFO", "BATTLEFIELD_COUNT", 0)
                : static_cast<int>(GetPrivateProfileIntW(L"BATTLEFIELD_INFO", L"BATTLEFIELD_COUNT", 0, path.c_str()));
            if (count <= 0)
                return;

            g_battlegroundEntries.reserve(count);

            for (int i = 1; i <= count; ++i)
            {
                wchar_t sectionWide[32]{};
                char sectionAnsi[32]{};
                swprintf_s(sectionWide, L"BATTLEFIELD_%d", i);
                std::snprintf(sectionAnsi, sizeof(sectionAnsi), "BATTLEFIELD_%d", i);
                std::string section(sectionAnsi);

                auto mapId = useMemoryIni
                    ? ini_get_int(memoryIni, section, "MAP_NO", -1)
                    : static_cast<int>(GetPrivateProfileIntW(sectionWide, L"MAP_NO", -1, path.c_str()));
                if (mapId < 0)
                    continue;

                BattlegroundClientEntry entry{};
                entry.id = useMemoryIni
                    ? ini_get_int(memoryIni, section, "ID", i)
                    : static_cast<int>(GetPrivateProfileIntW(sectionWide, L"ID", i, path.c_str()));
                entry.mapId = mapId;
                entry.levelMin = useMemoryIni
                    ? ini_get_int(memoryIni, section, "LEVEL_MIN", 0)
                    : static_cast<int>(GetPrivateProfileIntW(sectionWide, L"LEVEL_MIN", 0, path.c_str()));
                entry.levelMax = useMemoryIni
                    ? ini_get_int(memoryIni, section, "LEVEL_MAX", 0)
                    : static_cast<int>(GetPrivateProfileIntW(sectionWide, L"LEVEL_MAX", 0, path.c_str()));
                entry.enabled = useMemoryIni
                    ? ini_get_int(memoryIni, section, "ENABLED", 1) != 0
                    : GetPrivateProfileIntW(sectionWide, L"ENABLED", 1, path.c_str()) != 0;
                entry.title = useMemoryIni
                    ? ini_get_string(memoryIni, section, "TITLE")
                    : read_battleground_ini_string(path, sectionWide, L"TITLE");
                entry.subtitle = useMemoryIni
                    ? ini_get_string(memoryIni, section, "SUBTITLE")
                    : read_battleground_ini_string(path, sectionWide, L"SUBTITLE");
                entry.tag = useMemoryIni
                    ? ini_get_string(memoryIni, section, "TAG")
                    : read_battleground_ini_string(path, sectionWide, L"TAG");

                if (entry.title.empty())
                {
                    char mapName[256]{};
                    CPlayerData::GetMapName(mapName, mapId);
                    entry.title = mapName[0] ? mapName : "Battleground";
                }

                if (entry.subtitle.empty())
                    entry.subtitle = "Family spawn";

                if (entry.tag.empty())
                {
                    char tag[32]{};
                    std::snprintf(tag, sizeof(tag), "#%d", entry.id);
                    entry.tag = tag;
                }

                g_battlegroundEntries.push_back(std::move(entry));
            }
        }

        BattlegroundMapTexture& get_battleground_map_texture_state(std::unordered_map<int, BattlegroundMapTexture>& textureMap, int id)
        {
            return textureMap[id];
        }

        LPDIRECT3DTEXTURE9 get_battleground_button_texture(const char* relativePath, BattlegroundMapTexture& textureState);

        void ensure_battleground_asset_lookup(const char* relativePath, BattlegroundMapTexture& textureState)
        {
            if (textureState.lookupAttempted)
                return;

            textureState.lookupAttempted = true;

            auto sahPath = get_game_relative_path("data.sah");
            std::ifstream sahStream(sahPath, std::ios::binary);
            if (!sahStream)
                return;

            std::vector<char> sahData(
                (std::istreambuf_iterator<char>(sahStream)),
                std::istreambuf_iterator<char>());
            if (sahData.size() <= 0x34 || std::memcmp(sahData.data(), "SAH", 3) != 0)
                return;

            auto offset = std::size_t{ 0x34 };
            uint64_t fileOffset = 0;
            uint64_t fileSize = 0;
            if (!find_file_in_sah_directory(sahData, offset, "", to_lower_copy(relativePath), fileOffset, fileSize))
                return;

            textureState.dataOffset = fileOffset;
            textureState.dataSize = fileSize;
            textureState.found = true;
        }

        LPDIRECT3DTEXTURE9 get_battleground_map_texture(int id)
        {
            if (id <= 0)
                return nullptr;

            auto& textureState = get_battleground_map_texture_state(g_battlegroundMapTextures, id);
            char relativePath[64]{};
            std::snprintf(relativePath, sizeof(relativePath), "assets\\maps\\native\\%d.png", id);
            return get_battleground_button_texture(relativePath, textureState);
        }

        LPDIRECT3DTEXTURE9 get_battleground_current_map_texture(int id)
        {
            if (id <= 0)
                return nullptr;

            auto& textureState = get_battleground_map_texture_state(g_battlegroundCurrentMapTextures, id);
            char relativePath[80]{};
            std::snprintf(relativePath, sizeof(relativePath), "assets\\maps\\current\\%d.png", id);
            return get_battleground_button_texture(relativePath, textureState);
        }

        LPDIRECT3DTEXTURE9 get_battleground_disabled_map_texture(int id)
        {
            if (id <= 0)
                return nullptr;

            auto& textureState = get_battleground_map_texture_state(g_battlegroundDisabledMapTextures, id);
            char relativePath[82]{};
            std::snprintf(relativePath, sizeof(relativePath), "assets\\maps\\disabled\\%d.png", id);
            return get_battleground_button_texture(relativePath, textureState);
        }

        LPDIRECT3DTEXTURE9 get_battleground_background_texture()
        {
            auto& textureState = g_battlegroundBackgroundTexture;
            ensure_battleground_asset_lookup("assets\\maps\\panel\\background.png", textureState);

            if (textureState.loadAttempted)
                return textureState.texture;
            textureState.loadAttempted = true;

            if (!textureState.found || textureState.dataSize == 0 || textureState.dataSize > UINT_MAX || !g_device)
                return nullptr;

            auto safPath = get_game_relative_path("data.saf");
            std::ifstream safStream(safPath, std::ios::binary);
            if (!safStream)
                return nullptr;

            safStream.seekg(static_cast<std::streamoff>(textureState.dataOffset), std::ios::beg);
            if (!safStream)
                return nullptr;

            std::vector<char> fileData(static_cast<std::size_t>(textureState.dataSize), 0);
            safStream.read(fileData.data(), static_cast<std::streamsize>(fileData.size()));
            if (safStream.gcount() != static_cast<std::streamsize>(fileData.size()))
                return nullptr;

            textureState.texture = create_texture_from_image_memory(g_device, fileData.data(), static_cast<UINT>(fileData.size()));
            return textureState.texture;
        }

        LPDIRECT3DTEXTURE9 get_battleground_button_texture(const char* relativePath, BattlegroundMapTexture& textureState)
        {
            ensure_battleground_asset_lookup(relativePath, textureState);

            if (textureState.loadAttempted)
                return textureState.texture;
            textureState.loadAttempted = true;

            if (!textureState.found || textureState.dataSize == 0 || textureState.dataSize > UINT_MAX || !g_device)
                return nullptr;

            auto safPath = get_game_relative_path("data.saf");
            std::ifstream safStream(safPath, std::ios::binary);
            if (!safStream)
                return nullptr;

            safStream.seekg(static_cast<std::streamoff>(textureState.dataOffset), std::ios::beg);
            if (!safStream)
                return nullptr;

            std::vector<char> fileData(static_cast<std::size_t>(textureState.dataSize), 0);
            safStream.read(fileData.data(), static_cast<std::streamsize>(fileData.size()));
            if (safStream.gcount() != static_cast<std::streamsize>(fileData.size()))
                return nullptr;

            textureState.texture = create_texture_from_image_memory(g_device, fileData.data(), static_cast<UINT>(fileData.size()));
            return textureState.texture;
        }

        bool is_battleground_entry_available(const BattlegroundClientEntry& entry)
        {
            if (!entry.enabled || !g_pPlayerData)
                return false;

            auto level = static_cast<int>(g_pPlayerData->level);
            return level >= entry.levelMin && level <= entry.levelMax;
        }

        bool is_battleground_entry_current_map(const BattlegroundClientEntry& entry)
        {
            return g_pPlayerData && g_pPlayerData->mapId == entry.mapId;
        }

        std::string battleground_level_label(const BattlegroundClientEntry& entry)
        {
            char buffer[64]{};
            if (entry.levelMin == entry.levelMax)
                std::snprintf(buffer, sizeof(buffer), "%d lvl only", entry.levelMin);
            else
                std::snprintf(buffer, sizeof(buffer), "%d - %d lvl", entry.levelMin, entry.levelMax);
            return buffer;
        }

        ImU32 battleground_accent_color(const BattlegroundClientEntry& entry)
        {
            auto seed = static_cast<unsigned>(entry.id * 2654435761u) ^ static_cast<unsigned>(entry.mapId * 2246822519u);
            auto r = static_cast<int>(80 + (seed & 0x3F));
            auto g = static_cast<int>(92 + ((seed >> 6) & 0x3F));
            auto b = static_cast<int>(104 + ((seed >> 12) & 0x3F));
            return IM_COL32(r, g, b, 255);
        }

        void send_battleground_request(int destinationId)
        {
            GameMovePvPZoneIncoming outgoing{};
            outgoing.destinationId = static_cast<uint8_t>(std::clamp(destinationId, 0, 255));
            CNetwork::Send(&outgoing, sizeof(outgoing));
        }

        bool draw_battleground_card(const BattlegroundClientEntry& entry, const ImVec2& size)
        {
            ImGui::PushID(entry.id);

            auto available = is_battleground_entry_available(entry);
            auto currentMap = is_battleground_entry_current_map(entry);
            auto clicked = ImGui::InvisibleButton("##bg_card", size);
            auto hovered = ImGui::IsItemHovered();
            auto min = ImGui::GetItemRectMin();
            auto max = ImGui::GetItemRectMax();
            auto* drawList = ImGui::GetWindowDrawList();
            LPDIRECT3DTEXTURE9 mapTexture = nullptr;
            if (!available)
                mapTexture = get_battleground_disabled_map_texture(entry.id);
            else if (currentMap)
                mapTexture = get_battleground_current_map_texture(entry.id);
            if (!mapTexture)
                mapTexture = get_battleground_map_texture(entry.id);

            auto accent = battleground_accent_color(entry);
            if (mapTexture)
            {
                drawList->AddImage(reinterpret_cast<ImTextureID>(mapTexture), min, max);
                drawList->AddRect(min, max, hovered ? IM_COL32(255, 232, 184, 255) : IM_COL32(186, 154, 108, 220), 6.0f, 0, hovered ? 2.0f : 1.0f);
                if (hovered)
                    drawList->AddRectFilled(ImVec2(min.x + 2.5f, min.y + 2.5f), ImVec2(max.x - 2.5f, max.y - 2.5f), IM_COL32(255, 255, 255, 10), 5.0f);
            }
            else
            {
                auto bgTop = hovered ? IM_COL32(50, 40, 32, 238) : IM_COL32(38, 31, 27, 228);
                auto bgBottom = hovered ? IM_COL32(24, 21, 19, 242) : IM_COL32(18, 17, 16, 236);
                drawList->AddRectFilledMultiColor(min, max, bgTop, bgTop, bgBottom, bgBottom);
                drawList->AddRect(min, max, hovered ? IM_COL32(224, 196, 148, 255) : IM_COL32(118, 96, 74, 220), 6.0f, 0, hovered ? 2.0f : 1.0f);
                drawList->AddRectFilled(ImVec2(min.x, min.y), ImVec2(max.x, min.y + 20.0f), accent, 6.0f, ImDrawFlags_RoundCornersTop);
                drawList->AddText(ImVec2(min.x + 12.0f, min.y + 26.0f), IM_COL32(255, 214, 170, 255), entry.title.c_str());
                drawList->AddText(ImVec2(min.x + 12.0f, min.y + 48.0f), IM_COL32(224, 224, 224, 245), entry.subtitle.c_str());
                auto levelLabel = battleground_level_label(entry);
                drawList->AddText(ImVec2(min.x + 12.0f, min.y + size.y - 28.0f), IM_COL32(230, 230, 230, 230), levelLabel.c_str());
                auto tagSize = ImGui::CalcTextSize(entry.tag.c_str());
                drawList->AddText(ImVec2(max.x - tagSize.x - 12.0f, min.y + 26.0f), IM_COL32(230, 230, 230, 220), entry.tag.c_str());
            }

            ImGui::PopID();

            if (!clicked || !available || currentMap)
                return false;

            send_battleground_request(entry.id);
            return true;
        }

        bool draw_battleground_icon_button(const char* id, LPDIRECT3DTEXTURE9 texture, const ImVec2& position, const ImVec2& size, const char* fallbackLabel)
        {
            ImGui::SetCursorScreenPos(position);
            ImGui::PushID(id);
            auto clicked = ImGui::InvisibleButton("##icon_button", size);
            auto hovered = ImGui::IsItemHovered();
            auto min = ImGui::GetItemRectMin();
            auto max = ImGui::GetItemRectMax();
            auto* drawList = ImGui::GetWindowDrawList();

            if (texture)
                drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 228, 214, 236));
            else
            {
                drawList->AddRectFilled(min, max, hovered ? IM_COL32(160, 36, 36, 180) : IM_COL32(24, 24, 24, 110), 8.0f);
                drawList->AddText(ImVec2(min.x + 6.0f, min.y + 2.0f), IM_COL32(245, 238, 226, 255), fallbackLabel);
            }
            ImGui::PopID();
            return clicked;
        }
    }

    void draw_battleground_panel()
    {
        if (!g_showBattlegroundPanel)
            return;

        ensure_battleground_entries_loaded();
        if (g_battlegroundEntries.empty())
        {
            g_showBattlegroundPanel = false;
            return;
        }

        auto& io = ImGui::GetIO();
        if (g_battlegroundPanelPosition.x < 0.0f || g_battlegroundPanelPosition.y < 0.0f)
        {
            auto centered = ImVec2((io.DisplaySize.x - kBattlegroundPanelSize.x) * 0.5f,
                (io.DisplaySize.y - kBattlegroundPanelSize.y) * 0.5f);
            g_battlegroundPanelPosition = clamp_window_position(centered, kBattlegroundPanelSize, io.DisplaySize);
            mark_imgui_settings_dirty();
        }
        else
        {
            g_battlegroundPanelPosition = clamp_window_position(g_battlegroundPanelPosition, kBattlegroundPanelSize, io.DisplaySize);
        }

        ImGui::SetNextWindowPos(g_battlegroundPanelPosition, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(kBattlegroundPanelSize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);

        bool panelOpen = g_showBattlegroundPanel;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        if (ImGui::Begin(
            "##battleground_panel",
            &panelOpen,
            ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse))
        {
            auto pos = ImGui::GetWindowPos();
            auto size = ImGui::GetWindowSize();
            remember_rect(g_battlegroundPanelRect, pos, ImVec2(pos.x + size.x, pos.y + size.y));
            if (std::fabs(pos.x - g_battlegroundPanelPosition.x) >= 0.5f
                || std::fabs(pos.y - g_battlegroundPanelPosition.y) >= 0.5f)
            {
                g_battlegroundPanelPosition = pos;
                mark_imgui_settings_dirty();
            }

            auto* drawList = ImGui::GetWindowDrawList();
            if (auto* backgroundTexture = get_battleground_background_texture())
            {
                drawList->AddImage(reinterpret_cast<ImTextureID>(backgroundTexture), pos, ImVec2(pos.x + size.x, pos.y + size.y));
            }
            else
            {
                drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(12, 12, 12, 238), 10.0f);
                drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(162, 132, 92, 255), 10.0f, 0, 2.0f);
            }

            constexpr float kBackgroundNativeWidth = 902.0f;
            constexpr float kBackgroundNativeHeight = 572.0f;
            auto scaleX = size.x / kBackgroundNativeWidth;
            auto scaleY = size.y / kBackgroundNativeHeight;

            auto closeSize = ImVec2(26.0f * scaleX, 26.0f * scaleY);
            auto closePos = ImVec2(pos.x + (size.x - (44.0f * scaleX)), pos.y + (24.0f * scaleY));
            if (draw_battleground_icon_button(
                "bg_close",
                get_battleground_button_texture("assets\\maps\\panel\\button_close.png", g_battlegroundCloseButtonTexture),
                closePos,
                closeSize,
                "x"))
            {
                panelOpen = false;
                release_imgui_capture();
            }

            constexpr int kColumns = 3;
            constexpr int kRows = 4;
            constexpr int kPageSize = kColumns * kRows;
            const int totalEntries = static_cast<int>(g_battlegroundEntries.size());
            const int totalPages = std::max(1, (totalEntries + kPageSize - 1) / kPageSize);
            g_battlegroundPage = std::clamp(g_battlegroundPage, 0, totalPages - 1);

            auto gridMin = ImVec2(pos.x + (36.0f * scaleX), pos.y + (80.0f * scaleY));
            auto gridMax = ImVec2(pos.x + (866.0f * scaleX), pos.y + (490.0f * scaleY));
            auto cellWidth = (gridMax.x - gridMin.x) / static_cast<float>(kColumns);
            auto cellHeight = (gridMax.y - gridMin.y) / static_cast<float>(kRows);

            auto cardInsetX = 8.0f * scaleX;
            auto cardInsetY = 6.0f * scaleY;
            auto cardWidth = cellWidth - (cardInsetX * 2.0f);
            auto cardHeight = cellHeight - (cardInsetY * 2.0f);

            for (int row = 0; row < kRows; ++row)
            {
                for (int col = 0; col < kColumns; ++col)
                {
                    auto indexOnPage = row * kColumns + col;
                    auto entryIndex = g_battlegroundPage * kPageSize + indexOnPage;
                    auto cellPos = ImVec2(
                        gridMin.x + col * cellWidth + cardInsetX,
                        gridMin.y + row * cellHeight + cardInsetY);
                    ImGui::SetCursorScreenPos(cellPos);

                    if (entryIndex >= totalEntries)
                    {
                        ImGui::Dummy(ImVec2(cardWidth, cardHeight));
                        continue;
                    }

                    if (draw_battleground_card(g_battlegroundEntries[entryIndex], ImVec2(cardWidth, cardHeight)))
                    {
                        g_showBattlegroundPanel = false;
                        release_imgui_capture();
                    }
                }
            }

            auto centerX = pos.x + (size.x * 0.5f);
            auto navY = pos.y + (526.0f * scaleY);
            auto navButtonSize = ImVec2(36.0f * scaleX, 24.0f * scaleY);
            char pageLabel[32]{};
            std::snprintf(pageLabel, sizeof(pageLabel), "%d / %d", g_battlegroundPage + 1, totalPages);
            auto pageTextSize = ImGui::CalcTextSize(pageLabel);
            auto navGap = 10.0f * scaleX;
            auto navGroupWidth = navButtonSize.x * 2.0f + navGap * 2.0f + pageTextSize.x;
            auto navStartX = centerX - (navGroupWidth * 0.5f);

            bool prevClicked = draw_battleground_icon_button(
                "bg_prev",
                get_battleground_button_texture("assets\\maps\\panel\\button_prev.png", g_battlegroundPrevButtonTexture),
                ImVec2(navStartX, navY),
                navButtonSize,
                "<");

            auto textX = navStartX + navButtonSize.x + navGap;
            auto textY = navY + ((navButtonSize.y - pageTextSize.y) * 0.5f) - (1.0f * scaleY);
            ImGui::SetCursorScreenPos(ImVec2(textX, textY));
            ImGui::TextColored(ImVec4(0.96f, 0.89f, 0.78f, 1.0f), "%s", pageLabel);

            bool nextClicked = draw_battleground_icon_button(
                "bg_next",
                get_battleground_button_texture("assets\\maps\\panel\\button_next.png", g_battlegroundNextButtonTexture),
                ImVec2(navStartX + navButtonSize.x + navGap + pageTextSize.x + navGap, navY),
                navButtonSize,
                ">");

            if (prevClicked && g_battlegroundPage > 0)
            {
                --g_battlegroundPage;
                release_imgui_capture();
            }

            if (nextClicked && g_battlegroundPage + 1 < totalPages)
            {
                ++g_battlegroundPage;
                release_imgui_capture();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);

        g_showBattlegroundPanel = panelOpen;
    }

    void ensure_battleground_entries_ready()
    {
        ensure_battleground_entries_loaded();
    }

    void release_battleground_map_textures()
    {
        for (auto& [_, textureState] : g_battlegroundMapTextures)
        {
            if (textureState.texture)
            {
                textureState.texture->Release();
                textureState.texture = nullptr;
            }
            textureState.loadAttempted = false;
        }

        for (auto& [_, textureState] : g_battlegroundCurrentMapTextures)
        {
            if (textureState.texture)
            {
                textureState.texture->Release();
                textureState.texture = nullptr;
            }
            textureState.loadAttempted = false;
        }

        for (auto& [_, textureState] : g_battlegroundDisabledMapTextures)
        {
            if (textureState.texture)
            {
                textureState.texture->Release();
                textureState.texture = nullptr;
            }
            textureState.loadAttempted = false;
        }

        if (g_battlegroundBackgroundTexture.texture)
        {
            g_battlegroundBackgroundTexture.texture->Release();
            g_battlegroundBackgroundTexture.texture = nullptr;
        }
        g_battlegroundBackgroundTexture.loadAttempted = false;

        if (g_battlegroundCloseButtonTexture.texture)
        {
            g_battlegroundCloseButtonTexture.texture->Release();
            g_battlegroundCloseButtonTexture.texture = nullptr;
        }
        g_battlegroundCloseButtonTexture.loadAttempted = false;

        if (g_battlegroundPrevButtonTexture.texture)
        {
            g_battlegroundPrevButtonTexture.texture->Release();
            g_battlegroundPrevButtonTexture.texture = nullptr;
        }
        g_battlegroundPrevButtonTexture.loadAttempted = false;

        if (g_battlegroundNextButtonTexture.texture)
        {
            g_battlegroundNextButtonTexture.texture->Release();
            g_battlegroundNextButtonTexture.texture = nullptr;
        }
        g_battlegroundNextButtonTexture.loadAttempted = false;
    }

    void send_welcome_sysmsg_once()
    {
        if (!is_game_scene())
        {
            g_waitingWelcomeMessage = false;
            g_welcomeStartTick = 0;
            return;
        }

        if (g_sentWelcomeMessage)
            return;

        auto now = GetTickCount();
        if (!g_waitingWelcomeMessage)
        {
            g_waitingWelcomeMessage = true;
            g_welcomeStartTick = now;
            return;
        }

        // Give the game scene time to settle before posting the welcome notice.
        if (now - g_welcomeStartTick < 4000)
            return;

        Static::SysMsgToChatBox(ChatType::Acquire31, 12001, 12);
        g_sentWelcomeMessage = true;
        g_waitingWelcomeMessage = false;
    }

} // namespace imgui_layer

void toggle_battleground_panel()
{
    imgui_layer::ensure_battleground_entries_ready();
    if (imgui_layer::g_battlegroundEntries.empty())
        return;

    imgui_layer::g_showBattlegroundPanel = !imgui_layer::g_showBattlegroundPanel;
}

void close_battleground_panel()
{
    imgui_layer::g_showBattlegroundPanel = false;
}

bool is_battleground_panel_available()
{
    imgui_layer::ensure_battleground_entries_ready();
    return !imgui_layer::g_battlegroundEntries.empty();
}
