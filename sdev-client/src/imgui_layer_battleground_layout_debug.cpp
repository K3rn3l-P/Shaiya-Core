#include "include/imgui_layer_internal.h"
#include <filesystem>
#include <set>

namespace imgui_layer
{
    namespace
    {
        void reset_battleground_layout_defaults()
        {
            g_battlegroundLayoutDebug = BattlegroundLayoutDebugState{};
        }

        void reset_battleground_debug_image_overrides()
        {
            for (auto& overrideEntry : g_battlegroundDebugImageOverrides)
            {
                if (overrideEntry.texture)
                    overrideEntry.texture->Release();
                overrideEntry.texture = nullptr;
                for (auto& frame : overrideEntry.frames)
                {
                    if (frame.texture)
                        frame.texture->Release();
                }
                overrideEntry.frames.clear();
                overrideEntry.enabled = false;
                overrideEntry.loadAttempted = false;
                overrideEntry.frameCount = 0;
                overrideEntry.resolvedSource.clear();
                overrideEntry.status.clear();
            }
        }

        void draw_override_preview(const char* id, LPDIRECT3DTEXTURE9 texture)
        {
            if (!texture)
                return;

            D3DSURFACE_DESC desc{};
            if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.Width == 0 || desc.Height == 0)
                return;

            constexpr float kMaxW = 180.0f;
            constexpr float kMaxH = 72.0f;
            auto scale = std::min(kMaxW / static_cast<float>(desc.Width), kMaxH / static_cast<float>(desc.Height));
            scale = std::min(scale, 1.0f);

            ImGui::TextDisabled("Preview:");
            ImGui::PushID(id);
            ImGui::Image(reinterpret_cast<ImTextureID>(texture), ImVec2(desc.Width * scale, desc.Height * scale));
            ImGui::PopID();
        }

        void input_vec2(const char* label, ImVec2& value)
        {
            float data[2]{ value.x, value.y };
            if (ImGui::InputFloat2(label, data, "%.1f"))
                value = ImVec2(data[0], data[1]);
        }

        void input_float(const char* label, float& value)
        {
            ImGui::InputFloat(label, &value, 0.1f, 1.0f, "%.1f");
        }

        bool is_absolute_windows_path(const std::string& path)
        {
            return path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
        }

        std::string normalize_path_for_compare(std::string value)
        {
            std::replace(value.begin(), value.end(), '/', '\\');
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string trim_path_input(std::string value)
        {
            auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!value.empty() && isSpace(static_cast<unsigned char>(value.front())))
                value.erase(value.begin());
            while (!value.empty() && isSpace(static_cast<unsigned char>(value.back())))
                value.pop_back();
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            return value;
        }

        std::vector<std::string> get_assets_search_roots()
        {
            return {
                get_game_relative_path("data\\assets"),
                get_game_relative_path("data\\Assets"),
                get_game_relative_path("assets"),
                get_game_relative_path("Assets")
            };
        }

        bool file_exists(const std::string& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return static_cast<bool>(stream);
        }

        std::string find_image_in_assets_recursive(const std::string& inputPath)
        {
            namespace fs = std::filesystem;
            auto normalizedInput = normalize_path_for_compare(inputPath);
            auto hasSubPath = normalizedInput.find('\\') != std::string::npos;
            auto fileNameOnly = normalize_path_for_compare(fs::path(inputPath).filename().string());

            for (const auto& assetsRoot : get_assets_search_roots())
            {
                std::error_code ec;
                if (!fs::exists(assetsRoot, ec))
                    continue;

                for (fs::recursive_directory_iterator it(assetsRoot, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
                {
                    if (ec)
                        continue;
                    if (!it->is_regular_file(ec) || ec)
                        continue;

                    auto candidatePath = it->path().string();
                    auto candidateNorm = normalize_path_for_compare(candidatePath);
                    if (hasSubPath)
                    {
                        if (candidateNorm.size() >= normalizedInput.size()
                            && candidateNorm.compare(candidateNorm.size() - normalizedInput.size(), normalizedInput.size(), normalizedInput) == 0)
                            return candidatePath;
                    }
                    else
                    {
                        auto candidateName = normalize_path_for_compare(it->path().filename().string());
                        if (candidateName == fileNameOnly)
                            return candidatePath;
                    }
                }
            }

            return {};
        }

        std::string resolve_override_image_path(const std::string& inputPath)
        {
            return imgui_layer::resolve_asset_image_path(inputPath);
        }

        RouletteDebugImageOverride& get_override(BattlegroundDebugImageSlot slot)
        {
            return g_battlegroundDebugImageOverrides[static_cast<std::size_t>(slot)];
        }

        const char* get_override_slot_label(BattlegroundDebugImageSlot slot)
        {
            switch (slot)
            {
            case BattlegroundDebugImageSlot::Background: return "Background";
            case BattlegroundDebugImageSlot::CloseButton: return "Close Button";
            case BattlegroundDebugImageSlot::PrevButton: return "Prev Button";
            case BattlegroundDebugImageSlot::NextButton: return "Next Button";
            case BattlegroundDebugImageSlot::MapNative: return "Map Card (Native)";
            case BattlegroundDebugImageSlot::MapCurrent: return "Map Card (Current)";
            case BattlegroundDebugImageSlot::MapDisabled: return "Map Card (Disabled)";
            default: return "Unknown";
            }
        }

        bool load_battleground_debug_override_image(BattlegroundDebugImageSlot slot)
        {
            auto& overrideEntry = get_override(slot);
            overrideEntry.loadAttempted = true;

            if (overrideEntry.texture)
            {
                overrideEntry.texture->Release();
                overrideEntry.texture = nullptr;
            }
            for (auto& frame : overrideEntry.frames)
            {
                if (frame.texture)
                    frame.texture->Release();
            }
            overrideEntry.frames.clear();
            overrideEntry.frameCount = 0;
            overrideEntry.resolvedSource.clear();

            if (!g_device)
            {
                overrideEntry.status = "Device not ready";
                return false;
            }

            auto absolutePath = resolve_override_image_path(overrideEntry.path);
            if (absolutePath.empty())
            {
                overrideEntry.status = "Empty path";
                return false;
            }

            std::vector<char> fileData;
            std::string resolvedSource;
            if (!read_asset_image_bytes(overrideEntry.path, fileData, &resolvedSource))
            {
                overrideEntry.status = "File not found";
                return false;
            }
            if (fileData.empty())
            {
                overrideEntry.status = "File is empty";
                return false;
            }

            auto normalizedPath = normalize_asset_browser_text(overrideEntry.path);
            auto extension = std::filesystem::path(normalizedPath).extension().string();
            if (extension == ".gif")
            {
                if (!load_gif_frames_from_memory(fileData.data(), static_cast<UINT>(fileData.size()), overrideEntry.frames))
                {
                    overrideEntry.status = "Unsupported GIF or decode failed";
                    return false;
                }

                overrideEntry.texture = get_animated_texture_frame(overrideEntry.frames);
                overrideEntry.frameCount = static_cast<uint32_t>(overrideEntry.frames.size());
                overrideEntry.resolvedSource = resolvedSource;
                overrideEntry.status = "Loaded GIF";
                return overrideEntry.texture != nullptr;
            }

            overrideEntry.texture = create_texture_from_image_memory(g_device, fileData.data(), static_cast<UINT>(fileData.size()));
            if (!overrideEntry.texture)
            {
                overrideEntry.status = "Unsupported format or decode failed";
                return false;
            }

            overrideEntry.resolvedSource = resolvedSource;
            overrideEntry.status = "Loaded";
            return true;
        }

        void clear_battleground_debug_override_image(BattlegroundDebugImageSlot slot)
        {
            auto& overrideEntry = get_override(slot);
            if (overrideEntry.texture)
                overrideEntry.texture->Release();
            overrideEntry.texture = nullptr;
            for (auto& frame : overrideEntry.frames)
            {
                if (frame.texture)
                    frame.texture->Release();
            }
            overrideEntry.frames.clear();
            overrideEntry.enabled = false;
            overrideEntry.loadAttempted = false;
            overrideEntry.frameCount = 0;
            overrideEntry.resolvedSource.clear();
            overrideEntry.status = "Cleared";
        }

        std::vector<std::string> collect_asset_image_candidates()
        {
            return imgui_layer::collect_asset_image_candidates(false);
        }

        bool matches_candidate_filter(const std::string& candidate, const std::string& query)
        {
            return imgui_layer::asset_image_candidate_matches_filter(candidate, query);
        }

        void draw_asset_image_selector(BattlegroundDebugImageSlot slot, char* pathBuffer, std::size_t pathBufferSize, char* filterBuffer, std::size_t filterBufferSize)
        {
            auto browserId = std::string("bg_asset_browser_") + std::to_string(static_cast<int>(slot));
            if (imgui_layer::draw_asset_image_browser(browserId.c_str(), pathBuffer, pathBufferSize, filterBuffer, filterBufferSize, 140.0f))
            {
                auto& overrideEntry = get_override(slot);
                overrideEntry.path = pathBuffer;
                load_battleground_debug_override_image(slot);
                overrideEntry.enabled = true;
            }
        }

        void draw_battleground_debug_images_controls()
        {
            if (!ImGui::CollapsingHeader("Debug Images"))
                return;

            ImGui::TextDisabled("Path relativo, sotto-path o nome file. Ogni slot ha un browser ricorsivo degli `Assets` del client.");

            static std::array<std::array<char, MAX_PATH>, static_cast<std::size_t>(BattlegroundDebugImageSlot::Count)> s_pathBuffers{};
            static std::array<std::array<char, 96>, static_cast<std::size_t>(BattlegroundDebugImageSlot::Count)> s_filterBuffers{};
            static bool s_buffersInitialized = false;
            if (!s_buffersInitialized)
            {
                for (std::size_t i = 0; i < s_pathBuffers.size(); ++i)
                    std::snprintf(s_pathBuffers[i].data(), s_pathBuffers[i].size(), "%s", g_battlegroundDebugImageOverrides[i].path.c_str());
                s_buffersInitialized = true;
            }

            for (std::size_t i = 0; i < static_cast<std::size_t>(BattlegroundDebugImageSlot::Count); ++i)
            {
                auto slot = static_cast<BattlegroundDebugImageSlot>(i);
                auto& overrideEntry = g_battlegroundDebugImageOverrides[i];

                ImGui::SeparatorText(get_override_slot_label(slot));
                ImGui::Checkbox((std::string("Use##bg_img_use_") + std::to_string(i)).c_str(), &overrideEntry.enabled);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText((std::string("Path##bg_img_path_") + std::to_string(i)).c_str(), s_pathBuffers[i].data(), s_pathBuffers[i].size());

                if (ImGui::TreeNode((std::string("Asset Browser##bg_img_browser_") + std::to_string(i)).c_str()))
                {
                    draw_asset_image_selector(slot, s_pathBuffers[i].data(), s_pathBuffers[i].size(), s_filterBuffers[i].data(), s_filterBuffers[i].size());
                    ImGui::TreePop();
                }

                if (ImGui::Button((std::string("Load##bg_img_load_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_battleground_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Reload##bg_img_reload_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_battleground_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Clear##bg_img_clear_") + std::to_string(i)).c_str()))
                {
                    clear_battleground_debug_override_image(slot);
                    s_pathBuffers[i][0] = '\0';
                }

                if (!overrideEntry.status.empty())
                    ImGui::TextDisabled("Status: %s", overrideEntry.status.c_str());
                if (overrideEntry.frameCount > 0)
                    ImGui::TextDisabled("Animated GIF: %u frames", overrideEntry.frameCount);
                if (!overrideEntry.resolvedSource.empty())
                    ImGui::TextDisabled("Source: %s", overrideEntry.resolvedSource.c_str());

                auto previewTexture = overrideEntry.frames.empty()
                    ? overrideEntry.texture
                    : get_animated_texture_frame(overrideEntry.frames);
                draw_override_preview((std::string("slot_") + std::to_string(i)).c_str(), previewTexture);
            }
        }
    }

    LPDIRECT3DTEXTURE9 get_battleground_debug_override_texture(BattlegroundDebugImageSlot slot)
    {
        auto& overrideEntry = get_override(slot);
        if (!overrideEntry.enabled)
            return nullptr;

        if (!overrideEntry.texture && overrideEntry.frames.empty() && !overrideEntry.loadAttempted && !overrideEntry.path.empty())
            load_battleground_debug_override_image(slot);

        if (!overrideEntry.frames.empty())
            return get_animated_texture_frame(overrideEntry.frames);

        return overrideEntry.texture;
    }

    LPDIRECT3DTEXTURE9 get_battleground_debug_override_texture_for_path(const char* relativePath)
    {
        if (!relativePath || !relativePath[0])
            return nullptr;

        auto path = normalize_path_for_compare(relativePath);
        if (path == "assets\\maps\\panel\\button_close.png")
            return get_battleground_debug_override_texture(BattlegroundDebugImageSlot::CloseButton);
        if (path == "assets\\maps\\panel\\button_prev.png")
            return get_battleground_debug_override_texture(BattlegroundDebugImageSlot::PrevButton);
        if (path == "assets\\maps\\panel\\button_next.png")
            return get_battleground_debug_override_texture(BattlegroundDebugImageSlot::NextButton);
        return nullptr;
    }

    std::string build_battleground_layout_dump()
    {
        char block[2048]{};
        std::snprintf(
            block,
            sizeof(block),
            "backgroundNativeWidth = %.1ff\n"
            "backgroundNativeHeight = %.1ff\n"
            "closeRightOffset = %.1ff\n"
            "closeTopOffset = %.1ff\n"
            "closeButtonSize = { %.1ff, %.1ff }\n"
            "gridColumns = %d\n"
            "gridRows = %d\n"
            "gridMin = { %.1ff, %.1ff }\n"
            "gridMax = { %.1ff, %.1ff }\n"
            "cardInset = { %.1ff, %.1ff }\n"
            "navY = %.1ff\n"
            "navButtonSize = { %.1ff, %.1ff }\n"
            "navGap = %.1ff\n"
            "navTextYOffset = %.1ff\n",
            g_battlegroundLayoutDebug.backgroundNativeWidth,
            g_battlegroundLayoutDebug.backgroundNativeHeight,
            g_battlegroundLayoutDebug.closeRightOffset,
            g_battlegroundLayoutDebug.closeTopOffset,
            g_battlegroundLayoutDebug.closeButtonSize.x,
            g_battlegroundLayoutDebug.closeButtonSize.y,
            g_battlegroundLayoutDebug.gridColumns,
            g_battlegroundLayoutDebug.gridRows,
            g_battlegroundLayoutDebug.gridMin.x,
            g_battlegroundLayoutDebug.gridMin.y,
            g_battlegroundLayoutDebug.gridMax.x,
            g_battlegroundLayoutDebug.gridMax.y,
            g_battlegroundLayoutDebug.cardInset.x,
            g_battlegroundLayoutDebug.cardInset.y,
            g_battlegroundLayoutDebug.navY,
            g_battlegroundLayoutDebug.navButtonSize.x,
            g_battlegroundLayoutDebug.navButtonSize.y,
            g_battlegroundLayoutDebug.navGap,
            g_battlegroundLayoutDebug.navTextYOffset);

        std::string dump = block;

        dump += "\nDebug Images:\n";
        for (std::size_t i = 0; i < static_cast<std::size_t>(BattlegroundDebugImageSlot::Count); ++i)
        {
            auto slot = static_cast<BattlegroundDebugImageSlot>(i);
            const auto& entry = g_battlegroundDebugImageOverrides[i];
            dump += std::string(get_override_slot_label(slot)) + " = { enabled=" + (entry.enabled ? "true" : "false") + ", path=\"" + entry.path + "\" }\n";
        }

        return dump;
    }

    void draw_battleground_layout_debug_panel()
    {
        if (!g_showBattlegroundLayoutDebugPanel)
            return;

        ImGui::SetNextWindowSize(ImVec2(560.0f, 760.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Battleground Layout Debug", &g_showBattlegroundLayoutDebugPanel, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        if (ImGui::CollapsingHeader("Draw Guides"))
        {
            ImGui::Checkbox("Draw Guides##bg_layout", &g_battlegroundLayoutDebug.drawGuides);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Defaults##bg_layout"))
            {
                reset_battleground_layout_defaults();
                reset_battleground_debug_image_overrides();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Dump##bg_layout"))
            {
                auto dump = build_battleground_layout_dump();
                ImGui::SetClipboardText(dump.c_str());
            }
        }

        if (ImGui::CollapsingHeader("Panel Frame"))
        {
            input_float("Background Native Width", g_battlegroundLayoutDebug.backgroundNativeWidth);
            input_float("Background Native Height", g_battlegroundLayoutDebug.backgroundNativeHeight);
            input_float("Close Right Offset", g_battlegroundLayoutDebug.closeRightOffset);
            input_float("Close Top Offset", g_battlegroundLayoutDebug.closeTopOffset);
            input_vec2("Close Button Size", g_battlegroundLayoutDebug.closeButtonSize);
        }

        if (ImGui::CollapsingHeader("Grid"))
        {
            ImGui::InputInt("Columns", &g_battlegroundLayoutDebug.gridColumns, 1, 1);
            ImGui::InputInt("Rows", &g_battlegroundLayoutDebug.gridRows, 1, 1);
            g_battlegroundLayoutDebug.gridColumns = std::clamp(g_battlegroundLayoutDebug.gridColumns, 1, 8);
            g_battlegroundLayoutDebug.gridRows = std::clamp(g_battlegroundLayoutDebug.gridRows, 1, 8);
            input_vec2("Grid Min", g_battlegroundLayoutDebug.gridMin);
            input_vec2("Grid Max", g_battlegroundLayoutDebug.gridMax);
            input_vec2("Card Inset", g_battlegroundLayoutDebug.cardInset);
        }

        if (ImGui::CollapsingHeader("Navigation"))
        {
            input_float("Nav Y", g_battlegroundLayoutDebug.navY);
            input_vec2("Nav Button Size", g_battlegroundLayoutDebug.navButtonSize);
            input_float("Nav Gap", g_battlegroundLayoutDebug.navGap);
            input_float("Nav Text Y Offset", g_battlegroundLayoutDebug.navTextYOffset);
        }

        draw_battleground_debug_images_controls();

        ImGui::SeparatorText("Current Layout Dump");
        auto dump = build_battleground_layout_dump();
        std::vector<char> dumpBuffer(dump.begin(), dump.end());
        dumpBuffer.push_back('\0');

        if (ImGui::SmallButton("Copy Dump##bg_layout_footer"))
            ImGui::SetClipboardText(dump.c_str());

        ImGui::BeginChild("##bg_layout_dump_child", ImVec2(0.0f, 180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::InputTextMultiline(
            "##bg_layout_dump_text",
            dumpBuffer.data(),
            dumpBuffer.size(),
            ImVec2(-1.0f, -1.0f),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::EndChild();

        ImGui::End();
    }
}
