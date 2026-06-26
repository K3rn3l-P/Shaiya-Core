#include <shaiya/include/network/game/outgoing/0800.h>
#include "include/imgui_layer_internal.h"
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/CNetwork.h"
#include "include/shaiya/ItemInfo.h"

namespace imgui_layer {

    namespace
    {
        constexpr float kPi = 3.1415926535f;

        bool drag_ref_point(const char* id, ImVec2& refPoint, const ImVec2& origin, float scale, float radius = 7.0f)
        {
            auto screenPoint = ImVec2(origin.x + refPoint.x * scale, origin.y + refPoint.y * scale);
            auto handleSize = ImVec2(radius * 2.0f, radius * 2.0f);
            ImGui::SetCursorScreenPos(ImVec2(screenPoint.x - radius, screenPoint.y - radius));
            ImGui::InvisibleButton(id, handleSize);
            auto active = ImGui::IsItemActive();
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                auto mouse = ImGui::GetIO().MousePos;
                refPoint.x = (mouse.x - origin.x) / scale;
                refPoint.y = (mouse.y - origin.y) / scale;
                return true;
            }
            return false;
        }

        bool drag_ref_rect(const char* moveId, const char* resizeId, ImVec2& refMin, ImVec2& refSize, const ImVec2& origin, float scale)
        {
            auto changed = false;
            if (drag_ref_point(moveId, refMin, origin, scale))
                changed = true;

            auto refMax = ImVec2(refMin.x + refSize.x, refMin.y + refSize.y);
            if (drag_ref_point(resizeId, refMax, origin, scale))
            {
                refSize.x = std::max(8.0f, refMax.x - refMin.x);
                refSize.y = std::max(8.0f, refMax.y - refMin.y);
                changed = true;
            }
            return changed;
        }

        float wrap_float(float value, float maxValue)
        {
            if (maxValue <= 0.0f)
                return 0.0f;

            value = std::fmod(value, maxValue);
            if (value < 0.0f)
                value += maxValue;
            return value;
        }

        float ease_out_cubic(float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            auto inv = 1.0f - t;
            return 1.0f - inv * inv * inv;
        }

        float compute_spin_progress(uint32_t now, uint8_t itemCount)
        {
            auto target = static_cast<float>(roulette_event::rewardIndex);
            if (!roulette_event::spinActive || roulette_event::spinDurationMs == 0 || itemCount == 0)
                return target;

            auto t = static_cast<float>(now - roulette_event::spinStartTick) / static_cast<float>(roulette_event::spinDurationMs);
            t = std::clamp(t, 0.0f, 1.0f);

            auto totalTravel = static_cast<float>(itemCount) * 5.0f + target;
            auto pattern = (roulette_event::spinSerial * 1103515245u + 12345u) % 3u;
            auto requestedFinalSteps = pattern == 0 ? 3.0f : (pattern == 1 ? 5.0f : 7.0f);
            auto finalStepCount = std::min(requestedFinalSteps, std::max(1.0f, totalTravel - 1.0f));
            auto cruiseTravel = std::max(0.0f, totalTravel - finalStepCount);

            if (t < 0.72f)
            {
                auto localT = t / 0.72f;
                return cruiseTravel * ease_out_cubic(localT);
            }

            std::array<float, 5> stepProgress{
                cruiseTravel,
                std::max(cruiseTravel, totalTravel - 2.0f),
                std::max(cruiseTravel, totalTravel - 1.0f),
                totalTravel,
                totalTravel
            };
            std::array<float, 5> stepTimes{ 0.72f, 0.82f, 0.90f, 0.97f, 1.0f };

            for (size_t i = 0; i + 1 < stepTimes.size(); ++i)
            {
                if (t <= stepTimes[i + 1])
                {
                    auto segmentT = (t - stepTimes[i]) / std::max(0.0001f, stepTimes[i + 1] - stepTimes[i]);
                    auto eased = ease_out_cubic(segmentT);
                    return stepProgress[i] + (stepProgress[i + 1] - stepProgress[i]) * eased;
                }
            }

            return totalTravel;
        }

        void draw_dragon_fireball_effect(ImDrawList* drawList, const ImVec2& from, const ImVec2& to, uint32_t now, uint32_t startTick, float scale)
        {
            if (!drawList)
                return;

            constexpr uint32_t kEffectDurationMs = 820;
            auto elapsed = now - startTick;
            if (elapsed >= kEffectDurationMs)
                return;

            auto t = static_cast<float>(elapsed) / static_cast<float>(kEffectDurationMs);
            auto flightT = std::min(t / 0.72f, 1.0f);
            auto smokeT = ease_out_cubic(flightT);

            auto dx = to.x - from.x;
            auto dy = to.y - from.y;
            auto length = std::sqrt(dx * dx + dy * dy);
            if (length < 8.0f)
                return;

            auto dir = ImVec2(dx / length, dy / length);
            auto normal = ImVec2(-dir.y, dir.x);
            auto projectile = ImVec2(
                from.x + dx * smokeT,
                from.y + dy * smokeT);
            auto pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.0105f);
            auto smokeWidth = std::max(14.0f, scale * (20.0f + pulse * 10.0f));

            for (int i = 0; i < 10; ++i)
            {
                auto f = static_cast<float>(i) / 9.0f;
                if (f > smokeT)
                    break;
                auto drift = std::sin(static_cast<float>(now) * 0.006f + f * 9.0f) * smokeWidth * 0.28f;
                auto p = ImVec2(
                    from.x + dx * f + normal.x * drift,
                    from.y + dy * f + normal.y * drift);
                auto radius = std::max(5.0f, smokeWidth * (0.52f - f * 0.18f));
                auto alpha = static_cast<int>(118.0f - f * 48.0f);
                drawList->AddCircleFilled(p, radius, IM_COL32(152, 76, 214, alpha), 18);
                drawList->AddCircleFilled(p, radius * 0.72f, IM_COL32(210, 128, 255, alpha / 2), 18);
            }

            auto fireballRadius = std::max(10.0f, scale * (16.0f + pulse * 4.5f));
            drawList->AddCircleFilled(projectile, fireballRadius * 2.3f, IM_COL32(118, 34, 188, 88), 24);
            drawList->AddCircleFilled(projectile, fireballRadius * 1.55f, IM_COL32(154, 58, 228, 150), 24);
            drawList->AddCircleFilled(projectile, fireballRadius * 0.92f, IM_COL32(238, 160, 255, 235), 24);

            if (t > 0.72f)
            {
                auto impactT = (t - 0.72f) / 0.28f;
                auto impactPulse = 1.0f - impactT;
                drawList->AddCircleFilled(to, std::max(12.0f, fireballRadius * (1.4f + impactPulse * 2.2f)), IM_COL32(164, 82, 236, static_cast<int>(148 * impactPulse)), 24);
                drawList->AddCircle(to, std::max(14.0f, fireballRadius * (1.8f + impactPulse * 2.9f)), IM_COL32(238, 176, 255, static_cast<int>(210 * impactPulse)), 24, 2.3f);
            }
        }

        const char* get_reward_name(uint8_t type, uint8_t typeId)
        {
            auto* itemInfo = CDataFile::GetItemInfo(type, typeId);
            if (itemInfo && itemInfo->name && itemInfo->name[0])
                return itemInfo->name;
            return "Reward";
        }

        const char* get_reward_description(uint8_t type, uint8_t typeId)
        {
            auto* itemInfo = CDataFile::GetItemInfo(type, typeId);
            if (itemInfo && itemInfo->description && itemInfo->description[0])
                return itemInfo->description;
            return nullptr;
        }

        ImVec2 rotate_point(const ImVec2& point, const ImVec2& center, float radians)
        {
            auto sinValue = std::sin(radians);
            auto cosValue = std::cos(radians);
            auto deltaX = point.x - center.x;
            auto deltaY = point.y - center.y;
            return ImVec2(
                center.x + deltaX * cosValue - deltaY * sinValue,
                center.y + deltaX * sinValue + deltaY * cosValue);
        }

        ImVec2 point_on_circle(const ImVec2& center, float radius, float angleRadians)
        {
            return ImVec2(
                center.x + std::cos(angleRadians) * radius,
                center.y + std::sin(angleRadians) * radius);
        }

        std::string fit_text_to_width(const std::string& text, float maxWidth)
        {
            if (text.empty() || maxWidth <= 8.0f)
                return "";

            if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
                return text;

            static constexpr const char* kEllipsis = "...";
            std::string clipped = text;
            while (!clipped.empty())
            {
                clipped.pop_back();
                auto candidate = clipped + kEllipsis;
                if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
                    return candidate;
            }

            return kEllipsis;
        }

        void draw_rotated_sprite(ImDrawList* drawList, LPDIRECT3DTEXTURE9 texture, const ImVec2& center, const ImVec2& size, float radians, ImU32 tint = IM_COL32(255, 255, 255, 255))
        {
            if (!drawList || !texture)
                return;

            auto half = ImVec2(size.x * 0.5f, size.y * 0.5f);
            auto topLeft = rotate_point(ImVec2(center.x - half.x, center.y - half.y), center, radians);
            auto topRight = rotate_point(ImVec2(center.x + half.x, center.y - half.y), center, radians);
            auto bottomRight = rotate_point(ImVec2(center.x + half.x, center.y + half.y), center, radians);
            auto bottomLeft = rotate_point(ImVec2(center.x - half.x, center.y + half.y), center, radians);
            drawList->AddImageQuad(
                reinterpret_cast<ImTextureID>(texture),
                topLeft,
                topRight,
                bottomRight,
                bottomLeft,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                ImVec2(0.0f, 1.0f),
                tint);
        }

        void draw_soft_panel_frame(ImDrawList* drawList, const ImVec2& min, const ImVec2& size, const char* title)
        {
            auto max = ImVec2(min.x + size.x, min.y + size.y);
            drawList->AddRectFilled(min, max, IM_COL32(15, 13, 12, 186), 8.0f);
            drawList->AddRect(min, max, IM_COL32(160, 129, 82, 176), 8.0f, 0, 1.0f);

            if (title && title[0])
            {
                auto titleMin = ImVec2(min.x + 8.0f, min.y + 8.0f);
                auto titleMax = ImVec2(max.x - 8.0f, min.y + 30.0f);
                drawList->AddRectFilled(titleMin, titleMax, IM_COL32(31, 24, 18, 220), 5.0f);
                drawList->AddRect(titleMin, titleMax, IM_COL32(184, 146, 94, 155), 5.0f, 0, 1.0f);
                drawList->AddText(ImVec2(titleMin.x + 10.0f, titleMin.y + 6.0f), IM_COL32(244, 232, 203, 255), title);
            }
        }

        void draw_panel_texture_or_frame(ImDrawList* drawList, LPDIRECT3DTEXTURE9 texture, const ImVec2& min, const ImVec2& size, const char* fallbackTitle)
        {
            if (texture)
            {
                drawList->AddImage(
                    reinterpret_cast<ImTextureID>(texture),
                    min,
                    ImVec2(min.x + size.x, min.y + size.y),
                    ImVec2(0.0f, 0.0f),
                    ImVec2(1.0f, 1.0f),
                    IM_COL32(255, 255, 255, 255));
                return;
            }

            draw_soft_panel_frame(drawList, min, size, fallbackTitle);
        }

        void draw_reward_tooltip(int rewardIndex)
        {
            auto itemCount = roulette_item_count();
            if (rewardIndex < 0 || rewardIndex >= itemCount)
                return;

            auto count = roulette_event::rewardCount[rewardIndex] ? roulette_event::rewardCount[rewardIndex] : 1;
            auto itemName = get_reward_name(roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);
            auto itemDesc = get_reward_description(roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);

            ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(240.0f, FLT_MAX));
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(220.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.60f, 1.0f));
            ImGui::Text("%s", itemName);
            ImGui::PopStyleColor();
            if (itemDesc)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.80f, 0.82f, 1.0f));
                ImGui::TextUnformatted(itemDesc);
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            if (count > 1)
                ImGui::Text("Quantity: %d", count);
            ImGui::EndTooltip();
        }

        void draw_reward_list_panel(const ImVec2& size)
        {
            static int s_rewardListPage = 0;
            auto drawList = ImGui::GetWindowDrawList();
            auto panelMin = ImGui::GetWindowPos();
            auto panelTexture = load_roulette_left_panel_texture();
            draw_panel_texture_or_frame(drawList, panelTexture, panelMin, size, "REWARD LIST");

            auto scaleX = panelTexture ? (size.x / 360.0f) : 1.0f;
            auto scaleY = panelTexture ? (size.y / 941.0f) : 1.0f;
            auto bodyMin = panelTexture
                ? ImVec2(panelMin.x + 22.0f * scaleX, panelMin.y + 82.0f * scaleY)
                : ImVec2(panelMin.x + 10.0f, panelMin.y + 38.0f);
            auto bodySize = panelTexture
                ? ImVec2(316.0f * scaleX, 770.0f * scaleY)
                : ImVec2(size.x - 20.0f, size.y - 48.0f);
            auto rowHeight = 44.0f;
            auto pagerHeight = 28.0f;
            auto listGap = 6.0f;
            auto listBodySize = ImVec2(bodySize.x, bodySize.y - pagerHeight - listGap);
            auto rowStep = rowHeight + ImGui::GetStyle().ItemSpacing.y;
            auto itemsPerPage = std::max(1, static_cast<int>((listBodySize.y + ImGui::GetStyle().ItemSpacing.y) / rowStep));
            auto itemCount = static_cast<int>(roulette_item_count());
            auto pageCount = std::max(1, (itemCount + itemsPerPage - 1) / itemsPerPage);
            s_rewardListPage = std::clamp(s_rewardListPage, 0, pageCount - 1);
            auto startIndex = s_rewardListPage * itemsPerPage;
            auto endIndex = std::min(itemCount, startIndex + itemsPerPage);

            ImGui::SetCursorScreenPos(bodyMin);
            ImGui::BeginChild(
                "##roulette_reward_list_scroll",
                listBodySize,
                false,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            auto rowWidth = ImGui::GetContentRegionAvail().x;
            for (int i = startIndex; i < endIndex; ++i)
            {
                auto rowSize = ImVec2(rowWidth, rowHeight);
                auto rowId = "##roulette_reward_row_" + std::to_string(i);
                ImGui::InvisibleButton(rowId.c_str(), rowSize);
                auto rowMin = ImGui::GetItemRectMin();
                auto rowMax = ImGui::GetItemRectMax();
                auto hovered = ImGui::IsItemHovered();
                auto featured = roulette_event::featuredRewardIndex == i;
                auto guaranteed = roulette_event::guaranteedRewardIndex == i;

                auto fillColor = IM_COL32(45, 36, 31, 208);
                auto borderColor = IM_COL32(167, 135, 88, 160);
                if (guaranteed)
                {
                    fillColor = IM_COL32(88, 60, 34, 220);
                    borderColor = IM_COL32(247, 188, 104, 210);
                }
                else if (featured)
                {
                    fillColor = IM_COL32(49, 63, 93, 220);
                    borderColor = IM_COL32(109, 203, 255, 210);
                }
                else if (hovered)
                {
                    fillColor = IM_COL32(70, 53, 39, 228);
                    borderColor = IM_COL32(215, 177, 121, 210);
                }

                drawList->AddRectFilled(rowMin, rowMax, fillColor, 6.0f);
                drawList->AddRect(rowMin, rowMax, borderColor, 6.0f, 0, hovered ? 1.6f : 1.0f);

                auto iconMin = ImVec2(rowMin.x + 6.0f, rowMin.y + 6.0f);
                auto iconMax = ImVec2(iconMin.x + 32.0f, iconMin.y + 32.0f);
                draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
                draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[i]);

                auto name = get_reward_name(roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
                auto rowLabel = std::string(name) + " x" + std::to_string(std::max<int>(1, roulette_event::rewardCount[i]));
                auto rowTextWidth = std::max(48.0f, rowMax.x - (iconMax.x + 8.0f) - 6.0f);
                auto clippedRowLabel = fit_text_to_width(rowLabel, rowTextWidth);

                char meta[64]{};
                if (guaranteed)
                    std::snprintf(meta, sizeof(meta), "Guaranteed reward");
                else if (featured)
                    std::snprintf(meta, sizeof(meta), "Dragon's favorite");
                else
                    std::snprintf(meta, sizeof(meta), "Chance %.2f%%", static_cast<float>(roulette_event::rewardChance[i]) / 100.0f);

                auto clippedMeta = fit_text_to_width(meta, rowTextWidth);
                drawList->PushClipRect(rowMin, rowMax, true);
                drawList->AddText(ImVec2(iconMax.x + 8.0f, rowMin.y + 8.0f), IM_COL32(245, 234, 208, 255), clippedRowLabel.c_str());
                drawList->AddText(ImVec2(iconMax.x + 8.0f, rowMin.y + 24.0f), IM_COL32(190, 198, 207, 235), clippedMeta.c_str());
                drawList->PopClipRect();

                if (hovered)
                    draw_reward_tooltip(i);
            }

            ImGui::EndChild();

            auto pagerMin = ImVec2(bodyMin.x, bodyMin.y + listBodySize.y + listGap);
            auto pagerMax = ImVec2(bodyMin.x + bodySize.x, pagerMin.y + pagerHeight);
            auto buttonSize = ImVec2(24.0f, 22.0f);
            auto leftMin = ImVec2(pagerMin.x + 18.0f, pagerMin.y + 3.0f);
            auto leftMax = ImVec2(leftMin.x + buttonSize.x, leftMin.y + buttonSize.y);
            auto rightMin = ImVec2(pagerMax.x - 18.0f - buttonSize.x, pagerMin.y + 3.0f);
            auto rightMax = ImVec2(rightMin.x + buttonSize.x, rightMin.y + buttonSize.y);
            auto prevHovered = false;
            auto nextHovered = false;

            ImGui::SetCursorScreenPos(leftMin);
            ImGui::InvisibleButton("##roulette_reward_prev_page", buttonSize);
            prevHovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked() && s_rewardListPage > 0)
                --s_rewardListPage;

            ImGui::SetCursorScreenPos(rightMin);
            ImGui::InvisibleButton("##roulette_reward_next_page", buttonSize);
            nextHovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked() && s_rewardListPage + 1 < pageCount)
                ++s_rewardListPage;

            drawList->AddRectFilled(leftMin, leftMax, IM_COL32(38, 28, 22, prevHovered ? 220 : 180), 5.0f);
            drawList->AddRect(leftMin, leftMax, IM_COL32(165, 131, 83, 190), 5.0f, 0, 1.0f);
            drawList->AddText(ImVec2(leftMin.x + 8.0f, leftMin.y + 3.0f), IM_COL32(240, 228, 198, 255), "<");

            drawList->AddRectFilled(rightMin, rightMax, IM_COL32(38, 28, 22, nextHovered ? 220 : 180), 5.0f);
            drawList->AddRect(rightMin, rightMax, IM_COL32(165, 131, 83, 190), 5.0f, 0, 1.0f);
            drawList->AddText(ImVec2(rightMin.x + 8.0f, rightMin.y + 3.0f), IM_COL32(240, 228, 198, 255), ">");

            auto pageText = std::to_string(s_rewardListPage + 1) + " / " + std::to_string(pageCount);
            auto pageTextSize = ImGui::CalcTextSize(pageText.c_str());
            drawList->AddText(
                ImVec2(pagerMin.x + (bodySize.x - pageTextSize.x) * 0.5f, pagerMin.y + 5.0f),
                IM_COL32(231, 220, 193, 255),
                pageText.c_str());
        }

        void draw_reward_showcase_box(const char* title, const ImVec2& min, const ImVec2& size, int rewardIndex, const std::string& subtitle)
        {
            auto drawList = ImGui::GetWindowDrawList();
            auto max = ImVec2(min.x + size.x, min.y + size.y);
            drawList->AddRectFilled(min, max, IM_COL32(21, 18, 15, 205), 7.0f);
            drawList->AddRect(min, max, IM_COL32(167, 135, 88, 160), 7.0f, 0, 1.0f);
            drawList->AddText(ImVec2(min.x + 10.0f, min.y + 8.0f), IM_COL32(244, 232, 203, 255), title);

            if (rewardIndex >= 0 && rewardIndex < roulette_item_count())
            {
                auto iconMin = ImVec2(min.x + 10.0f, min.y + 28.0f);
                auto iconMax = ImVec2(iconMin.x + 36.0f, iconMin.y + 36.0f);
                draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);
                draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[rewardIndex]);

                auto name = get_reward_name(roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);
                auto label = std::string(name) + " x" + std::to_string(std::max<int>(1, roulette_event::rewardCount[rewardIndex]));
                drawList->AddText(ImVec2(iconMax.x + 10.0f, min.y + 32.0f), IM_COL32(245, 234, 208, 255), label.c_str());
                if (!subtitle.empty())
                    drawList->AddText(ImVec2(iconMax.x + 10.0f, min.y + 52.0f), IM_COL32(190, 198, 207, 230), subtitle.c_str());
            }
            else
            {
                drawList->AddText(ImVec2(min.x + 10.0f, min.y + 40.0f), IM_COL32(170, 176, 184, 220), "Not configured");
            }
        }

        void draw_right_info_panel(const ImVec2& size, int tokenCount, int requiredTokenCount)
        {
            auto drawList = ImGui::GetWindowDrawList();
            auto panelMin = ImGui::GetWindowPos();
            auto panelTexture = load_roulette_right_panel_texture();
            draw_panel_texture_or_frame(drawList, panelTexture, panelMin, size, "FORTUNE INFO");

            auto scaleX = panelTexture ? (size.x / 392.0f) : 1.0f;
            auto scaleY = panelTexture ? (size.y / 941.0f) : 1.0f;
            auto guaranteedMin = panelTexture
                ? ImVec2(panelMin.x + 18.0f * scaleX, panelMin.y + 98.0f * scaleY)
                : ImVec2(panelMin.x + 10.0f, panelMin.y + 42.0f);
            auto guaranteedSize = panelTexture
                ? ImVec2(344.0f * scaleX, 250.0f * scaleY)
                : ImVec2(size.x - 20.0f, 156.0f);
            auto winnersMin = panelTexture
                ? ImVec2(panelMin.x + 18.0f * scaleX, panelMin.y + 387.0f * scaleY)
                : ImVec2(panelMin.x + 10.0f, guaranteedMin.y + guaranteedSize.y + 14.0f);
            auto winnersSize = panelTexture
                ? ImVec2(344.0f * scaleX, 330.0f * scaleY)
                : ImVec2(size.x - 20.0f, 210.0f);
            auto tokensMin = panelTexture
                ? ImVec2(panelMin.x + 18.0f * scaleX, panelMin.y + 770.0f * scaleY)
                : ImVec2(panelMin.x + 10.0f, winnersMin.y + winnersSize.y + 14.0f);
            auto tokensSize = panelTexture
                ? ImVec2(344.0f * scaleX, 124.0f * scaleY)
                : ImVec2(size.x - 20.0f, 90.0f);
            auto winnersMax = ImVec2(winnersMin.x + winnersSize.x, winnersMin.y + winnersSize.y);

            if (!panelTexture)
            {
                draw_reward_showcase_box(
                    "Guaranteed Top Reward",
                    guaranteedMin,
                    guaranteedSize,
                    roulette_event::guaranteedRewardIndex == 0xFF ? -1 : roulette_event::guaranteedRewardIndex,
                    "");
                drawList->AddRectFilled(winnersMin, winnersMax, IM_COL32(21, 18, 15, 205), 7.0f);
                drawList->AddRect(winnersMin, winnersMax, IM_COL32(167, 135, 88, 160), 7.0f, 0, 1.0f);
                drawList->AddText(ImVec2(winnersMin.x + 10.0f, winnersMin.y + 8.0f), IM_COL32(244, 232, 203, 255), "LAST BIG WINNERS");
                auto tokensMax = ImVec2(tokensMin.x + tokensSize.x, tokensMin.y + tokensSize.y);
                drawList->AddRectFilled(tokensMin, tokensMax, IM_COL32(21, 18, 15, 205), 7.0f);
                drawList->AddRect(tokensMin, tokensMax, IM_COL32(167, 135, 88, 160), 7.0f, 0, 1.0f);
            }

            auto guaranteedIndex = roulette_event::guaranteedRewardIndex == 0xFF ? -1 : roulette_event::guaranteedRewardIndex;
            if (guaranteedIndex >= 0 && guaranteedIndex < roulette_item_count())
            {
                auto iconSize = 42.0f * std::min(scaleX, scaleY);
                auto iconMin = ImVec2(guaranteedMin.x + (guaranteedSize.x - iconSize) * 0.5f, guaranteedMin.y + 76.0f * scaleY);
                auto iconMax = ImVec2(iconMin.x + iconSize, iconMin.y + iconSize);
                draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[guaranteedIndex], roulette_event::rewardTypeId[guaranteedIndex]);
                draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[guaranteedIndex]);

                auto rewardName = get_reward_name(roulette_event::rewardType[guaranteedIndex], roulette_event::rewardTypeId[guaranteedIndex]);
                auto rewardLabel = fit_text_to_width(
                    std::string(rewardName) + " x" + std::to_string(std::max<int>(1, roulette_event::rewardCount[guaranteedIndex])),
                    guaranteedSize.x - 40.0f * scaleX);
                auto rewardLabelSize = ImGui::CalcTextSize(rewardLabel.c_str());
                drawList->AddText(
                    ImVec2(guaranteedMin.x + (guaranteedSize.x - rewardLabelSize.x) * 0.5f, guaranteedMin.y + 130.0f * scaleY),
                    IM_COL32(245, 236, 214, 255),
                    rewardLabel.c_str());
            }

            auto spinsText = roulette_event::guaranteedSpinTarget
                ? ("Spins: " + std::to_string(roulette_event::guaranteedSpinProgress) + " / " + std::to_string(roulette_event::guaranteedSpinTarget))
                : "Spins: -- / --";
            auto spinsTextSize = ImGui::CalcTextSize(spinsText.c_str());
            drawList->AddText(
                ImVec2(guaranteedMin.x + (guaranteedSize.x - spinsTextSize.x) * 0.5f, guaranteedMin.y + 171.0f * scaleY),
                IM_COL32(198, 242, 118, 255),
                spinsText.c_str());

            auto hintText = roulette_event::guaranteedSpinTarget
                ? ("Spin " + std::to_string(std::max(0, roulette_event::guaranteedSpinTarget - roulette_event::guaranteedSpinProgress)) + " more time(s) to get the guaranteed top reward!")
                : std::string("No guaranteed reward configured.");
            auto hintClipped = fit_text_to_width(hintText, guaranteedSize.x - 48.0f * scaleX);
            auto hintSize = ImGui::CalcTextSize(hintClipped.c_str());
            drawList->AddText(
                ImVec2(guaranteedMin.x + (guaranteedSize.x - hintSize.x) * 0.5f, guaranteedMin.y + 210.0f * scaleY),
                IM_COL32(232, 220, 194, 235),
                hintClipped.c_str());

            auto winnerCount = std::min<int>(roulette_event::recentWinnerCount, 5);
            if (!winnerCount)
            {
                drawList->AddText(
                    ImVec2(winnersMin.x + 22.0f * scaleX, winnersMin.y + 52.0f * scaleY),
                    IM_COL32(170, 176, 184, 220),
                    "No winners yet");
            }
            else
            {
                auto rowHeight = 53.0f * scaleY;
                auto rowGap = 14.0f * scaleY;
                for (int i = 0; i < winnerCount && i < static_cast<int>(roulette_event::recentWinnerName.size()); ++i)
                {
                    auto rowMin = ImVec2(winnersMin.x + 18.0f * scaleX, winnersMin.y + 46.0f * scaleY + static_cast<float>(i) * (rowHeight + rowGap));
                    auto rowMax = ImVec2(winnersMax.x - 18.0f * scaleX, rowMin.y + rowHeight);
                    auto rewardIndex = roulette_event::recentWinnerRewardIndex[i];
                    auto iconSize = 34.0f * std::min(scaleX, scaleY);
                    auto iconMin = ImVec2(rowMin.x + 8.0f * scaleX, rowMin.y + (rowHeight - iconSize) * 0.5f);
                    auto iconMax = ImVec2(iconMin.x + iconSize, iconMin.y + iconSize);
                    if (rewardIndex >= 0 && rewardIndex < roulette_item_count())
                        draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);

                    auto playerName = fit_text_to_width(std::string(roulette_event::recentWinnerName[i].data()), 152.0f * scaleX);
                    auto ageText = std::to_string(roulette_event::recentWinnerAgeMinutes[i]) + "m";
                    auto ageSize = ImGui::CalcTextSize(ageText.c_str());
                    drawList->AddText(ImVec2(iconMax.x + 10.0f * scaleX, rowMin.y + 15.0f * scaleY), IM_COL32(243, 246, 249, 255), playerName.c_str());
                    drawList->AddText(ImVec2(rowMax.x - ageSize.x - 10.0f * scaleX, rowMin.y + 15.0f * scaleY), IM_COL32(76, 214, 255, 255), ageText.c_str());
                }
            }

            auto balanceLabel = std::to_string(tokenCount) + " TP";
            auto balanceLabelSize = ImGui::CalcTextSize(balanceLabel.c_str());
            drawList->AddText(
                ImVec2(tokensMin.x + 171.0f * scaleX - balanceLabelSize.x * 0.5f, tokensMin.y + 46.0f * scaleY),
                IM_COL32(255, 246, 224, 255),
                balanceLabel.c_str());

            auto costLabel = "Each spin costs " + std::to_string(requiredTokenCount) + " TP";
            auto costLabelSize = ImGui::CalcTextSize(costLabel.c_str());
            drawList->AddText(
                ImVec2(tokensMin.x + (tokensSize.x - costLabelSize.x) * 0.5f, tokensMin.y + 91.0f * scaleY),
                IM_COL32(216, 205, 183, 235),
                costLabel.c_str());
        }
    }

    const char* roulette_result_label(uint8_t result)
    {
        switch (static_cast<GameRouletteResult>(result))
        {
        case GameRouletteResult::NoToken:
            return "Not enough tokens";
        case GameRouletteResult::InventoryFull:
            return "Inventory is full";
        case GameRouletteResult::Busy:
            return "Roulette is already spinning";
        case GameRouletteResult::NotConfigured:
            return "Roulette is not configured";
        case GameRouletteResult::Failure:
            return "Could not complete the roll";
        default:
            return "";
        }
    }

    void request_roulette_list()
    {
        ensure_client_sysmsg_dispatch_ready();
        if (g_var->hwnd && IsWindow(g_var->hwnd))
            PostMessageA(g_var->hwnd, kClientRouletteListWindowMessage, 0, 0);

        g_lastRouletteListTick = GetTickCount();
    }

    void request_roulette_spin()
    {
        ensure_client_sysmsg_dispatch_ready();
        if (g_var->hwnd && IsWindow(g_var->hwnd))
            PostMessageA(g_var->hwnd, kClientRouletteRollWindowMessage, 0, 0);

        roulette_event::lastResult = 0;
        roulette_event::lastSpinSuccess = false;
        roulette_event::lastGrantSuccess = false;
        g_lastRouletteRollTick = GetTickCount();
        release_imgui_capture();
    }

    void update_roulette_spin_state()
    {
        if (!roulette_event::spinActive)
            return;

        auto now = GetTickCount();
        auto duration = roulette_event::spinDurationMs ? roulette_event::spinDurationMs : 4500;
        if (now - roulette_event::spinStartTick <= duration + 500)
            return;

        roulette_event::spinActive = false;
        roulette_event::spinDurationMs = 0;
        release_imgui_capture();
    }

    uint8_t roulette_item_count()
    {
        auto count = roulette_event::itemCount;
        if (count > roulette_event::rewardType.size())
            count = static_cast<uint8_t>(roulette_event::rewardType.size());
        if (count > 12)
            count = 12;
        return count;
    }

    void draw_roulette_wheel(bool canRoll)
    {
        auto drawList = ImGui::GetWindowDrawList();
        auto itemCount = roulette_item_count();
        auto regionOrigin = ImGui::GetCursorScreenPos();
        auto regionSize = ImGui::GetContentRegionAvail();
        auto bgSize = std::min(regionSize.x, regionSize.y);
        auto origin = ImVec2(regionOrigin.x + std::max(0.0f, (regionSize.x - bgSize) * 0.5f), regionOrigin.y);
        auto height = bgSize;
        auto now = GetTickCount();

        ImGui::Dummy(regionSize);

        auto bgTex = load_roulette_bg_texture();
        if (bgTex)
        {
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(bgTex),
                origin,
                ImVec2(origin.x + bgSize, origin.y + bgSize),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, 255));
        }
        else
        {
            drawList->AddRectFilled(origin, ImVec2(origin.x + bgSize, origin.y + height), IM_COL32(16, 17, 20, 210), 6.0f);
            drawList->AddRect(origin, ImVec2(origin.x + bgSize, origin.y + height), IM_COL32(96, 111, 132, 180), 6.0f);
        }

        if (!itemCount)
        {
            auto label = "Waiting for server rewards...";
            auto textSize = ImGui::CalcTextSize(label);
            drawList->AddText(ImVec2(origin.x + (bgSize - textSize.x) * 0.5f, origin.y + (height - textSize.y) * 0.5f), IM_COL32(180, 185, 194, 230), label);
            return;
        }

        auto progress = compute_spin_progress(now, itemCount);

        auto centerX = origin.x + bgSize * 0.5f;
        auto centerY = origin.y + bgSize * (bgTex ? 0.478f : 0.52f);
        auto wheelCenter = ImVec2(centerX, centerY);
        auto slotFrameSize = bgTex ? (bgSize * 0.084f) : (itemCount > 12 ? (bgSize * 0.07f) : (bgSize * 0.09f));
        slotFrameSize = std::max(slotFrameSize, 24.0f);
        auto iconSize = bgTex ? (slotFrameSize * 0.76f) : slotFrameSize;
        auto wheelRadiusX = bgTex ? (bgSize * 0.282f) : (bgSize * 0.34f);
        auto wheelRadiusY = bgTex ? (bgSize * 0.270f) : (bgSize * 0.34f);
        auto step = (kPi * 2.0f) / static_cast<float>(itemCount);

        auto mousePos = ImGui::GetMousePos();
        auto hoveredSlot = -1;

        auto selectorProgress = wrap_float(progress, static_cast<float>(itemCount));
        auto selectorAngle = -kPi * 0.5f + selectorProgress * step;
        auto selectorSlot = ImVec2(centerX + std::cos(selectorAngle) * wheelRadiusX, centerY + std::sin(selectorAngle) * wheelRadiusY);

        drawList->PushClipRect(origin, ImVec2(origin.x + bgSize, origin.y + height), true);

        if (!bgTex)
        {
            auto wheelRadius = (wheelRadiusX + wheelRadiusY) * 0.5f;
            drawList->AddCircleFilled(ImVec2(centerX, centerY), wheelRadius + 30.0f, IM_COL32(31, 34, 42, 230), 48);
            drawList->AddCircle(ImVec2(centerX, centerY), wheelRadius + 30.0f, IM_COL32(116, 134, 164, 190), 48, 2.0f);
            drawList->AddCircleFilled(ImVec2(centerX, centerY), 26.0f, IM_COL32(22, 24, 29, 240), 32);
            drawList->AddCircle(ImVec2(centerX, centerY), 26.0f, IM_COL32(212, 178, 96, 230), 32, 2.0f);
        }

        for (auto i = 0; i < itemCount; ++i)
        {
            auto angle = -kPi * 0.5f + static_cast<float>(i) * step;
            auto slotCenter = ImVec2(centerX + std::cos(angle) * wheelRadiusX, centerY + std::sin(angle) * wheelRadiusY);

            auto frameMin = ImVec2(slotCenter.x - slotFrameSize * 0.5f, slotCenter.y - slotFrameSize * 0.5f);
            auto frameMax = ImVec2(frameMin.x + slotFrameSize, frameMin.y + slotFrameSize);
            auto iconMin = ImVec2(slotCenter.x - iconSize * 0.5f, slotCenter.y - iconSize * 0.5f);
            auto iconMax = ImVec2(iconMin.x + iconSize, iconMin.y + iconSize);
            auto wrappedProgress = wrap_float(progress, static_cast<float>(itemCount));
            auto distance = std::fabs(static_cast<float>(i) - wrappedProgress);
            distance = std::min(distance, static_cast<float>(itemCount) - distance);
            auto selected = distance < 0.45f;

            auto slotHovered = mousePos.x >= frameMin.x && mousePos.x <= frameMax.x
                && mousePos.y >= frameMin.y && mousePos.y <= frameMax.y;
            if (slotHovered)
                hoveredSlot = i;

            if (!bgTex)
                drawList->AddLine(ImVec2(centerX, centerY), slotCenter, IM_COL32(78, 88, 108, 135), 1.0f);

            if (bgTex)
            {
                if (selected)
                    drawList->AddRect(frameMin, frameMax, IM_COL32(255, 219, 118, 165), 8.0f, 0, 2.0f);
                else if (slotHovered)
                    drawList->AddRect(frameMin, frameMax, IM_COL32(255, 235, 168, 120), 8.0f, 0, 2.0f);
            }
            else
            {
                auto bgColor = slotHovered ? IM_COL32(80, 88, 108, 250)
                    : (selected ? IM_COL32(65, 72, 88, 250) : IM_COL32(43, 48, 58, 235));
                auto borderColor = slotHovered ? IM_COL32(255, 230, 140, 255)
                    : (selected ? IM_COL32(255, 219, 118, 245) : IM_COL32(132, 150, 178, 220));
                auto borderWidth = (slotHovered || selected) ? 2.0f : 1.0f;
                drawList->AddRectFilled(frameMin, frameMax, bgColor, 5.0f);
                drawList->AddRect(frameMin, frameMax, borderColor, 5.0f, 0, borderWidth);
            }

            draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
            draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[i]);
        }
        drawList->PopClipRect();

        if (!bgTex)
        {
            drawList->AddTriangleFilled(
                ImVec2(centerX, origin.y + 13.0f),
                ImVec2(centerX - 9.0f, origin.y + 30.0f),
                ImVec2(centerX + 9.0f, origin.y + 30.0f),
                IM_COL32(255, 226, 126, 255));
        }

        if (static_cast<int32_t>(now - roulette_event::celebrationUntilTick) < 0)
        {
            auto pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.0105f);
            auto glowAlpha = static_cast<int>(60.0f + pulse * 70.0f);
            auto glowRadius = slotFrameSize * (0.82f + pulse * 0.32f);
            drawList->AddCircleFilled(selectorSlot, glowRadius, IM_COL32(255, 204, 96, glowAlpha), 28);
            drawList->AddCircle(selectorSlot, glowRadius + 2.5f, IM_COL32(255, 236, 180, 180), 28, 2.0f);
        }

        if (hoveredSlot >= 0 && hoveredSlot < itemCount)
            draw_reward_tooltip(hoveredSlot);

        auto buttonMin = bgTex
            ? ImVec2(origin.x + bgSize * 0.282f, origin.y + bgSize * 0.855f)
            : ImVec2(origin.x + bgSize * 0.22f, origin.y + bgSize * 0.88f);
        auto buttonMax = bgTex
            ? ImVec2(origin.x + bgSize * 0.718f, origin.y + bgSize * 0.942f)
            : ImVec2(origin.x + bgSize * 0.78f, origin.y + bgSize * 0.95f);

        auto buttonHovered = mousePos.x >= buttonMin.x && mousePos.x <= buttonMax.x
            && mousePos.y >= buttonMin.y && mousePos.y <= buttonMax.y;
        auto mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        auto clicked = buttonHovered && mouseDown && !g_rollMouseWasDown;
        g_rollMouseWasDown = mouseDown;

        if (buttonHovered)
        {
            auto alpha = mouseDown ? 120 : static_cast<int>(70.0f + 22.0f * std::sin(static_cast<float>(now) * 0.0045f));
            drawList->AddRectFilled(buttonMin, buttonMax, IM_COL32(255, 210, 110, alpha), 10.0f);
        }

        auto buttonText = "";
        if (buttonText[0] != '\0')
        {
            auto textSize = ImGui::CalcTextSize(buttonText);
            drawList->AddText(
                ImVec2(buttonMin.x + (buttonMax.x - buttonMin.x - textSize.x) * 0.5f,
                    buttonMin.y + (buttonMax.y - buttonMin.y - textSize.y) * 0.5f),
                IM_COL32(255, 248, 231, 245),
                buttonText);
        }

        if (clicked && canRoll)
            request_roulette_spin();
    }

    void draw_roulette_composite_panel(bool canRoll, int tokenCount, int requiredTokenCount)
    {
        auto drawList = ImGui::GetWindowDrawList();
        auto regionOrigin = ImGui::GetCursorScreenPos();
        auto regionSize = ImGui::GetContentRegionAvail();
        auto now = GetTickCount();
        ImGui::Dummy(regionSize);

        auto* bgTex = load_roulette_bg_texture();
        constexpr float kRefW = 1672.0f;
        constexpr float kRefH = 941.0f;
        auto scale = std::min(regionSize.x / kRefW, regionSize.y / kRefH);
        scale = std::max(scale, 0.1f);
        auto drawSize = ImVec2(kRefW * scale, kRefH * scale);
        auto origin = ImVec2(
            regionOrigin.x + std::max(0.0f, (regionSize.x - drawSize.x) * 0.5f),
            regionOrigin.y + std::max(0.0f, (regionSize.y - drawSize.y) * 0.5f));

        if (bgTex)
        {
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(bgTex),
                origin,
                ImVec2(origin.x + drawSize.x, origin.y + drawSize.y),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, 255));
        }
        else
        {
            drawList->AddRectFilled(origin, ImVec2(origin.x + drawSize.x, origin.y + drawSize.y), IM_COL32(15, 16, 20, 235), 8.0f);
        }

        auto map_point = [&](float x, float y)
            {
                return ImVec2(origin.x + x * scale, origin.y + y * scale);
            };
        auto map_value = [&](float v)
            {
                return v * scale;
            };

        auto itemCount = roulette_item_count();
        if (!itemCount)
        {
            auto text = "Waiting for server rewards...";
            auto textSize = ImGui::CalcTextSize(text);
            drawList->AddText(ImVec2(origin.x + (drawSize.x - textSize.x) * 0.5f, origin.y + (drawSize.y - textSize.y) * 0.5f), IM_COL32(200, 205, 214, 240), text);
            return;
        }

        auto progress = compute_spin_progress(now, itemCount);

        auto slotFrameSize = map_value(g_rouletteLayoutDebug.slotFrameSize);
        auto iconSize = map_value(g_rouletteLayoutDebug.iconSize);
        auto mousePos = ImGui::GetMousePos();
        auto hoveredSlot = -1;
        auto selectorProgress = wrap_float(progress, static_cast<float>(itemCount));
        auto selectorBaseIndex = static_cast<int>(std::floor(selectorProgress)) % itemCount;
        auto selectorNextIndex = (selectorBaseIndex + 1) % itemCount;
        auto selectorBlend = selectorProgress - std::floor(selectorProgress);
        auto selectorBaseRef = g_rouletteLayoutDebug.slotCenters[selectorBaseIndex];
        auto selectorNextRef = g_rouletteLayoutDebug.slotCenters[selectorNextIndex];
        auto selectorRef = ImVec2(
            selectorBaseRef.x + (selectorNextRef.x - selectorBaseRef.x) * selectorBlend,
            selectorBaseRef.y + (selectorNextRef.y - selectorBaseRef.y) * selectorBlend);
        auto selectorSlot = map_point(selectorRef.x, selectorRef.y);
        auto dragonBreathFrom = map_point(608.0f, 233.0f);

        drawList->PushClipRect(origin, ImVec2(origin.x + drawSize.x, origin.y + drawSize.y), true);
        for (auto i = 0; i < itemCount; ++i)
        {
            auto slotCenter = map_point(g_rouletteLayoutDebug.slotCenters[i].x, g_rouletteLayoutDebug.slotCenters[i].y);
            auto frameMin = ImVec2(slotCenter.x - slotFrameSize * 0.5f, slotCenter.y - slotFrameSize * 0.5f);
            auto frameMax = ImVec2(frameMin.x + slotFrameSize, frameMin.y + slotFrameSize);
            auto iconMin = ImVec2(slotCenter.x - iconSize * 0.5f, slotCenter.y - iconSize * 0.5f);
            auto iconMax = ImVec2(iconMin.x + iconSize, iconMin.y + iconSize);

            auto wrappedProgress = wrap_float(progress, static_cast<float>(itemCount));
            auto distance = std::fabs(static_cast<float>(i) - wrappedProgress);
            distance = std::min(distance, static_cast<float>(itemCount) - distance);
            auto selected = distance < 0.45f;
            auto slotHovered = mousePos.x >= frameMin.x && mousePos.x <= frameMax.x
                && mousePos.y >= frameMin.y && mousePos.y <= frameMax.y;
            if (slotHovered)
                hoveredSlot = i;

            if (selected)
                drawList->AddRect(frameMin, frameMax, IM_COL32(255, 231, 140, 185), 8.0f, 0, 2.0f);
            else if (slotHovered)
                drawList->AddRect(frameMin, frameMax, IM_COL32(255, 245, 190, 120), 8.0f, 0, 2.0f);

            draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
            draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[i]);
        }

        if (g_rouletteLayoutDebug.drawGuides)
        {
            for (auto i = 0; i < itemCount; ++i)
            {
                auto slotCenter = map_point(g_rouletteLayoutDebug.slotCenters[i].x, g_rouletteLayoutDebug.slotCenters[i].y);
                auto guideFrameMin = ImVec2(slotCenter.x - slotFrameSize * 0.5f, slotCenter.y - slotFrameSize * 0.5f);
                auto guideFrameMax = ImVec2(guideFrameMin.x + slotFrameSize, guideFrameMin.y + slotFrameSize);
                drawList->AddRect(guideFrameMin, guideFrameMax, IM_COL32(0, 255, 180, 170), 4.0f, 0, 1.0f);
                drawList->AddLine(ImVec2(slotCenter.x - 5.0f, slotCenter.y), ImVec2(slotCenter.x + 5.0f, slotCenter.y), IM_COL32(0, 255, 180, 200), 1.0f);
                drawList->AddLine(ImVec2(slotCenter.x, slotCenter.y - 5.0f), ImVec2(slotCenter.x, slotCenter.y + 5.0f), IM_COL32(0, 255, 180, 200), 1.0f);
                auto label = std::to_string(i + 1);
                drawList->AddText(ImVec2(guideFrameMin.x, guideFrameMin.y - 14.0f), IM_COL32(0, 255, 180, 220), label.c_str());
            }
        }

        auto selectorFrameMin = ImVec2(selectorSlot.x - slotFrameSize * 0.5f, selectorSlot.y - slotFrameSize * 0.5f);
        auto selectorFrameMax = ImVec2(selectorFrameMin.x + slotFrameSize, selectorFrameMin.y + slotFrameSize);
        if (roulette_event::spinActive || static_cast<int32_t>(now - roulette_event::celebrationUntilTick) < 0)
        {
            auto pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 0.012f);
            auto glowAlpha = static_cast<int>(76.0f + pulse * 72.0f);
            auto inset = std::max(3.0f, slotFrameSize * 0.06f);
            drawList->AddRectFilled(
                ImVec2(selectorFrameMin.x + inset, selectorFrameMin.y + inset),
                ImVec2(selectorFrameMax.x - inset, selectorFrameMax.y - inset),
                IM_COL32(255, 208, 96, glowAlpha),
                8.0f);
            drawList->AddRect(selectorFrameMin, selectorFrameMax, IM_COL32(255, 236, 180, 230), 8.0f, 0, 2.4f);
            drawList->AddRect(
                ImVec2(selectorFrameMin.x - 2.0f, selectorFrameMin.y - 2.0f),
                ImVec2(selectorFrameMax.x + 2.0f, selectorFrameMax.y + 2.0f),
                IM_COL32(255, 208, 96, static_cast<int>(155.0f + pulse * 70.0f)),
                10.0f,
                0,
                1.8f);
        }
        if (roulette_event::grantEffectStartTick != 0 && now - roulette_event::grantEffectStartTick < 820)
            draw_dragon_fireball_effect(drawList, dragonBreathFrom, selectorSlot, now, roulette_event::grantEffectStartTick, scale);
        drawList->PopClipRect();

        if (hoveredSlot >= 0 && hoveredSlot < itemCount)
            draw_reward_tooltip(hoveredSlot);

        auto listMin = map_point(g_rouletteLayoutDebug.listMin.x, g_rouletteLayoutDebug.listMin.y);
        auto listSize = ImVec2(map_value(g_rouletteLayoutDebug.listSize.x), map_value(g_rouletteLayoutDebug.listSize.y));
        auto rowHeight = map_value(g_rouletteLayoutDebug.listRowHeight);
        auto rowGap = map_value(g_rouletteLayoutDebug.listRowGap);
        auto rowWidth = listSize.x;
        if (g_rouletteLayoutDebug.drawGuides)
        {
            drawList->AddRect(listMin, ImVec2(listMin.x + listSize.x, listMin.y + listSize.y), IM_COL32(255, 120, 0, 170), 4.0f, 0, 1.0f);
            drawList->AddCircleFilled(listMin, 5.0f, IM_COL32(255, 120, 0, 220), 12);
            drawList->AddCircleFilled(ImVec2(listMin.x + listSize.x, listMin.y + listSize.y), 5.0f, IM_COL32(255, 120, 0, 220), 12);
        }
        for (int i = 0; i < itemCount; ++i)
        {
            auto rowMin = ImVec2(listMin.x, listMin.y + i * (rowHeight + rowGap));
            auto rowMax = ImVec2(rowMin.x + rowWidth, rowMin.y + rowHeight);
            auto hovered = mousePos.x >= rowMin.x && mousePos.x <= rowMax.x && mousePos.y >= rowMin.y && mousePos.y <= rowMax.y;
            auto featured = roulette_event::featuredRewardIndex == i;
            auto guaranteed = roulette_event::guaranteedRewardIndex == i;

            auto fillColor = IM_COL32(41, 31, 25, hovered ? 235 : 205);
            auto borderColor = IM_COL32(161, 124, 77, hovered ? 220 : 150);
            if (guaranteed)
            {
                fillColor = IM_COL32(87, 57, 30, 220);
                borderColor = IM_COL32(247, 188, 104, 220);
            }
            else if (featured)
            {
                fillColor = IM_COL32(52, 63, 92, 220);
                borderColor = IM_COL32(118, 204, 255, 220);
            }

            drawList->AddRectFilled(rowMin, rowMax, fillColor, 8.0f);
            drawList->AddRect(rowMin, rowMax, borderColor, 8.0f, 0, hovered ? 1.7f : 1.0f);

            auto iconMin = ImVec2(rowMin.x + map_value(g_rouletteLayoutDebug.listIconOffset.x), rowMin.y + map_value(g_rouletteLayoutDebug.listIconOffset.y));
            auto iconMax = ImVec2(iconMin.x + map_value(g_rouletteLayoutDebug.listIconSize), iconMin.y + map_value(g_rouletteLayoutDebug.listIconSize));
            draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
            draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[i]);

            auto rewardName = get_reward_name(roulette_event::rewardType[i], roulette_event::rewardTypeId[i]);
            auto label = fit_text_to_width(
                std::string(rewardName) + " x" + std::to_string(std::max<int>(1, roulette_event::rewardCount[i])),
                rowWidth - map_value(g_rouletteLayoutDebug.listTextOffsetX + 10.0f));
            std::string meta;
            if (guaranteed)
                meta = "Guaranteed reward";
            else if (featured)
                meta = "Dragon's favorite";
            else
            {
                meta.clear();
            }
            meta = fit_text_to_width(meta, rowWidth - map_value(g_rouletteLayoutDebug.listTextOffsetX + 10.0f));
            drawList->AddText(ImVec2(rowMin.x + map_value(g_rouletteLayoutDebug.listTextOffsetX), rowMin.y + map_value(g_rouletteLayoutDebug.listLabelY)), IM_COL32(244, 235, 210, 255), label.c_str());
            if (!meta.empty())
                drawList->AddText(ImVec2(rowMin.x + map_value(g_rouletteLayoutDebug.listTextOffsetX), rowMin.y + map_value(g_rouletteLayoutDebug.listMetaY)), IM_COL32(189, 198, 208, 235), meta.c_str());

            if (hovered)
                draw_reward_tooltip(i);
        }

        auto guaranteedIndex = roulette_event::guaranteedRewardIndex == 0xFF ? -1 : roulette_event::guaranteedRewardIndex;
        if (guaranteedIndex >= 0 && guaranteedIndex < itemCount)
        {
            auto iconMin = map_point(g_rouletteLayoutDebug.guaranteedIconMin.x, g_rouletteLayoutDebug.guaranteedIconMin.y);
            auto iconMax = ImVec2(iconMin.x + map_value(g_rouletteLayoutDebug.guaranteedIconSize), iconMin.y + map_value(g_rouletteLayoutDebug.guaranteedIconSize));
            draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[guaranteedIndex], roulette_event::rewardTypeId[guaranteedIndex]);
            draw_item_count_badge(drawList, iconMin, iconMax, roulette_event::rewardCount[guaranteedIndex]);

            auto rewardName = get_reward_name(roulette_event::rewardType[guaranteedIndex], roulette_event::rewardTypeId[guaranteedIndex]);
            auto rewardLabel = fit_text_to_width(
                std::string(rewardName) + " x" + std::to_string(std::max<int>(1, roulette_event::rewardCount[guaranteedIndex])),
                map_value(g_rouletteLayoutDebug.guaranteedLabelWidth));
            auto rewardSize = ImGui::CalcTextSize(rewardLabel.c_str());
            drawList->AddText(map_point(g_rouletteLayoutDebug.guaranteedCenterX - rewardSize.x * 0.5f / scale, g_rouletteLayoutDebug.guaranteedLabelY), IM_COL32(245, 236, 214, 255), rewardLabel.c_str());
        }

        auto spinsText = roulette_event::guaranteedSpinTarget
            ? ("Spins: " + std::to_string(roulette_event::guaranteedSpinProgress) + " / " + std::to_string(roulette_event::guaranteedSpinTarget))
            : "Spins: -- / --";
        auto spinsTextSize = ImGui::CalcTextSize(spinsText.c_str());
        drawList->AddText(map_point(g_rouletteLayoutDebug.guaranteedCenterX - spinsTextSize.x * 0.5f / scale, g_rouletteLayoutDebug.guaranteedSpinsY), IM_COL32(198, 242, 118, 255), spinsText.c_str());

        auto hintText = roulette_event::guaranteedSpinTarget
            ? ("Spin " + std::to_string(std::max(0, roulette_event::guaranteedSpinTarget - roulette_event::guaranteedSpinProgress)) + " more time(s) to get the guaranteed top reward!")
            : std::string("No guaranteed reward configured.");
        auto hintClipped = fit_text_to_width(hintText, map_value(g_rouletteLayoutDebug.guaranteedHintWidth));
        auto hintSize = ImGui::CalcTextSize(hintClipped.c_str());
        drawList->AddText(map_point(g_rouletteLayoutDebug.guaranteedCenterX - hintSize.x * 0.5f / scale, g_rouletteLayoutDebug.guaranteedHintY), IM_COL32(232, 220, 194, 235), hintClipped.c_str());
        if (g_rouletteLayoutDebug.drawGuides)
        {
            auto guideMin = map_point(g_rouletteLayoutDebug.guaranteedCenterX - 167.0f, g_rouletteLayoutDebug.guaranteedIconMin.y - 12.0f);
            auto guideMax = map_point(g_rouletteLayoutDebug.guaranteedCenterX + 167.0f, g_rouletteLayoutDebug.guaranteedHintY + 30.0f);
            drawList->AddRect(guideMin, guideMax, IM_COL32(180, 0, 255, 160), 4.0f, 0, 1.0f);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.guaranteedIconMin.x, g_rouletteLayoutDebug.guaranteedIconMin.y), 5.0f, IM_COL32(180, 0, 255, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedLabelY), 5.0f, IM_COL32(180, 0, 255, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedSpinsY), 5.0f, IM_COL32(180, 0, 255, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedHintY), 5.0f, IM_COL32(180, 0, 255, 220), 12);
        }

        auto winnerCount = std::min<int>(roulette_event::recentWinnerCount, 5);
        for (int i = 0; i < winnerCount && i < static_cast<int>(roulette_event::recentWinnerName.size()); ++i)
        {
            auto rewardIndex = roulette_event::recentWinnerRewardIndex[i];
            auto rowMin = map_point(g_rouletteLayoutDebug.winnersStart.x, g_rouletteLayoutDebug.winnersStart.y + i * g_rouletteLayoutDebug.winnersRowStep);
            auto rowMax = map_point(g_rouletteLayoutDebug.winnersEndX, g_rouletteLayoutDebug.winnersStart.y + g_rouletteLayoutDebug.winnersRowHeight + i * g_rouletteLayoutDebug.winnersRowStep);
            if (rewardIndex >= 0 && rewardIndex < itemCount)
            {
                auto iconMin = ImVec2(rowMin.x + map_value(g_rouletteLayoutDebug.winnersIconOffset.x), rowMin.y + map_value(g_rouletteLayoutDebug.winnersIconOffset.y));
                auto iconMax = ImVec2(iconMin.x + map_value(g_rouletteLayoutDebug.winnersIconSize), iconMin.y + map_value(g_rouletteLayoutDebug.winnersIconSize));
                draw_item_icon_at(drawList, iconMin, iconMax, roulette_event::rewardType[rewardIndex], roulette_event::rewardTypeId[rewardIndex]);
            }
            auto playerName = fit_text_to_width(std::string(roulette_event::recentWinnerName[i].data()), map_value(160.0f));
            auto ageText = std::to_string(roulette_event::recentWinnerAgeMinutes[i]) + "m";
            auto ageSize = ImGui::CalcTextSize(ageText.c_str());
            drawList->AddText(ImVec2(rowMin.x + map_value(g_rouletteLayoutDebug.winnersNameOffset.x), rowMin.y + map_value(g_rouletteLayoutDebug.winnersNameOffset.y)), IM_COL32(243, 246, 249, 255), playerName.c_str());
            drawList->AddText(ImVec2(rowMax.x - ageSize.x - map_value(g_rouletteLayoutDebug.winnersAgeRightPadding), rowMin.y + map_value(g_rouletteLayoutDebug.winnersNameOffset.y)), IM_COL32(76, 214, 255, 255), ageText.c_str());
        }
        if (!winnerCount)
            drawList->AddText(map_point(g_rouletteLayoutDebug.winnersStart.x + 22.0f, g_rouletteLayoutDebug.winnersStart.y + 8.0f), IM_COL32(170, 176, 184, 220), "No winners yet");
        if (g_rouletteLayoutDebug.drawGuides)
        {
            auto winnersGuideMin = map_point(g_rouletteLayoutDebug.winnersStart.x, g_rouletteLayoutDebug.winnersStart.y);
            auto winnersGuideMax = map_point(g_rouletteLayoutDebug.winnersEndX, g_rouletteLayoutDebug.winnersStart.y + 4.0f * g_rouletteLayoutDebug.winnersRowStep + g_rouletteLayoutDebug.winnersRowHeight);
            drawList->AddRect(winnersGuideMin, winnersGuideMax, IM_COL32(80, 180, 255, 170), 4.0f, 0, 1.0f);
            drawList->AddCircleFilled(winnersGuideMin, 5.0f, IM_COL32(80, 180, 255, 220), 12);
            drawList->AddCircleFilled(winnersGuideMax, 5.0f, IM_COL32(80, 180, 255, 220), 12);
        }

        auto balanceLabel = std::to_string(tokenCount) + " TP";
        auto balanceSize = ImGui::CalcTextSize(balanceLabel.c_str());
        drawList->AddText(map_point(g_rouletteLayoutDebug.balanceCenter.x - (balanceSize.x / scale) * 0.5f, g_rouletteLayoutDebug.balanceCenter.y), IM_COL32(255, 246, 224, 255), balanceLabel.c_str());

        auto costLabel = std::to_string(requiredTokenCount) + " TP";
        auto costSize = ImGui::CalcTextSize(costLabel.c_str());
        drawList->AddText(map_point(g_rouletteLayoutDebug.costCenter.x - (costSize.x / scale) * 0.5f, g_rouletteLayoutDebug.costCenter.y), IM_COL32(255, 246, 224, 255), costLabel.c_str());

        auto footerHint = "Each spin costs " + std::to_string(requiredTokenCount) + " TP";
        auto footerSize = ImGui::CalcTextSize(footerHint.c_str());
        drawList->AddText(map_point(g_rouletteLayoutDebug.footerHintCenter.x - (footerSize.x / scale) * 0.5f, g_rouletteLayoutDebug.footerHintCenter.y), IM_COL32(216, 205, 183, 235), footerHint.c_str());

        auto buttonMin = map_point(g_rouletteLayoutDebug.buttonMin.x, g_rouletteLayoutDebug.buttonMin.y);
        auto buttonMax = map_point(g_rouletteLayoutDebug.buttonMax.x, g_rouletteLayoutDebug.buttonMax.y);
        auto buttonHovered = mousePos.x >= buttonMin.x && mousePos.x <= buttonMax.x
            && mousePos.y >= buttonMin.y && mousePos.y <= buttonMax.y;
        auto mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        auto clicked = buttonHovered && mouseDown && !g_rollMouseWasDown;
        g_rollMouseWasDown = mouseDown;
        if (g_rouletteLayoutDebug.drawGuides)
            drawList->AddRect(buttonMin, buttonMax, IM_COL32(255, 220, 0, 170), 6.0f, 0, 1.0f);
        if (g_rouletteLayoutDebug.drawGuides)
        {
            drawList->AddCircleFilled(buttonMin, 5.0f, IM_COL32(255, 220, 0, 220), 12);
            drawList->AddCircleFilled(buttonMax, 5.0f, IM_COL32(255, 220, 0, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.balanceCenter.x, g_rouletteLayoutDebug.balanceCenter.y), 5.0f, IM_COL32(255, 220, 0, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.costCenter.x, g_rouletteLayoutDebug.costCenter.y), 5.0f, IM_COL32(255, 220, 0, 220), 12);
            drawList->AddCircleFilled(map_point(g_rouletteLayoutDebug.footerHintCenter.x, g_rouletteLayoutDebug.footerHintCenter.y), 5.0f, IM_COL32(255, 220, 0, 220), 12);
        }

        if (g_rouletteLayoutDebug.show)
        {
            for (size_t i = 0; i < g_rouletteLayoutDebug.slotCenters.size(); ++i)
            {
                auto id = "##roulette_slot_drag_" + std::to_string(i);
                drag_ref_point(id.c_str(), g_rouletteLayoutDebug.slotCenters[i], origin, scale);
            }

            drag_ref_rect(
                "##roulette_list_move",
                "##roulette_list_resize",
                g_rouletteLayoutDebug.listMin,
                g_rouletteLayoutDebug.listSize,
                origin,
                scale);
            drag_ref_point("##roulette_guaranteed_icon", g_rouletteLayoutDebug.guaranteedIconMin, origin, scale);
            auto guaranteedLabelPoint = ImVec2(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedLabelY);
            if (drag_ref_point("##roulette_guaranteed_label", guaranteedLabelPoint, origin, scale))
            {
                g_rouletteLayoutDebug.guaranteedCenterX = guaranteedLabelPoint.x;
                g_rouletteLayoutDebug.guaranteedLabelY = guaranteedLabelPoint.y;
            }
            auto guaranteedSpinsPoint = ImVec2(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedSpinsY);
            if (drag_ref_point("##roulette_guaranteed_spins", guaranteedSpinsPoint, origin, scale))
            {
                g_rouletteLayoutDebug.guaranteedCenterX = guaranteedSpinsPoint.x;
                g_rouletteLayoutDebug.guaranteedSpinsY = guaranteedSpinsPoint.y;
            }
            auto guaranteedHintPoint = ImVec2(g_rouletteLayoutDebug.guaranteedCenterX, g_rouletteLayoutDebug.guaranteedHintY);
            if (drag_ref_point("##roulette_guaranteed_hint", guaranteedHintPoint, origin, scale))
            {
                g_rouletteLayoutDebug.guaranteedCenterX = guaranteedHintPoint.x;
                g_rouletteLayoutDebug.guaranteedHintY = guaranteedHintPoint.y;
            }

            auto winnersSizeRef = ImVec2(
                g_rouletteLayoutDebug.winnersEndX - g_rouletteLayoutDebug.winnersStart.x,
                4.0f * g_rouletteLayoutDebug.winnersRowStep + g_rouletteLayoutDebug.winnersRowHeight);
            if (drag_ref_rect(
                "##roulette_winners_move",
                "##roulette_winners_resize",
                g_rouletteLayoutDebug.winnersStart,
                winnersSizeRef,
                origin,
                scale))
            {
                g_rouletteLayoutDebug.winnersEndX = g_rouletteLayoutDebug.winnersStart.x + winnersSizeRef.x;
                g_rouletteLayoutDebug.winnersRowStep = std::max(20.0f, (winnersSizeRef.y - g_rouletteLayoutDebug.winnersRowHeight) / 4.0f);
            }

            drag_ref_point("##roulette_balance_center", g_rouletteLayoutDebug.balanceCenter, origin, scale);
            drag_ref_point("##roulette_cost_center", g_rouletteLayoutDebug.costCenter, origin, scale);
            drag_ref_point("##roulette_footer_center", g_rouletteLayoutDebug.footerHintCenter, origin, scale);
            drag_ref_point("##roulette_button_min", g_rouletteLayoutDebug.buttonMin, origin, scale);
            drag_ref_point("##roulette_button_max", g_rouletteLayoutDebug.buttonMax, origin, scale);
        }

        if (buttonHovered)
        {
            auto alpha = mouseDown ? 120 : static_cast<int>(70.0f + 22.0f * std::sin(static_cast<float>(now) * 0.0045f));
            drawList->AddRectFilled(buttonMin, buttonMax, IM_COL32(255, 210, 110, alpha), 12.0f);
        }

        auto stateText = "";
        if (stateText[0] != '\0')
        {
            auto stateSize = ImGui::CalcTextSize(stateText);
            drawList->AddText(
                ImVec2(buttonMin.x + (buttonMax.x - buttonMin.x - stateSize.x) * 0.5f, buttonMin.y + map_value(24.0f)),
                IM_COL32(255, 248, 231, 245),
                stateText);
        }

        if (clicked && canRoll)
            request_roulette_spin();
    }

    void draw_roulette_section()
    {
        auto now = GetTickCount();
        if (is_game_scene() && (!roulette_event::listReceived || now - g_lastRouletteListTick > 15000))
            request_roulette_list();

        update_roulette_spin_state();

        auto tokenType = roulette_event::tokenType;
        auto tokenTypeId = roulette_event::tokenTypeId;
        auto requiredTokenCount = roulette_event::tokenCount ? roulette_event::tokenCount : 1;
        auto tokenCount = tokenType ? count_inventory_item(tokenType, tokenTypeId) : 0;
        auto itemCount = roulette_item_count();
        auto canRoll = is_game_scene() && roulette_event::hasList && itemCount > 0 && tokenCount >= requiredTokenCount && !roulette_event::spinActive;

        draw_roulette_composite_panel(canRoll, tokenCount, requiredTokenCount);
    }

    void draw_panel_shell()
    {
        auto scaledW = kPanelFixedW * g_panelScale;
        auto scaledH = kPanelFixedH * g_panelScale;
        auto displaySize = ImGui::GetIO().DisplaySize;
        g_panelPosition = ImVec2(
            std::clamp(g_panelPosition.x, 0.0f, std::max(0.0f, displaySize.x - scaledW)),
            std::clamp(g_panelPosition.y, 0.0f, std::max(0.0f, displaySize.y - scaledH)));

        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(g_panelPosition, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(scaledW, scaledH), ImGuiCond_Always);

        bool panelOpen = g_showPanel;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin(
            "##roulette_panel",
            &panelOpen,
            ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus))
        {
            auto windowPos = ImGui::GetWindowPos();
            g_panelPosition = windowPos;
            g_panelWindowRect.left = static_cast<LONG>(windowPos.x);
            g_panelWindowRect.top = static_cast<LONG>(windowPos.y);
            g_panelWindowRect.right = static_cast<LONG>(windowPos.x + scaledW);
            g_panelWindowRect.bottom = static_cast<LONG>(windowPos.y + scaledH);
            draw_roulette_section();

            auto drawList = ImGui::GetWindowDrawList();
            auto moveHandleSize = 18.0f;
            auto moveHandleMin = ImVec2(windowPos.x + 4.0f, windowPos.y + 4.0f);
            auto moveHandleMax = ImVec2(moveHandleMin.x + moveHandleSize, moveHandleMin.y + moveHandleSize);
            ImGui::SetCursorScreenPos(moveHandleMin);
            ImGui::InvisibleButton("##roulette_move_handle", ImVec2(moveHandleSize, moveHandleSize));
            auto moveHovered = ImGui::IsItemHovered();
            auto moveActive = ImGui::IsItemActive();
            auto moveMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            auto moveMouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

            if (ImGui::IsItemActivated())
            {
                g_draggingPanel = true;
                g_draggedPanel = false;
                g_panelDragOffset = ImVec2(
                    ImGui::GetIO().MousePos.x - g_panelPosition.x,
                    ImGui::GetIO().MousePos.y - g_panelPosition.y);
            }

            if (g_draggingPanel && moveActive && moveMouseDown)
            {
                auto mouse = ImGui::GetIO().MousePos;
                auto newPosition = ImVec2(mouse.x - g_panelDragOffset.x, mouse.y - g_panelDragOffset.y);
                newPosition = ImVec2(
                    std::clamp(newPosition.x, 0.0f, std::max(0.0f, displaySize.x - scaledW)),
                    std::clamp(newPosition.y, 0.0f, std::max(0.0f, displaySize.y - scaledH)));

                if (std::fabs(newPosition.x - g_panelPosition.x) >= 0.5f || std::fabs(newPosition.y - g_panelPosition.y) >= 0.5f)
                {
                    g_panelPosition = newPosition;
                    g_draggedPanel = true;
                }
            }

            if (moveMouseReleased && g_draggingPanel)
            {
                if (g_draggedPanel)
                    mark_imgui_settings_dirty();
                g_draggingPanel = false;
                g_draggedPanel = false;
            }

            auto handleSize = 18.0f;
            auto handleMin = ImVec2(windowPos.x + scaledW - handleSize - 4.0f, windowPos.y + scaledH - handleSize - 4.0f);
            auto handleMax = ImVec2(handleMin.x + handleSize, handleMin.y + handleSize);
            ImGui::SetCursorScreenPos(handleMin);
            ImGui::InvisibleButton("##roulette_scale_handle", ImVec2(handleSize, handleSize));
            auto hovered = ImGui::IsItemHovered();
            auto active = ImGui::IsItemActive();
            auto mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            auto mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

            if (ImGui::IsItemActivated())
            {
                g_draggingPanelScale = true;
                g_draggedPanelScale = false;
                g_panelScaleDragStart = g_panelScale;
            }

            if (g_draggingPanelScale && active && mouseDown)
            {
                auto mouse = ImGui::GetIO().MousePos;
                auto newScale = std::max(
                    (mouse.x - windowPos.x) / kPanelFixedW,
                    (mouse.y - windowPos.y) / kPanelFixedH);
                newScale = std::clamp(newScale, 0.45f, 1.15f);
                if (std::fabs(newScale - g_panelScale) >= 0.0025f)
                {
                    g_panelScale = newScale;
                    g_draggedPanelScale = true;
                }
            }

            if (mouseReleased && g_draggingPanelScale)
            {
                if (g_draggedPanelScale)
                    mark_imgui_settings_dirty();
                g_draggingPanelScale = false;
                g_draggedPanelScale = false;
            }

            auto lineColor = hovered || g_draggingPanelScale
                ? IM_COL32(255, 226, 148, 255)
                : IM_COL32(196, 162, 102, 210);
            auto moveLineColor = moveHovered || g_draggingPanel
                ? IM_COL32(255, 226, 148, 255)
                : IM_COL32(196, 162, 102, 210);
            drawList->AddLine(ImVec2(moveHandleMin.x, moveHandleMin.y + 12.0f), ImVec2(moveHandleMin.x + 12.0f, moveHandleMin.y), moveLineColor, 1.5f);
            drawList->AddLine(ImVec2(moveHandleMin.x, moveHandleMin.y + 8.0f), ImVec2(moveHandleMin.x + 8.0f, moveHandleMin.y), moveLineColor, 1.5f);
            drawList->AddLine(ImVec2(moveHandleMin.x, moveHandleMin.y + 4.0f), ImVec2(moveHandleMin.x + 4.0f, moveHandleMin.y), moveLineColor, 1.5f);
            drawList->AddLine(ImVec2(handleMax.x - 12.0f, handleMax.y), ImVec2(handleMax.x, handleMax.y - 12.0f), lineColor, 1.5f);
            drawList->AddLine(ImVec2(handleMax.x - 8.0f, handleMax.y), ImVec2(handleMax.x, handleMax.y - 8.0f), lineColor, 1.5f);
            drawList->AddLine(ImVec2(handleMax.x - 4.0f, handleMax.y), ImVec2(handleMax.x, handleMax.y - 4.0f), lineColor, 1.5f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        g_showPanel = panelOpen;
    }

    void draw_roulette_button_overlay()
    {
        auto& io = ImGui::GetIO();
        if (!is_overlay_display_usable(io.DisplaySize))
            return;

        auto buttonSize = kRouletteButtonSize;
        g_rouletteButtonPosition = clamp_window_position(g_rouletteButtonPosition, buttonSize, io.DisplaySize);

        ImGui::SetNextWindowPos(g_rouletteButtonPosition, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin(
            "##roulette_button_overlay",
            nullptr,
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus
            | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::InvisibleButton("##roulette_toggle", buttonSize);

        auto min = ImGui::GetItemRectMin();
        auto max = ImGui::GetItemRectMax();
        remember_rect(g_rouletteButtonRect, min, max);

        auto hovered = ImGui::IsItemHovered() || is_cursor_in_rect(g_rouletteButtonRect);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            g_showPanel = !g_showPanel;

        auto drawList = ImGui::GetWindowDrawList();
        auto* texture = load_roulette_icon_texture();
        auto tint = IM_COL32(255, 255, 255, hovered ? 255 : 230);
        if (texture)
        {
            drawList->AddImage(reinterpret_cast<ImTextureID>(texture), min, max,
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        }
        else
        {
            draw_icon_button_fallback(drawList, min, max, tint, IM_COL32(31, 24, 37, 225), "O");
        }

        if (hovered)
            drawList->AddRectFilled(min, max, IM_COL32(255, 255, 255, 28), 4.0f);

        if (hovered)
        {
            ImGui::SetNextWindowPos(ImVec2(min.x - 2.0f, min.y - 24.0f), ImGuiCond_Always);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(g_showPanel ? "Hide roulette" : "Show roulette");
            ImGui::EndTooltip();
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
    }

} // namespace imgui_layer
