#include "include/imgui_layer_internal.h"
#include <filesystem>
#include <set>

namespace imgui_layer
{
    namespace
    {
        void reset_roulette_layout_defaults()
        {
            g_rouletteLayoutDebug = RouletteLayoutDebugState{};
        }

        void reset_roulette_debug_image_overrides()
        {
            for (auto& overrideEntry : g_rouletteDebugImageOverrides)
            {
                if (overrideEntry.texture)
                    overrideEntry.texture->Release();
                overrideEntry.texture = nullptr;
                overrideEntry.enabled = false;
                overrideEntry.loadAttempted = false;
                overrideEntry.status.clear();
            }
        }

        void input_vec2(const char* label, ImVec2& value, float step = 0.1f)
        {
            float data[2]{ value.x, value.y };
            if (ImGui::InputFloat2(label, data, "%.1f"))
                value = ImVec2(data[0], data[1]);
        }

        void input_float(const char* label, float& value, float step = 0.1f)
        {
            ImGui::InputFloat(label, &value, step, step * 10.0f, "%.1f");
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
                        {
                            return candidatePath;
                        }
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
            auto sanitizedInput = trim_path_input(inputPath);
            if (sanitizedInput.empty())
                return {};

            if (is_absolute_windows_path(sanitizedInput))
                return sanitizedInput;

            auto directRelativePath = get_game_relative_path(sanitizedInput.c_str());
            if (file_exists(directRelativePath))
                return directRelativePath;

            auto recursiveMatch = find_image_in_assets_recursive(sanitizedInput);
            if (!recursiveMatch.empty())
                return recursiveMatch;

            return directRelativePath;
        }

        RouletteDebugImageOverride& get_override(RouletteDebugImageSlot slot)
        {
            return g_rouletteDebugImageOverrides[static_cast<std::size_t>(slot)];
        }

        const char* get_override_slot_label(RouletteDebugImageSlot slot)
        {
            switch (slot)
            {
            case RouletteDebugImageSlot::Background: return "Background";
            case RouletteDebugImageSlot::Dragon: return "Dragon";
            case RouletteDebugImageSlot::LeftPanel: return "Left Panel";
            case RouletteDebugImageSlot::RightPanel: return "Right Panel";
            default: return "Unknown";
            }
        }

        bool load_roulette_debug_override_image(RouletteDebugImageSlot slot)
        {
            auto& overrideEntry = get_override(slot);
            overrideEntry.loadAttempted = true;

            if (overrideEntry.texture)
            {
                overrideEntry.texture->Release();
                overrideEntry.texture = nullptr;
            }

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

            std::ifstream stream(absolutePath, std::ios::binary);
            if (!stream)
            {
                overrideEntry.status = "File not found";
                return false;
            }

            std::vector<char> fileData((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (fileData.empty())
            {
                overrideEntry.status = "File is empty";
                return false;
            }

            overrideEntry.texture = create_texture_from_image_memory(g_device, fileData.data(), static_cast<UINT>(fileData.size()));
            if (!overrideEntry.texture)
            {
                overrideEntry.status = "Unsupported format or decode failed";
                return false;
            }

            overrideEntry.status = "Loaded";
            return true;
        }

        void clear_roulette_debug_override_image(RouletteDebugImageSlot slot)
        {
            auto& overrideEntry = get_override(slot);
            if (overrideEntry.texture)
                overrideEntry.texture->Release();
            overrideEntry.texture = nullptr;
            overrideEntry.enabled = false;
            overrideEntry.loadAttempted = false;
            overrideEntry.status = "Cleared";
        }

        std::vector<std::string> collect_asset_image_candidates()
        {
            namespace fs = std::filesystem;
            static constexpr std::array<const char*, 7> kSupportedExt{
                ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".dds", ".gif"
            };

            std::set<std::string> unique;
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

                    auto ext = normalize_path_for_compare(it->path().extension().string());
                    if (std::find_if(kSupportedExt.begin(), kSupportedExt.end(), [&](const char* e) { return ext == e; }) == kSupportedExt.end())
                        continue;

                    std::error_code relEc;
                    auto relative = fs::relative(it->path(), assetsRoot, relEc);
                    if (relEc)
                        continue;

                    auto candidate = std::string("assets\\") + relative.string();
                    std::replace(candidate.begin(), candidate.end(), '/', '\\');
                    unique.insert(candidate);
                }
            }

            return std::vector<std::string>(unique.begin(), unique.end());
        }

        bool matches_candidate_filter(const std::string& candidate, const std::string& query)
        {
            if (query.empty())
                return true;

            namespace fs = std::filesystem;
            auto normalizedCandidate = normalize_path_for_compare(candidate);
            auto normalizedQuery = normalize_path_for_compare(query);
            auto fileName = normalize_path_for_compare(fs::path(candidate).filename().string());

            auto hasPathHint = normalizedQuery.find('\\') != std::string::npos;
            if (hasPathHint)
                return normalizedCandidate.find(normalizedQuery) != std::string::npos;

            if (fileName.rfind(normalizedQuery, 0) == 0)
                return true;

            return normalizedCandidate.find(normalizedQuery) != std::string::npos;
        }

        void draw_asset_image_selector(RouletteDebugImageSlot slot, char* pathBuffer, std::size_t pathBufferSize)
        {
            static std::vector<std::string> s_candidates;
            static bool s_loaded = false;

            if (!s_loaded)
            {
                s_candidates = collect_asset_image_candidates();
                s_loaded = true;
            }

            if (ImGui::SmallButton((std::string("Refresh List##roulette_img_refresh_") + std::to_string(static_cast<int>(slot))).c_str()))
                s_candidates = collect_asset_image_candidates();

            if (s_candidates.empty())
            {
                ImGui::TextDisabled("No images found under assets folders.");
                return;
            }

            auto query = trim_path_input(pathBuffer ? std::string(pathBuffer) : std::string{});
            const char* preview = query.empty() ? "Select image..." : pathBuffer;

            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo((std::string("Asset image##roulette_img_combo_") + std::to_string(static_cast<int>(slot))).c_str(), preview))
            {
                for (const auto& candidate : s_candidates)
                {
                    if (!matches_candidate_filter(candidate, query))
                        continue;

                    if (ImGui::Selectable(candidate.c_str()))
                    {
                        std::snprintf(pathBuffer, pathBufferSize, "%s", candidate.c_str());
                        auto& overrideEntry = get_override(slot);
                        overrideEntry.path = pathBuffer;
                        load_roulette_debug_override_image(slot);
                        overrideEntry.enabled = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        void draw_roulette_debug_images_controls()
        {
            if (!ImGui::CollapsingHeader("Debug Images"))
                return;

            ImGui::TextDisabled("Path relativo, sotto-path o nome file (es: bg.png). Cerca anche in data\\assets\\**");
            ImGui::TextDisabled("Nota: Dragon non e' usato nel rendering roulette attuale.");

            static std::array<std::array<char, MAX_PATH>, static_cast<std::size_t>(RouletteDebugImageSlot::Count)> s_pathBuffers{};
            static bool s_buffersInitialized = false;
            if (!s_buffersInitialized)
            {
                for (std::size_t i = 0; i < s_pathBuffers.size(); ++i)
                    std::snprintf(s_pathBuffers[i].data(), s_pathBuffers[i].size(), "%s", g_rouletteDebugImageOverrides[i].path.c_str());
                s_buffersInitialized = true;
            }

            for (std::size_t i = 0; i < static_cast<std::size_t>(RouletteDebugImageSlot::Count); ++i)
            {
                auto slot = static_cast<RouletteDebugImageSlot>(i);
                if (slot == RouletteDebugImageSlot::Dragon)
                    continue;

                auto& overrideEntry = g_rouletteDebugImageOverrides[i];

                ImGui::SeparatorText(get_override_slot_label(slot));
                ImGui::Checkbox((std::string("Use##roulette_img_use_") + std::to_string(i)).c_str(), &overrideEntry.enabled);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText((std::string("Path##roulette_img_path_") + std::to_string(i)).c_str(), s_pathBuffers[i].data(), s_pathBuffers[i].size());

                draw_asset_image_selector(slot, s_pathBuffers[i].data(), s_pathBuffers[i].size());

                if (ImGui::Button((std::string("Load##roulette_img_load_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_roulette_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Reload##roulette_img_reload_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_roulette_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Clear##roulette_img_clear_") + std::to_string(i)).c_str()))
                    clear_roulette_debug_override_image(slot);

                if (!overrideEntry.status.empty())
                    ImGui::TextDisabled("Status: %s", overrideEntry.status.c_str());
            }
        }
    }

    LPDIRECT3DTEXTURE9 get_roulette_debug_override_texture(RouletteDebugImageSlot slot)
    {
        auto& overrideEntry = get_override(slot);
        if (!overrideEntry.enabled)
            return nullptr;

        if (!overrideEntry.texture && !overrideEntry.loadAttempted && !overrideEntry.path.empty())
            load_roulette_debug_override_image(slot);

        return overrideEntry.texture;
    }

    std::string build_roulette_layout_dump()
    {
        std::string dump;
        for (size_t i = 0; i < g_rouletteLayoutDebug.slotCenters.size(); ++i)
        {
            char line[128]{};
            std::snprintf(
                line,
                sizeof(line),
                "slot%zu = { %.1ff, %.1ff }\n",
                i + 1,
                g_rouletteLayoutDebug.slotCenters[i].x,
                g_rouletteLayoutDebug.slotCenters[i].y);
            dump += line;
        }

        char block[2048]{};
        std::snprintf(
            block,
            sizeof(block),
            "\nslotFrameSize = %.1ff\n"
            "iconSize = %.1ff\n"
            "listMin = { %.1ff, %.1ff }\n"
            "listSize = { %.1ff, %.1ff }\n"
            "listRowHeight = %.1ff\n"
            "listRowGap = %.1ff\n"
            "listIconSize = %.1ff\n"
            "listIconOffset = { %.1ff, %.1ff }\n"
            "listTextOffsetX = %.1ff\n"
            "listLabelY = %.1ff\n"
            "listMetaY = %.1ff\n"
            "guaranteedIconMin = { %.1ff, %.1ff }\n"
            "guaranteedIconSize = %.1ff\n"
            "guaranteedCenterX = %.1ff\n"
            "guaranteedLabelY = %.1ff\n"
            "guaranteedSpinsY = %.1ff\n"
            "guaranteedHintY = %.1ff\n"
            "guaranteedLabelWidth = %.1ff\n"
            "guaranteedHintWidth = %.1ff\n"
            "winnersStart = { %.1ff, %.1ff }\n"
            "winnersEndX = %.1ff\n"
            "winnersRowStep = %.1ff\n"
            "winnersRowHeight = %.1ff\n"
            "winnersIconOffset = { %.1ff, %.1ff }\n"
            "winnersIconSize = %.1ff\n"
            "winnersNameOffset = { %.1ff, %.1ff }\n"
            "winnersAgeRightPadding = %.1ff\n"
            "balanceCenter = { %.1ff, %.1ff }\n"
            "costCenter = { %.1ff, %.1ff }\n"
            "footerHintCenter = { %.1ff, %.1ff }\n"
            "buttonMin = { %.1ff, %.1ff }\n"
            "buttonMax = { %.1ff, %.1ff }\n",
            g_rouletteLayoutDebug.slotFrameSize,
            g_rouletteLayoutDebug.iconSize,
            g_rouletteLayoutDebug.listMin.x,
            g_rouletteLayoutDebug.listMin.y,
            g_rouletteLayoutDebug.listSize.x,
            g_rouletteLayoutDebug.listSize.y,
            g_rouletteLayoutDebug.listRowHeight,
            g_rouletteLayoutDebug.listRowGap,
            g_rouletteLayoutDebug.listIconSize,
            g_rouletteLayoutDebug.listIconOffset.x,
            g_rouletteLayoutDebug.listIconOffset.y,
            g_rouletteLayoutDebug.listTextOffsetX,
            g_rouletteLayoutDebug.listLabelY,
            g_rouletteLayoutDebug.listMetaY,
            g_rouletteLayoutDebug.guaranteedIconMin.x,
            g_rouletteLayoutDebug.guaranteedIconMin.y,
            g_rouletteLayoutDebug.guaranteedIconSize,
            g_rouletteLayoutDebug.guaranteedCenterX,
            g_rouletteLayoutDebug.guaranteedLabelY,
            g_rouletteLayoutDebug.guaranteedSpinsY,
            g_rouletteLayoutDebug.guaranteedHintY,
            g_rouletteLayoutDebug.guaranteedLabelWidth,
            g_rouletteLayoutDebug.guaranteedHintWidth,
            g_rouletteLayoutDebug.winnersStart.x,
            g_rouletteLayoutDebug.winnersStart.y,
            g_rouletteLayoutDebug.winnersEndX,
            g_rouletteLayoutDebug.winnersRowStep,
            g_rouletteLayoutDebug.winnersRowHeight,
            g_rouletteLayoutDebug.winnersIconOffset.x,
            g_rouletteLayoutDebug.winnersIconOffset.y,
            g_rouletteLayoutDebug.winnersIconSize,
            g_rouletteLayoutDebug.winnersNameOffset.x,
            g_rouletteLayoutDebug.winnersNameOffset.y,
            g_rouletteLayoutDebug.winnersAgeRightPadding,
            g_rouletteLayoutDebug.balanceCenter.x,
            g_rouletteLayoutDebug.balanceCenter.y,
            g_rouletteLayoutDebug.costCenter.x,
            g_rouletteLayoutDebug.costCenter.y,
            g_rouletteLayoutDebug.footerHintCenter.x,
            g_rouletteLayoutDebug.footerHintCenter.y,
            g_rouletteLayoutDebug.buttonMin.x,
            g_rouletteLayoutDebug.buttonMin.y,
            g_rouletteLayoutDebug.buttonMax.x,
            g_rouletteLayoutDebug.buttonMax.y);
        dump += block;

        dump += "\nDebug Images:\n";
        for (std::size_t i = 0; i < static_cast<std::size_t>(RouletteDebugImageSlot::Count); ++i)
        {
            auto slot = static_cast<RouletteDebugImageSlot>(i);
            const auto& entry = g_rouletteDebugImageOverrides[i];
            dump += std::string(get_override_slot_label(slot)) + " = { enabled=" + (entry.enabled ? "true" : "false") + ", path=\"" + entry.path + "\" }\n";
        }

        return dump;
    }

    void draw_roulette_layout_debug_panel()
    {
        if (!g_showRouletteLayoutDebugPanel)
            return;

        ImGui::SetNextWindowSize(ImVec2(560.0f, 820.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Roulette Layout Debug", &g_showRouletteLayoutDebugPanel, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        if (ImGui::CollapsingHeader("Draw Guides"))
        {
            ImGui::Checkbox("Draw Guides##roulette_layout", &g_rouletteLayoutDebug.drawGuides);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Defaults##roulette_layout"))
            {
                reset_roulette_layout_defaults();
                reset_roulette_debug_image_overrides();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Dump##roulette_layout"))
            {
                auto dump = build_roulette_layout_dump();
                ImGui::SetClipboardText(dump.c_str());
            }
        }

        draw_roulette_debug_images_controls();

        if (ImGui::CollapsingHeader("Slots"))
        {
            for (size_t i = 0; i < g_rouletteLayoutDebug.slotCenters.size(); ++i)
            {
                char label[32]{};
                std::snprintf(label, sizeof(label), "Slot %zu", i + 1);
                input_vec2(label, g_rouletteLayoutDebug.slotCenters[i]);
            }
            input_float("Slot Frame Size", g_rouletteLayoutDebug.slotFrameSize);
            input_float("Icon Size", g_rouletteLayoutDebug.iconSize);
        }

        if (ImGui::CollapsingHeader("Reward List"))
        {
            input_vec2("List Min", g_rouletteLayoutDebug.listMin);
            input_vec2("List Size", g_rouletteLayoutDebug.listSize);
            input_float("Row Height", g_rouletteLayoutDebug.listRowHeight);
            input_float("Row Gap", g_rouletteLayoutDebug.listRowGap);
            input_float("List Icon Size", g_rouletteLayoutDebug.listIconSize);
            input_vec2("List Icon Offset", g_rouletteLayoutDebug.listIconOffset);
            input_float("List Text X", g_rouletteLayoutDebug.listTextOffsetX);
            input_float("List Label Y", g_rouletteLayoutDebug.listLabelY);
            input_float("List Meta Y", g_rouletteLayoutDebug.listMetaY);
        }

        if (ImGui::CollapsingHeader("Guaranteed"))
        {
            input_vec2("Guaranteed Icon Min", g_rouletteLayoutDebug.guaranteedIconMin);
            input_float("Guaranteed Icon Size", g_rouletteLayoutDebug.guaranteedIconSize);
            input_float("Guaranteed Center X", g_rouletteLayoutDebug.guaranteedCenterX);
            input_float("Guaranteed Label Y", g_rouletteLayoutDebug.guaranteedLabelY);
            input_float("Guaranteed Spins Y", g_rouletteLayoutDebug.guaranteedSpinsY);
            input_float("Guaranteed Hint Y", g_rouletteLayoutDebug.guaranteedHintY);
            input_float("Guaranteed Label Width", g_rouletteLayoutDebug.guaranteedLabelWidth);
            input_float("Guaranteed Hint Width", g_rouletteLayoutDebug.guaranteedHintWidth);
        }

        if (ImGui::CollapsingHeader("Winners"))
        {
            input_vec2("Winners Start", g_rouletteLayoutDebug.winnersStart);
            input_float("Winners End X", g_rouletteLayoutDebug.winnersEndX);
            input_float("Winners Row Step", g_rouletteLayoutDebug.winnersRowStep);
            input_float("Winners Row Height", g_rouletteLayoutDebug.winnersRowHeight);
            input_vec2("Winners Icon Offset", g_rouletteLayoutDebug.winnersIconOffset);
            input_float("Winners Icon Size", g_rouletteLayoutDebug.winnersIconSize);
            input_vec2("Winners Name Offset", g_rouletteLayoutDebug.winnersNameOffset);
            input_float("Winners Age Right Padding", g_rouletteLayoutDebug.winnersAgeRightPadding);
        }

        if (ImGui::CollapsingHeader("Footer + Button"))
        {
            input_vec2("Balance Center", g_rouletteLayoutDebug.balanceCenter);
            input_vec2("Cost Center", g_rouletteLayoutDebug.costCenter);
            input_vec2("Footer Hint Center", g_rouletteLayoutDebug.footerHintCenter);
            input_vec2("Button Min", g_rouletteLayoutDebug.buttonMin);
            input_vec2("Button Max", g_rouletteLayoutDebug.buttonMax);
        }

        ImGui::SeparatorText("Current Layout Dump");
        auto dump = build_roulette_layout_dump();
        std::vector<char> dumpBuffer(dump.begin(), dump.end());
        dumpBuffer.push_back('\0');

        if (ImGui::SmallButton("Copy Dump##roulette_layout_footer"))
            ImGui::SetClipboardText(dump.c_str());

        ImGui::BeginChild("##roulette_layout_dump_child", ImVec2(0.0f, 210.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::InputTextMultiline(
            "##roulette_layout_dump_text",
            dumpBuffer.data(),
            dumpBuffer.size(),
            ImVec2(-1.0f, -1.0f),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::EndChild();

        ImGui::End();
    }
}

