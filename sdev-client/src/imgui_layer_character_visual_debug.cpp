#include "include/imgui_layer_internal.h"
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/ItemInfo.h"
#include <filesystem>
#include <set>

namespace imgui_layer
{
    namespace
    {
        constexpr float kPanelWidth = 560.0f;
        constexpr float kPanelHeight = 860.0f;
        constexpr DWORD kAnchorTimeoutMs = 250;
        constexpr uint8_t kTextTitleReqIg = 1;
        constexpr uint8_t kPngTitleReqIg = 2;
        constexpr uint8_t kGifTitleReqIg = 3;

        void input_vec2(const char* label, ImVec2& value)
        {
            float data[2]{ value.x, value.y };
            if (ImGui::InputFloat2(label, data, "%.1f"))
                value = ImVec2(data[0], data[1]);
        }

        RouletteDebugImageOverride& get_override(CharacterDebugImageSlot slot)
        {
            return g_characterDebugImageOverrides[static_cast<std::size_t>(slot)];
        }

        const char* get_override_slot_label(CharacterDebugImageSlot slot)
        {
            switch (slot)
            {
            case CharacterDebugImageSlot::TitleBackground: return "Title Background";
            case CharacterDebugImageSlot::TitleFrame: return "Title Frame";
            case CharacterDebugImageSlot::NameBackground: return "Name Background";
            case CharacterDebugImageSlot::NameFrame: return "Name Frame";
            case CharacterDebugImageSlot::HpBarBackground: return "HP Bar Background";
            case CharacterDebugImageSlot::HpBarFill: return "HP Bar Fill";
            case CharacterDebugImageSlot::HpBarBorder: return "HP Bar Border";
            default: return "Unknown";
            }
        }

        void reset_character_debug_images()
        {
            for (auto& overrideEntry : g_characterDebugImageOverrides)
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
                overrideEntry.path.clear();
                overrideEntry.status.clear();
            }
        }

        void reset_character_layout_defaults()
        {
            g_characterLayoutDebug = CharacterLayoutDebugState{};
            reset_character_debug_images();
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

        void write_advanced_setting(const char* key, bool enabled)
        {
            if (!g_var)
                return;

            WritePrivateProfileStringA("ADVANCED", key, enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
        }

        void apply_titles_setting(bool enabled)
        {
            g_showTitles = enabled;
            write_advanced_setting("TITLES", enabled);
        }

        void apply_name_colors_setting(bool enabled)
        {
            g_showNameColors = enabled;
            write_advanced_setting("COLOUR", enabled);
        }

        void apply_effects_setting(bool enabled)
        {
            g_showEffects = enabled;
            g_showMobEffects = enabled;
            write_advanced_setting("EFFECTS", enabled);
            if (!enabled)
                name_color::disable_enchant_eft();
        }

        void apply_pets_setting(bool enabled)
        {
            g_showPets = enabled;
            g_showMobEffects = enabled;
            write_advanced_setting("PETS", enabled);
        }

        void apply_wings_setting(bool enabled)
        {
            g_showWings = enabled;
            write_advanced_setting("WINGS", enabled);
        }

        void apply_costumes_setting(bool enabled)
        {
            g_showCostumes = enabled;
            write_advanced_setting("COSTUMES", enabled);
        }

        CCharacter* get_local_user()
        {
            return g_pWorldMgr ? g_pWorldMgr->user : nullptr;
        }

        bool is_title_cloak(const ItemInfo* itemInfo)
        {
            if (!itemInfo)
                return false;

            return (itemInfo->type == std::to_underlying(RealType::Cloak)
                || itemInfo->type == std::to_underlying(RealType::FuryCloak))
                && (itemInfo->reqIg == kTextTitleReqIg
                    || itemInfo->reqIg == kPngTitleReqIg
                    || itemInfo->reqIg == kGifTitleReqIg);
        }

        const char* title_mode_label(const ItemInfo* itemInfo)
        {
            if (!itemInfo)
                return "None";
            if (!is_title_cloak(itemInfo))
                return "Not a title cloak";

            switch (itemInfo->reqIg)
            {
            case kTextTitleReqIg:
                return "Text";
            case kPngTitleReqIg:
                return "PNG";
            case kGifTitleReqIg:
                return "GIF";
            default:
                return "Unknown";
            }
        }

        bool is_anchor_valid()
        {
            if (!g_characterLayoutAnchorValid)
                return false;

            auto now = GetTickCount();
            return static_cast<int32_t>(now - g_characterLayoutAnchorTick) <= static_cast<int32_t>(kAnchorTimeoutMs);
        }

        void add_cross(ImDrawList* drawList, const ImVec2& center, float radius, ImU32 color)
        {
            if (!drawList)
                return;

            drawList->AddLine(ImVec2(center.x - radius, center.y), ImVec2(center.x + radius, center.y), color, 1.2f);
            drawList->AddLine(ImVec2(center.x, center.y - radius), ImVec2(center.x, center.y + radius), color, 1.2f);
        }

        void add_centered_rect(ImDrawList* drawList, const ImVec2& center, const ImVec2& size, ImU32 color)
        {
            if (!drawList)
                return;

            auto half = ImVec2(size.x * 0.5f, size.y * 0.5f);
            auto min = ImVec2(center.x - half.x, center.y - half.y);
            auto max = ImVec2(center.x + half.x, center.y + half.y);
            drawList->AddRect(min, max, color, 3.0f, 0, 1.4f);
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
            return {};
        }

        bool file_exists(const std::string& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return static_cast<bool>(stream);
        }

        std::string find_image_in_assets_recursive(const std::string& inputPath)
        {
            return imgui_layer::resolve_asset_image_path(inputPath);
        }

        std::string resolve_override_image_path(const std::string& inputPath)
        {
            return imgui_layer::resolve_asset_image_path(inputPath);
        }

        LPDIRECT3DTEXTURE9 create_debug_texture_from_file_data(const std::string& path, const void* data, UINT dataSize)
        {
            auto device = g_device ? g_device : (g_var ? g_var->camera.device : nullptr);
            if (!device || !data || dataSize == 0)
                return nullptr;

            auto extension = normalize_path_for_compare(std::filesystem::path(path).extension().string());
            if (extension == ".dds")
                return create_texture_from_dds_memory(device, data, dataSize);

            return create_texture_from_image_memory(device, data, dataSize);
        }

        bool load_character_debug_override_image(CharacterDebugImageSlot slot)
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

            overrideEntry.texture = create_debug_texture_from_file_data(absolutePath, fileData.data(), static_cast<UINT>(fileData.size()));
            if (!overrideEntry.texture)
            {
                overrideEntry.status = "Unsupported format or decode failed";
                return false;
            }

            overrideEntry.resolvedSource = resolvedSource;
            overrideEntry.status = "Loaded";
            return true;
        }

        void clear_character_debug_override_image(CharacterDebugImageSlot slot)
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
            overrideEntry.path.clear();
            overrideEntry.status = "Cleared";
        }

        LPDIRECT3DTEXTURE9 get_character_debug_override_texture(CharacterDebugImageSlot slot)
        {
            auto& overrideEntry = get_override(slot);
            if (!overrideEntry.enabled)
                return nullptr;

            if (!overrideEntry.texture && overrideEntry.frames.empty() && !overrideEntry.loadAttempted && !overrideEntry.path.empty())
                load_character_debug_override_image(slot);

            if (!overrideEntry.frames.empty())
                return get_animated_texture_frame(overrideEntry.frames);

            return overrideEntry.texture;
        }

        std::vector<std::string> collect_asset_image_candidates()
        {
            return imgui_layer::collect_asset_image_candidates(false);
        }

        bool matches_candidate_filter(const std::string& candidate, const std::string& query)
        {
            return imgui_layer::asset_image_candidate_matches_filter(candidate, query);
        }

        void draw_asset_image_selector(CharacterDebugImageSlot slot, char* pathBuffer, std::size_t pathBufferSize, char* filterBuffer, std::size_t filterBufferSize)
        {
            auto browserId = std::string("character_asset_browser_") + std::to_string(static_cast<int>(slot));
            if (imgui_layer::draw_asset_image_browser(browserId.c_str(), pathBuffer, pathBufferSize, filterBuffer, filterBufferSize, 140.0f))
            {
                auto& overrideEntry = get_override(slot);
                overrideEntry.path = pathBuffer;
                load_character_debug_override_image(slot);
                overrideEntry.enabled = true;
            }
        }

        void draw_texture_rect(ImDrawList* drawList, LPDIRECT3DTEXTURE9 texture, const ImVec2& min, const ImVec2& max, ImU32 tint = IM_COL32_WHITE)
        {
            if (!drawList || !texture)
                return;

            drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        }

        void draw_texture_rect_fill_x(ImDrawList* drawList, LPDIRECT3DTEXTURE9 texture, const ImVec2& min, const ImVec2& max, float ratio, ImU32 tint = IM_COL32_WHITE)
        {
            if (!drawList || !texture)
                return;
            if (ratio <= 0.0f)
                return;

            ratio = std::clamp(ratio, 0.0f, 1.0f);
            auto fillMax = ImVec2(min.x + (max.x - min.x) * ratio, max.y);
            drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, fillMax, ImVec2(0.0f, 0.0f), ImVec2(ratio, 1.0f), tint);
        }

        void draw_character_debug_images_controls()
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Debug Images"))
                return;

            ImGui::TextDisabled("Path relativo, sotto-path o nome file. Ogni slot ha un browser ricorsivo degli `Assets` del client.");

            static std::array<std::array<char, MAX_PATH>, static_cast<std::size_t>(CharacterDebugImageSlot::Count)> s_pathBuffers{};
            static std::array<std::array<char, 96>, static_cast<std::size_t>(CharacterDebugImageSlot::Count)> s_filterBuffers{};
            static bool s_buffersInitialized = false;
            if (!s_buffersInitialized)
            {
                for (std::size_t i = 0; i < s_pathBuffers.size(); ++i)
                    std::snprintf(s_pathBuffers[i].data(), s_pathBuffers[i].size(), "%s", g_characterDebugImageOverrides[i].path.c_str());
                s_buffersInitialized = true;
            }

            for (std::size_t i = 0; i < static_cast<std::size_t>(CharacterDebugImageSlot::Count); ++i)
            {
                auto slot = static_cast<CharacterDebugImageSlot>(i);
                auto& overrideEntry = g_characterDebugImageOverrides[i];

                ImGui::SeparatorText(get_override_slot_label(slot));
                ImGui::Checkbox((std::string("Use##character_img_use_") + std::to_string(i)).c_str(), &overrideEntry.enabled);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText((std::string("Path##character_img_path_") + std::to_string(i)).c_str(), s_pathBuffers[i].data(), s_pathBuffers[i].size());

                if (ImGui::TreeNode((std::string("Asset Browser##character_img_browser_") + std::to_string(i)).c_str()))
                {
                    draw_asset_image_selector(slot, s_pathBuffers[i].data(), s_pathBuffers[i].size(), s_filterBuffers[i].data(), s_filterBuffers[i].size());
                    ImGui::TreePop();
                }

                if (ImGui::Button((std::string("Load##character_img_load_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_character_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Reload##character_img_reload_") + std::to_string(i)).c_str()))
                {
                    overrideEntry.path = s_pathBuffers[i].data();
                    load_character_debug_override_image(slot);
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Clear##character_img_clear_") + std::to_string(i)).c_str()))
                {
                    clear_character_debug_override_image(slot);
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

        void draw_local_character_guides()
        {
            if (!g_showCharacterLayoutDebugPanel)
                return;
            if (!g_characterLayoutDebug.drawGuides && !g_characterLayoutDebug.drawHpBar)
                return;
            if (!is_anchor_valid())
                return;

            auto* user = get_local_user();
            if (!user)
                return;

            auto drawList = ImGui::GetForegroundDrawList();
            auto anchor = g_characterLayoutAnchorScreen;

            auto titleCenter = ImVec2(anchor.x + g_characterLayoutDebug.titleCenterOffset.x, anchor.y + g_characterLayoutDebug.titleCenterOffset.y);
            auto titleHalf = ImVec2(g_characterLayoutDebug.titleBoxSize.x * 0.5f, g_characterLayoutDebug.titleBoxSize.y * 0.5f);
            auto titleMin = ImVec2(titleCenter.x - titleHalf.x, titleCenter.y - titleHalf.y);
            auto titleMax = ImVec2(titleCenter.x + titleHalf.x, titleCenter.y + titleHalf.y);

            auto nameCenter = ImVec2(anchor.x + g_characterLayoutDebug.nameCenterOffset.x, anchor.y + g_characterLayoutDebug.nameCenterOffset.y);
            auto nameHalf = ImVec2(g_characterLayoutDebug.nameBoxSize.x * 0.5f, g_characterLayoutDebug.nameBoxSize.y * 0.5f);
            auto nameMin = ImVec2(nameCenter.x - nameHalf.x, nameCenter.y - nameHalf.y);
            auto nameMax = ImVec2(nameCenter.x + nameHalf.x, nameCenter.y + nameHalf.y);

            draw_texture_rect(drawList, get_character_debug_override_texture(CharacterDebugImageSlot::TitleBackground), titleMin, titleMax);
            draw_texture_rect(drawList, get_character_debug_override_texture(CharacterDebugImageSlot::TitleFrame), titleMin, titleMax);
            draw_texture_rect(drawList, get_character_debug_override_texture(CharacterDebugImageSlot::NameBackground), nameMin, nameMax);
            draw_texture_rect(drawList, get_character_debug_override_texture(CharacterDebugImageSlot::NameFrame), nameMin, nameMax);

            if (g_characterLayoutDebug.drawGuides)
            {
                if (g_characterLayoutDebug.drawAnchorCross)
                    add_cross(drawList, anchor, 12.0f, IM_COL32(255, 220, 90, 255));

                if (g_characterLayoutDebug.drawTitleGuide)
                {
                    add_centered_rect(drawList, titleCenter, g_characterLayoutDebug.titleBoxSize, IM_COL32(76, 214, 255, 220));
                    drawList->AddText(ImVec2(titleCenter.x - 16.0f, titleCenter.y - g_characterLayoutDebug.titleBoxSize.y * 0.5f - 18.0f), IM_COL32(76, 214, 255, 255), "Title");
                }

                if (g_characterLayoutDebug.drawNameGuide)
                {
                    add_centered_rect(drawList, nameCenter, g_characterLayoutDebug.nameBoxSize, IM_COL32(155, 255, 140, 220));
                    drawList->AddText(ImVec2(nameCenter.x - 18.0f, nameCenter.y - g_characterLayoutDebug.nameBoxSize.y * 0.5f - 18.0f), IM_COL32(155, 255, 140, 255), "Name");
                }
            }

            if (!g_characterLayoutDebug.drawHpBar)
                return;

            auto barSize = g_characterLayoutDebug.hpBarSize;
            barSize.x = std::max(barSize.x, 16.0f);
            barSize.y = std::max(barSize.y, 4.0f);

            auto barMin = ImVec2(anchor.x + g_characterLayoutDebug.hpBarOffset.x, anchor.y + g_characterLayoutDebug.hpBarOffset.y);
            auto barMax = ImVec2(barMin.x + barSize.x, barMin.y + barSize.y);
            auto ratio = user->maxHealth > 0 ? std::clamp(static_cast<float>(user->health) / static_cast<float>(user->maxHealth), 0.0f, 1.0f) : 0.0f;
            auto fillTexture = get_character_debug_override_texture(CharacterDebugImageSlot::HpBarFill);
            auto bgTexture = get_character_debug_override_texture(CharacterDebugImageSlot::HpBarBackground);
            auto borderTexture = get_character_debug_override_texture(CharacterDebugImageSlot::HpBarBorder);

            if (bgTexture)
                draw_texture_rect(drawList, bgTexture, barMin, barMax);
            else
                drawList->AddRectFilled(barMin, barMax, IM_COL32(22, 18, 18, 220), 3.0f);

            if (fillTexture)
                draw_texture_rect_fill_x(drawList, fillTexture, barMin, barMax, ratio);
            else
            {
                auto fillMax = ImVec2(barMin.x + (barSize.x - 2.0f) * ratio, barMax.y - 1.0f);
                drawList->AddRectFilled(ImVec2(barMin.x + 1.0f, barMin.y + 1.0f), fillMax, IM_COL32(210, 55, 55, 230), 2.0f);
            }

            if (borderTexture)
                draw_texture_rect(drawList, borderTexture, barMin, barMax);
            else
                drawList->AddRect(barMin, barMax, IM_COL32(255, 255, 255, 200), 3.0f, 0, 1.2f);

            if (g_characterLayoutDebug.drawGuides)
                drawList->AddText(ImVec2(barMin.x, barMin.y - 18.0f), IM_COL32(255, 170, 170, 255), "HP Bar");

            if (!g_characterLayoutDebug.drawHpText)
                return;

            char hpText[64]{};
            std::snprintf(hpText, sizeof(hpText), "%d / %d", std::max(0, user->health), std::max(0, user->maxHealth));
            auto textSize = ImGui::CalcTextSize(hpText);
            auto textPos = ImVec2(barMin.x + (barSize.x - textSize.x) * 0.5f, barMin.y + g_characterLayoutDebug.hpTextOffsetY);
            drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), hpText);
        }

        void draw_inspector_section(CCharacter* user)
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Character Inspector"))
                return;

            if (!user)
            {
                ImGui::TextDisabled("Local character is not ready.");
                return;
            }

            ImGui::Text("Name: %s", user->charName.data());
            ImGui::Text("ID: %u", user->id);
            ImGui::Text("HP/MP/SP: %d / %d   %d / %d   %d / %d", user->health, user->maxHealth, user->mana, user->maxMana, user->stamina, user->maxStamina);
            ImGui::Text("Action: %d / %d", static_cast<int>(user->actionType1), static_cast<int>(user->actionType2));
            ImGui::Text("Motion: %d / %d", user->motionType1, user->motionType2);
            ImGui::Text("Target ID: %u", user->targetId);
            ImGui::Text("Costume: %s", user->enableCostume ? "enabled" : "disabled");
            ImGui::Text("Vehicle model: %u", static_cast<unsigned>(user->vehicleModel));
            ImGui::Text("Has pet / wings / vehicle: %s / %s / %s", user->pet ? "yes" : "no", user->wings ? "yes" : "no", user->vehicle ? "yes" : "no");
            ImGui::Text("Anchor: %s", is_anchor_valid() ? "ready" : "waiting");
            if (is_anchor_valid())
                ImGui::Text("Anchor Screen: %.1f , %.1f", g_characterLayoutAnchorScreen.x, g_characterLayoutAnchorScreen.y);
        }

        void draw_visual_toggles_section()
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Visual Toggles"))
                return;

            auto showTitles = g_showTitles != 0;
            if (ImGui::Checkbox("Show Titles", &showTitles))
                apply_titles_setting(showTitles);

            auto showNameColors = g_showNameColors != 0;
            if (ImGui::Checkbox("Show Name Colors", &showNameColors))
                apply_name_colors_setting(showNameColors);

            auto showEffects = g_showEffects != 0;
            if (ImGui::Checkbox("Show Effects", &showEffects))
                apply_effects_setting(showEffects);

            auto showPets = g_showPets != 0;
            if (ImGui::Checkbox("Show Pets", &showPets))
                apply_pets_setting(showPets);

            auto showWings = g_showWings != 0;
            if (ImGui::Checkbox("Show Wings", &showWings))
                apply_wings_setting(showWings);

            auto showCostumes = g_showCostumes != 0;
            if (ImGui::Checkbox("Show Costumes", &showCostumes))
                apply_costumes_setting(showCostumes);
        }

        void draw_title_section(CCharacter* user)
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Title"))
                return;

            if (!user)
            {
                ImGui::TextDisabled("Local character is not ready.");
                return;
            }

            auto cloakType = user->equipment.type[ItemSlot::Cloak];
            auto cloakTypeId = user->equipment.typeId[ItemSlot::Cloak];
            auto helmetType = user->equipment.type[ItemSlot::Helmet];
            auto helmetTypeId = user->equipment.typeId[ItemSlot::Helmet];
            auto* cloakItem = CDataFile::GetItemInfo(cloakType, cloakTypeId);
            auto* helmetItem = CDataFile::GetItemInfo(helmetType, helmetTypeId);

            ImGui::Text("Cloak: %u / %u", static_cast<unsigned>(cloakType), static_cast<unsigned>(cloakTypeId));
            ImGui::Text("Title mode: %s", title_mode_label(cloakItem));
            if (cloakItem && cloakItem->name)
                ImGui::TextWrapped("Cloak item: %s", cloakItem->name);

            if (cloakItem && is_title_cloak(cloakItem))
            {
                ImGui::Text("Title range/index: %u", static_cast<unsigned>(cloakItem->range));
                if (cloakItem->reqIg == kPngTitleReqIg)
                    ImGui::Text("Expected asset: assets\\titles\\title%u.png", static_cast<unsigned>(cloakItem->range));
                else if (cloakItem->reqIg == kGifTitleReqIg)
                    ImGui::Text("Expected asset: assets\\titlesanimated\\title%u.gif", static_cast<unsigned>(cloakItem->range));
            }

            ImGui::Separator();
            ImGui::Text("Helmet: %u / %u", static_cast<unsigned>(helmetType), static_cast<unsigned>(helmetTypeId));
            if (helmetItem && helmetItem->name)
                ImGui::TextWrapped("Helmet item: %s", helmetItem->name);
            if (helmetItem)
                ImGui::Text("Helmet range/color index: %u", static_cast<unsigned>(helmetItem->range));
        }

        void draw_custom_effect_section()
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Custom Effect"))
                return;

            static std::array<char, 128> s_effectName{};
            static bool s_loop = false;
            static int s_loopIntervalMs = 1200;

            if (s_effectName[0] == '\0')
            {
                auto current = get_custom_eft_name_from_ui();
                if (current && current[0])
                    std::snprintf(s_effectName.data(), s_effectName.size(), "%s", current);
            }

            s_loop = is_custom_eft_loop_enabled_from_ui();
            s_loopIntervalMs = static_cast<int>(get_custom_eft_loop_interval_from_ui());
            s_loopIntervalMs = std::clamp(s_loopIntervalMs, 100, 10000);

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("Effect File", s_effectName.data(), s_effectName.size());
            ImGui::Checkbox("Loop", &s_loop);
            ImGui::InputInt("Loop Interval (ms)", &s_loopIntervalMs, 100, 500);
            s_loopIntervalMs = std::clamp(s_loopIntervalMs, 100, 10000);

            if (ImGui::Button("Play##character_eft"))
                play_custom_eft_from_ui(s_effectName.data(), s_loop, static_cast<unsigned>(s_loopIntervalMs));
            ImGui::SameLine();
            if (ImGui::Button("Stop##character_eft"))
                stop_custom_eft_from_ui();

            auto current = get_custom_eft_name_from_ui();
            ImGui::Text("Current: %s", current && current[0] ? current : "<none>");
        }

        void draw_overlay_layout_section()
        {
            ImGui::SetNextItemOpen(false, ImGuiCond_Appearing);
            if (!ImGui::CollapsingHeader("Overlay Layout"))
                return;

            ImGui::Checkbox("Draw Anchor Cross", &g_characterLayoutDebug.drawAnchorCross);
            ImGui::Checkbox("Draw Title Guide", &g_characterLayoutDebug.drawTitleGuide);
            ImGui::Checkbox("Draw Name Guide", &g_characterLayoutDebug.drawNameGuide);
            ImGui::Checkbox("Show HP Bar Overlay", &g_characterLayoutDebug.drawHpBar);
            ImGui::Checkbox("Show HP Text", &g_characterLayoutDebug.drawHpText);

            input_vec2("Title Center Offset", g_characterLayoutDebug.titleCenterOffset);
            input_vec2("Title Box Size", g_characterLayoutDebug.titleBoxSize);
            input_vec2("Name Center Offset", g_characterLayoutDebug.nameCenterOffset);
            input_vec2("Name Box Size", g_characterLayoutDebug.nameBoxSize);
            input_vec2("HP Bar Offset", g_characterLayoutDebug.hpBarOffset);
            input_vec2("HP Bar Size", g_characterLayoutDebug.hpBarSize);
            ImGui::InputFloat("HP Text Offset Y", &g_characterLayoutDebug.hpTextOffsetY, 1.0f, 5.0f, "%.1f");

            g_characterLayoutDebug.titleBoxSize.x = std::max(g_characterLayoutDebug.titleBoxSize.x, 8.0f);
            g_characterLayoutDebug.titleBoxSize.y = std::max(g_characterLayoutDebug.titleBoxSize.y, 8.0f);
            g_characterLayoutDebug.nameBoxSize.x = std::max(g_characterLayoutDebug.nameBoxSize.x, 8.0f);
            g_characterLayoutDebug.nameBoxSize.y = std::max(g_characterLayoutDebug.nameBoxSize.y, 8.0f);
            g_characterLayoutDebug.hpBarSize.x = std::max(g_characterLayoutDebug.hpBarSize.x, 16.0f);
            g_characterLayoutDebug.hpBarSize.y = std::max(g_characterLayoutDebug.hpBarSize.y, 4.0f);
        }
    }

    void capture_character_layout_anchor(CCharacter* user, float x, float y, float extrusion)
    {
        auto* localUser = get_local_user();
        if (!user || !localUser || user != localUser)
            return;

        g_characterLayoutAnchorValid = true;
        g_characterLayoutAnchorScreen = ImVec2(x, y);
        g_characterLayoutAnchorDepth = extrusion;
        g_characterLayoutAnchorUserId = user->id;
        g_characterLayoutAnchorTick = GetTickCount();
    }

    std::string build_character_layout_dump()
    {
        char block[1024]{};
        std::snprintf(
            block,
            sizeof(block),
            "drawGuides = %s\n"
            "drawAnchorCross = %s\n"
            "drawTitleGuide = %s\n"
            "drawNameGuide = %s\n"
            "drawHpBar = %s\n"
            "drawHpText = %s\n"
            "titleCenterOffset = { %.1ff, %.1ff }\n"
            "titleBoxSize = { %.1ff, %.1ff }\n"
            "nameCenterOffset = { %.1ff, %.1ff }\n"
            "nameBoxSize = { %.1ff, %.1ff }\n"
            "hpBarOffset = { %.1ff, %.1ff }\n"
            "hpBarSize = { %.1ff, %.1ff }\n"
            "hpTextOffsetY = %.1ff\n",
            g_characterLayoutDebug.drawGuides ? "true" : "false",
            g_characterLayoutDebug.drawAnchorCross ? "true" : "false",
            g_characterLayoutDebug.drawTitleGuide ? "true" : "false",
            g_characterLayoutDebug.drawNameGuide ? "true" : "false",
            g_characterLayoutDebug.drawHpBar ? "true" : "false",
            g_characterLayoutDebug.drawHpText ? "true" : "false",
            g_characterLayoutDebug.titleCenterOffset.x,
            g_characterLayoutDebug.titleCenterOffset.y,
            g_characterLayoutDebug.titleBoxSize.x,
            g_characterLayoutDebug.titleBoxSize.y,
            g_characterLayoutDebug.nameCenterOffset.x,
            g_characterLayoutDebug.nameCenterOffset.y,
            g_characterLayoutDebug.nameBoxSize.x,
            g_characterLayoutDebug.nameBoxSize.y,
            g_characterLayoutDebug.hpBarOffset.x,
            g_characterLayoutDebug.hpBarOffset.y,
            g_characterLayoutDebug.hpBarSize.x,
            g_characterLayoutDebug.hpBarSize.y,
            g_characterLayoutDebug.hpTextOffsetY);

        std::string dump = block;
        dump += "\nVisual Toggles:\n";
        dump += std::string("titles = ") + (g_showTitles ? "true" : "false") + "\n";
        dump += std::string("nameColors = ") + (g_showNameColors ? "true" : "false") + "\n";
        dump += std::string("effects = ") + (g_showEffects ? "true" : "false") + "\n";
        dump += std::string("pets = ") + (g_showPets ? "true" : "false") + "\n";
        dump += std::string("wings = ") + (g_showWings ? "true" : "false") + "\n";
        dump += std::string("costumes = ") + (g_showCostumes ? "true" : "false") + "\n";

        auto currentEffect = get_custom_eft_name_from_ui();
        dump += "\nCustom Effect:\n";
        dump += std::string("file = \"") + (currentEffect ? currentEffect : "") + "\"\n";
        dump += std::string("loop = ") + (is_custom_eft_loop_enabled_from_ui() ? "true" : "false") + "\n";
        dump += "loopIntervalMs = " + std::to_string(get_custom_eft_loop_interval_from_ui()) + "\n";

        dump += "\nDebug Images:\n";
        for (std::size_t i = 0; i < static_cast<std::size_t>(CharacterDebugImageSlot::Count); ++i)
        {
            auto slot = static_cast<CharacterDebugImageSlot>(i);
            const auto& entry = g_characterDebugImageOverrides[i];
            dump += std::string(get_override_slot_label(slot)) + " = { enabled=" + (entry.enabled ? "true" : "false") + ", path=\"" + entry.path + "\" }\n";
        }

        return dump;
    }

    void draw_character_layout_debug_panel()
    {
        if (!g_showCharacterLayoutDebugPanel)
            return;

        ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kPanelHeight), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Character Visual Debug", &g_showCharacterLayoutDebugPanel, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            draw_local_character_guides();
            return;
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
        if (ImGui::CollapsingHeader("Draw Guides", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Draw Guides##character_layout", &g_characterLayoutDebug.drawGuides);
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Defaults##character_layout"))
                reset_character_layout_defaults();
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Dump##character_layout"))
            {
                auto dump = build_character_layout_dump();
                ImGui::SetClipboardText(dump.c_str());
            }

            ImGui::TextDisabled("Le guide usano l'anchor del render sopra la testa del personaggio locale.");
            if (!is_anchor_valid())
                ImGui::TextDisabled("Anchor non disponibile: muovi o rendi visibile il personaggio in scena.");
        }

        auto* user = get_local_user();
        draw_inspector_section(user);
        draw_visual_toggles_section();
        draw_title_section(user);
        draw_custom_effect_section();
        draw_overlay_layout_section();
        draw_character_debug_images_controls();

        ImGui::SeparatorText("Current Layout Dump");
        auto dump = build_character_layout_dump();
        std::vector<char> dumpBuffer(dump.begin(), dump.end());
        dumpBuffer.push_back('\0');

        if (ImGui::SmallButton("Copy Dump##character_layout_footer"))
            ImGui::SetClipboardText(dump.c_str());

        ImGui::BeginChild("##character_layout_dump_child", ImVec2(0.0f, 180.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::InputTextMultiline(
            "##character_layout_dump_text",
            dumpBuffer.data(),
            dumpBuffer.size(),
            ImVec2(-1.0f, -1.0f),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::EndChild();

        ImGui::End();
        draw_local_character_guides();
    }
}
