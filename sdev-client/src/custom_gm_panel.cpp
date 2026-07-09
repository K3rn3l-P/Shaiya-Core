#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <external/imgui/imgui.h>
#include "include/main.h"
#include "include/GmCommandNative.h"
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/CPlayerData.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/Static.h"

using namespace shaiya;

// ---------------------------------------------------------------------------
// F7 Custom GM Panel - preserves custom features across upstream merges
// 
// Includes:
// - EFT Browser: search and play custom .eft files from data.sah
// - Item Search: advanced item catalog with filters
// - GM Commands: quick reference for GM commands
// - Screen Capture: Ctrl+LMB coordinate capture tool
// ---------------------------------------------------------------------------

namespace custom_gm_panel
{
    // -----------------------------------------------------------------------
    // Panel state
    // -----------------------------------------------------------------------
    static bool g_showCustomPanel = false;
    static bool g_f7Down = false;

    static constexpr float kPanelW = 720.0f;
    static constexpr float kPanelH = 560.0f;

    // -----------------------------------------------------------------------
    // Helper functions
    // -----------------------------------------------------------------------
    std::string to_lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string get_game_relative_path(const char* relativePath)
    {
        char modulePath[MAX_PATH]{};
        if (GetModuleFileNameA(nullptr, modulePath, sizeof(modulePath)) == 0)
            return relativePath ? relativePath : "";

        auto path = std::string(modulePath);
        auto slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return relativePath ? relativePath : "";

        path.resize(slash + 1);
        if (relativePath)
            path += relativePath;

        return path;
    }

    static bool is_admin()
    {
        return g_pPlayerData && g_pPlayerData->isAdmin;
    }

    // -----------------------------------------------------------------------
    // EFT Browser
    // -----------------------------------------------------------------------
    struct EftEntry
    {
        std::string path;
        std::string fileName;
        std::string lowerFileName;
    };

    static bool g_eftCatalogLoaded = false;
    static std::vector<EftEntry> g_eftEntries;
    static int g_selectedEftIndex = -1;
    static bool g_eftLoop = false;
    static int g_eftLoopMs = 1200;
    static std::array<char, 128> g_eftSearch{};

    bool read_sah_u32(const std::vector<char>& data, std::size_t& offset, uint32_t& value)
    {
        if (offset + sizeof(value) > data.size())
            return false;

        std::memcpy(&value, data.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    }

    bool read_sah_u64(const std::vector<char>& data, std::size_t& offset, uint64_t& value)
    {
        if (offset + sizeof(value) > data.size())
            return false;

        std::memcpy(&value, data.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    }

    bool read_sah_string(const std::vector<char>& data, std::size_t& offset, std::string& value)
    {
        uint32_t length = 0;
        if (!read_sah_u32(data, offset, length) || offset + length > data.size())
            return false;

        value.assign(data.data() + offset, data.data() + offset + length);
        while (!value.empty() && value.back() == '\0')
            value.pop_back();

        offset += length;
        return true;
    }

    bool is_sah_effect_folder(const std::string& lowerPath)
    {
        return lowerPath == "effect"
            || lowerPath == "data\\effect"
            || lowerPath.rfind("effect\\", 0) == 0
            || lowerPath.rfind("data\\effect\\", 0) == 0;
    }

    void try_add_eft_file(const std::string& lowerPath, const std::string& fileName)
    {
        auto lowerFileName = to_lower_ascii(fileName);
        if (lowerFileName.size() < 4 || lowerFileName.substr(lowerFileName.size() - 4) != ".eft")
            return;

        if (!is_sah_effect_folder(lowerPath))
            return;

        auto it = std::find_if(g_eftEntries.begin(), g_eftEntries.end(), [&](const EftEntry& entry) {
            return entry.lowerFileName == lowerFileName;
        });
        if (it != g_eftEntries.end())
            return;

        g_eftEntries.push_back({ lowerPath, fileName, lowerFileName });
    }

    bool scan_sah_directory_for_efts(const std::vector<char>& data, std::size_t& offset, const std::string& parentPath)
    {
        std::string name;
        if (!read_sah_string(data, offset, name))
            return false;

        auto path = parentPath;
        if (!name.empty())
            path = path.empty() ? name : path + "\\" + name;

        uint32_t fileCount = 0;
        if (!read_sah_u32(data, offset, fileCount))
            return false;

        auto lowerPath = to_lower_ascii(path);
        for (uint32_t i = 0; i < fileCount; ++i)
        {
            std::string fileName;
            uint64_t fileOffset = 0;
            uint64_t fileSize = 0;
            if (!read_sah_string(data, offset, fileName)
                || !read_sah_u64(data, offset, fileOffset)
                || !read_sah_u64(data, offset, fileSize))
            {
                return false;
            }

            try_add_eft_file(lowerPath, fileName);
        }

        uint32_t directoryCount = 0;
        if (!read_sah_u32(data, offset, directoryCount))
            return false;

        for (uint32_t i = 0; i < directoryCount; ++i)
        {
            if (!scan_sah_directory_for_efts(data, offset, path))
                return false;
        }

        return true;
    }

    void ensure_eft_catalog_loaded()
    {
        if (g_eftCatalogLoaded)
            return;

        g_eftCatalogLoaded = true;
        auto sahPath = get_game_relative_path("data.sah");
        std::ifstream stream(sahPath, std::ios::binary);
        if (!stream)
            return;

        std::vector<char> data(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        if (data.size() <= 0x34 || std::memcmp(data.data(), "SAH", 3) != 0)
            return;

        auto offset = std::size_t{ 0x34 };
        if (!scan_sah_directory_for_efts(data, offset, ""))
            return;

        std::sort(g_eftEntries.begin(), g_eftEntries.end(), [](const EftEntry& lhs, const EftEntry& rhs) {
            return lhs.lowerFileName < rhs.lowerFileName;
        });
    }

    void draw_eft_browser()
    {
        ensure_eft_catalog_loaded();

        ImGui::Text("Archive EFT count: %d", static_cast<int>(g_eftEntries.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh##eft"))
        {
            g_eftCatalogLoaded = false;
            g_eftEntries.clear();
            g_selectedEftIndex = -1;
            ensure_eft_catalog_loaded();
        }

        ImGui::InputTextWithHint("##eft_search", "Search .eft...", g_eftSearch.data(), g_eftSearch.size());

        ImGui::Checkbox("Loop", &g_eftLoop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("Loop ms", &g_eftLoopMs, 100, 500);
        if (g_eftLoopMs < 100)
            g_eftLoopMs = 100;
        else if (g_eftLoopMs > 10000)
            g_eftLoopMs = 10000;

        auto currentName = get_custom_eft_name_from_ui();
        ImGui::TextDisabled("Current: %s", currentName && currentName[0] ? currentName : "(none)");
        if (is_custom_eft_loop_enabled_from_ui())
            ImGui::TextDisabled("Loop active: %u ms", get_custom_eft_loop_interval_from_ui());

        const EftEntry* selectedEntry = nullptr;
        if (g_selectedEftIndex >= 0 && g_selectedEftIndex < static_cast<int>(g_eftEntries.size()))
            selectedEntry = &g_eftEntries[g_selectedEftIndex];

        if (ImGui::Button("Play Selected##eft", ImVec2(130.0f, 0.0f)) && selectedEntry)
            play_custom_eft_from_ui(selectedEntry->fileName.c_str(), g_eftLoop, static_cast<unsigned>(g_eftLoopMs));
        ImGui::SameLine();
        if (ImGui::Button("Stop##eft", ImVec2(90.0f, 0.0f)))
            stop_custom_eft_from_ui();

        ImGui::Separator();

        auto search = to_lower_ascii(g_eftSearch.data());
        ImGui::BeginChild("eft_list_child", ImVec2(0.0f, 0.0f), true);

        if (!search.empty())
        {
            for (int i = 0; i < static_cast<int>(g_eftEntries.size()); ++i)
            {
                auto& entry = g_eftEntries[i];
                if (!search.empty()
                    && entry.lowerFileName.find(search) == std::string::npos
                    && entry.path.find(search) == std::string::npos)
                {
                    continue;
                }

                auto selected = g_selectedEftIndex == i;
                auto label = entry.fileName + "##eft_" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), selected))
                    g_selectedEftIndex = i;

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", entry.path.c_str());

                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    play_custom_eft_from_ui(entry.fileName.c_str(), g_eftLoop, static_cast<unsigned>(g_eftLoopMs));
            }
        }
        else
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(g_eftEntries.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    auto& entry = g_eftEntries[i];
                    auto selected = g_selectedEftIndex == i;
                    auto label = entry.fileName + "##eft_" + std::to_string(i);
                    if (ImGui::Selectable(label.c_str(), selected))
                        g_selectedEftIndex = i;

                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", entry.path.c_str());

                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        play_custom_eft_from_ui(entry.fileName.c_str(), g_eftLoop, static_cast<unsigned>(g_eftLoopMs));
                }
            }
        }
        ImGui::EndChild();
    }

    // -----------------------------------------------------------------------
    // GM Commands
    // -----------------------------------------------------------------------
    static std::array<char, 128> g_gmCommandSearch{};

    static const char* category_to_label(shaiya::gm::Category category)
    {
        switch (category)
        {
        case shaiya::gm::Category::Mode: return "Mode";
        case shaiya::gm::Category::Movement: return "Movement";
        case shaiya::gm::Category::Moderation: return "Moderation";
        case shaiya::gm::Category::Notice: return "Notice";
        case shaiya::gm::Category::SpawnItem: return "SpawnItem";
        case shaiya::gm::Category::Guild: return "Guild";
        case shaiya::gm::Category::QuestEvent: return "QuestEvent";
        case shaiya::gm::Category::Server: return "Server";
        case shaiya::gm::Category::WorldDebug: return "WorldDebug";
        case shaiya::gm::Category::Auction: return "Auction";
        case shaiya::gm::Category::AdminPower: return "AdminPower";
        case shaiya::gm::Category::ExtendsCommand: return "ExtendsCommand";
        default: return "Other";
        }
    }

    static void post_gm_command_to_chat(const char* command)
    {
        ensure_client_sysmsg_dispatch_ready();
        if (!command || !command[0] || !g_var->hwnd || !IsWindow(g_var->hwnd))
            return;

        SendMessageA(g_var->hwnd, kClientGMCommandWindowMessage, 0, reinterpret_cast<LPARAM>(command));
    }

    static bool native_command_matches_search(const shaiya::gm::CommandEntry& entry, const std::string& lowerSearch)
    {
        if (lowerSearch.empty())
            return true;

        auto lowerPrefix = to_lower_ascii(entry.prefix ? entry.prefix : "");
        auto lowerCategory = to_lower_ascii(category_to_label(entry.category));
        auto lowerSummary = to_lower_ascii(entry.summary ? entry.summary : "");
        return lowerPrefix.find(lowerSearch) != std::string::npos
            || lowerCategory.find(lowerSearch) != std::string::npos
            || lowerSummary.find(lowerSearch) != std::string::npos;
    }

    static bool extends_command_matches_search(const shaiya::gm::extendsCommands::Entry& entry, const std::string& lowerSearch)
    {
        if (lowerSearch.empty())
            return true;

        auto lowerCommand = to_lower_ascii(entry.command ? entry.command : "");
        auto lowerSummary = to_lower_ascii(entry.summary ? entry.summary : "");
        return lowerCommand.find(lowerSearch) != std::string::npos
            || lowerSummary.find(lowerSearch) != std::string::npos;
    }

    static void draw_gm_commands()
    {
        ImGui::InputTextWithHint("##gm_cmd_search", "Search command...", g_gmCommandSearch.data(), g_gmCommandSearch.size());
        auto search = to_lower_ascii(g_gmCommandSearch.data());
        ImGui::TextDisabled("Click a command to replace text in chat input.");

        constexpr auto categories = std::to_array({
            shaiya::gm::Category::Mode,
            shaiya::gm::Category::Movement,
            shaiya::gm::Category::Moderation,
            shaiya::gm::Category::Notice,
            shaiya::gm::Category::SpawnItem,
            shaiya::gm::Category::Guild,
            shaiya::gm::Category::QuestEvent,
            shaiya::gm::Category::Server,
            shaiya::gm::Category::WorldDebug,
            shaiya::gm::Category::Auction,
            shaiya::gm::Category::AdminPower,
            shaiya::gm::Category::ExtendsCommand,
            shaiya::gm::Category::Other,
        });

        for (auto category : categories)
        {
            bool hasVisible = false;

            for (std::size_t i = 0; i < shaiya::gm::kNativeCatalogCount; ++i)
            {
                const auto& entry = shaiya::gm::kNativeCatalog[i];
                if (entry.category == category && native_command_matches_search(entry, search))
                {
                    hasVisible = true;
                    break;
                }
            }

            if (!hasVisible && category == shaiya::gm::Category::ExtendsCommand)
            {
                for (std::size_t i = 0; i < shaiya::gm::extendsCommands::kExtendsCommandCount; ++i)
                {
                    const auto& entry = shaiya::gm::extendsCommands::kExtendsCommands[i];
                    if (extends_command_matches_search(entry, search))
                    {
                        hasVisible = true;
                        break;
                    }
                }
            }

            if (!hasVisible)
                continue;

            if (!ImGui::TreeNode(category_to_label(category)))
                continue;

            for (std::size_t i = 0; i < shaiya::gm::kNativeCatalogCount; ++i)
            {
                const auto& entry = shaiya::gm::kNativeCatalog[i];
                if (entry.category != category || !native_command_matches_search(entry, search))
                    continue;

                auto label = std::string(entry.prefix) + "##gm_native_" + std::to_string(i);
                if (ImGui::Selectable(label.c_str()))
                    post_gm_command_to_chat(entry.prefix);

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "%s\nRequired level: %u\nAction: 0x%02X",
                        entry.summary ? entry.summary : "",
                        static_cast<unsigned>(entry.required_level),
                        static_cast<unsigned>(entry.action_id));
                }
            }

            if (category == shaiya::gm::Category::ExtendsCommand)
            {
                for (std::size_t i = 0; i < shaiya::gm::extendsCommands::kExtendsCommandCount; ++i)
                {
                    const auto& entry = shaiya::gm::extendsCommands::kExtendsCommands[i];
                    if (!extends_command_matches_search(entry, search))
                        continue;

                    auto label = std::string(entry.command) + "##gm_ext_" + std::to_string(i);
                    if (ImGui::Selectable(label.c_str()))
                        post_gm_command_to_chat(entry.command);

                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", entry.summary ? entry.summary : "");
                }
            }

            ImGui::TreePop();
        }
    }

    // -----------------------------------------------------------------------
    // Item Search
    // -----------------------------------------------------------------------
    struct ItemSearchEntry
    {
        int type;
        int typeId;
        int itemId;
        int level;
        int grade;
        std::string name;
        std::string lowerName;
    };

    static bool g_itemCatalogLoaded = false;
    static std::vector<ItemSearchEntry> g_itemEntries;
    static std::array<char, 128> g_itemSearch{};
    static int g_itemSearchPage = 0;
    static constexpr int kItemSearchPageSize = 25;

    static bool g_filterArmor = false;
    static bool g_filterWeapons = false;
    static bool g_filterAccessories = false;
    static bool g_filterLapis = false;
    static bool g_filterCapes = false;
    static bool g_filterMounts = false;
    static bool g_filterConsumables = false;
    static bool g_filterPets = false;
    static bool g_filterWings = false;
    static bool g_filterCostumes = false;

    static int g_factionFilter = 0;
    static int g_minLevel = 0;
    static int g_maxLevel = 80;
    static bool g_enableLevelFilter = false;
    static bool g_hideUnnamedItems = true;
    static std::array<char, 7> g_gradeFilter{};
    static bool g_enableGradeFilter = false;

    static bool is_light_armor_type(int type)
    {
        return type == 16 || type == 17 || type == 18 || type == 20 || type == 21
            || type == 67 || type == 68 || type == 70 || type == 71 || type == 72
            || type == 73 || type == 74 || type == 76 || type == 77;
    }

    static bool is_fury_armor_type(int type)
    {
        return type == 31 || type == 32 || type == 33 || type == 35 || type == 36
            || type == 82 || type == 83 || type == 85 || type == 86 || type == 87
            || type == 88 || type == 89 || type == 91 || type == 92;
    }

    static bool is_armor_type(int type)
    {
        return is_light_armor_type(type) || is_fury_armor_type(type);
    }

    static bool is_light_weapon_type(int type)
    {
        return type == 1 || type == 2 || type == 5 || type == 6 || type == 7
            || type == 8 || type == 9 || type == 10 || type == 12 || type == 13
            || type == 14 || type == 15 || type == 19 || type == 45 || type == 46
            || type == 49 || type == 51 || type == 53 || type == 55 || type == 57
            || type == 58 || type == 60 || type == 62 || type == 64 || type == 65
            || type == 69;
    }

    static bool is_fury_weapon_type(int type)
    {
        return type == 3 || type == 4 || type == 5 || type == 6 || type == 7
            || type == 8 || type == 9 || type == 10 || type == 11 || type == 12
            || type == 13 || type == 15 || type == 34 || type == 47 || type == 48
            || type == 50 || type == 52 || type == 54 || type == 56 || type == 57
            || type == 58 || type == 59 || type == 61 || type == 63 || type == 65
            || type == 84;
    }

    static bool is_weapon_type(int type)
    {
        return is_light_weapon_type(type) || is_fury_weapon_type(type);
    }

    static bool is_accessory_type(int type)
    {
        return type == 22 || type == 23 || type == 37 || type == 40 || type == 96 || type == 97;
    }

    static bool is_lapis_type(int type)
    {
        return type == 30 || type == 95;
    }

    static bool is_cape_type(int type)
    {
        return type == 24 || type == 39;
    }

    static bool is_mount_type(int type)
    {
        return type == 42;
    }

    static bool is_consumable_type(int type)
    {
        return type == 25 || type == 27 || type == 28 || type == 29 || type == 38
            || type == 41 || type == 43 || type == 44 || type == 94 || type == 99
            || type == 100 || type == 101 || type == 102;
    }

    static bool is_pet_type(int type)
    {
        return type == 120;
    }

    static bool is_wing_type(int type)
    {
        return type == 121;
    }

    static bool is_costume_type(int type)
    {
        return type == 150;
    }

    static void send_getitem_type_typeid_to_chat(int type, int typeId)
    {
        ensure_client_sysmsg_dispatch_ready();
        if (!g_var->hwnd || !IsWindow(g_var->hwnd))
            return;

        char command[64]{};
        sprintf_s(command, "/getitem %d %d 1", type, typeId);
        SendMessageA(g_var->hwnd, kClientGMCommandWindowMessage, 0, reinterpret_cast<LPARAM>(command));
    }
    
    static void ensure_item_catalog_loaded()
    {
        if (g_itemCatalogLoaded)
            return;

        g_itemCatalogLoaded = true;
        g_itemEntries.clear();
        g_itemEntries.reserve(4096);

        for (int type = 1; type <= 255; ++type)
        {
            for (int typeId = 1; typeId <= 255; ++typeId)
            {
                auto* itemInfo = CDataFile::GetItemInfo(type, typeId);
                if (!itemInfo)
                    continue;

                std::string name = itemInfo->name ? itemInfo->name : "";
                if (name.empty())
                    name = "(unnamed)";

                g_itemEntries.push_back({
                    type,
                    typeId,
                    type * 1000 + typeId,
                    static_cast<int>(itemInfo->level),
                    static_cast<int>(itemInfo->dropGrade),
                    name,
                    to_lower_ascii(name)
                });
            }
        }

        std::sort(g_itemEntries.begin(), g_itemEntries.end(), [](const ItemSearchEntry& lhs, const ItemSearchEntry& rhs) {
            if (lhs.itemId != rhs.itemId)
                return lhs.itemId < rhs.itemId;
            return lhs.lowerName < rhs.lowerName;
        });

        g_itemSearchPage = 0;
    }

    static bool item_matches_search(const ItemSearchEntry& entry, const std::string& lowerSearch)
    {
        if (lowerSearch.empty())
            return true;

        if (entry.lowerName.find(lowerSearch) != std::string::npos)
            return true;

        if (std::to_string(entry.itemId).find(lowerSearch) != std::string::npos)
            return true;

        if (std::to_string(entry.type).find(lowerSearch) != std::string::npos)
            return true;

        return std::to_string(entry.typeId).find(lowerSearch) != std::string::npos;
    }

    static bool item_matches_faction_filter(const ItemSearchEntry& entry)
    {
        if (g_factionFilter == 0)
            return true;

        bool isLight = is_light_armor_type(entry.type) || is_light_weapon_type(entry.type);
        bool isFury = is_fury_armor_type(entry.type) || is_fury_weapon_type(entry.type);

        if (g_factionFilter == 1)
            return isLight;
        if (g_factionFilter == 2)
            return isFury;

        return !isLight && !isFury;
    }

    static bool item_matches_level_filter(const ItemSearchEntry& entry)
    {
        if (!g_enableLevelFilter)
            return true;

        return entry.level >= g_minLevel && entry.level <= g_maxLevel;
    }

    static bool item_matches_unnamed_filter(const ItemSearchEntry& entry)
    {
        if (!g_hideUnnamedItems)
            return true;

        return entry.name.find("???") == std::string::npos;
    }

    static bool item_matches_grade_filter(const ItemSearchEntry& entry)
    {
        if (!g_enableGradeFilter || g_gradeFilter[0] == '\0')
            return true;

        try {
            int filterGrade = std::stoi(g_gradeFilter.data());
            return entry.grade == filterGrade;
        }
        catch (...) {
            return true;
        }
    }

    static bool item_matches_category_filter(const ItemSearchEntry& entry)
    {
        bool anyFilterActive = g_filterArmor || g_filterWeapons || g_filterAccessories
            || g_filterLapis || g_filterCapes || g_filterMounts || g_filterConsumables
            || g_filterPets || g_filterWings || g_filterCostumes;

        if (!anyFilterActive)
            return true;

        if (g_filterArmor && is_armor_type(entry.type))
            return true;
        if (g_filterWeapons && is_weapon_type(entry.type))
            return true;
        if (g_filterAccessories && is_accessory_type(entry.type))
            return true;
        if (g_filterLapis && is_lapis_type(entry.type))
            return true;
        if (g_filterCapes && is_cape_type(entry.type))
            return true;
        if (g_filterMounts && is_mount_type(entry.type))
            return true;
        if (g_filterConsumables && is_consumable_type(entry.type))
            return true;
        if (g_filterPets && is_pet_type(entry.type))
            return true;
        if (g_filterWings && is_wing_type(entry.type))
            return true;
        if (g_filterCostumes && is_costume_type(entry.type))
            return true;

        return false;
    }

    static void draw_item_search()
    {
        ensure_item_catalog_loaded();

        ImGui::InputTextWithHint("##item_search_input", "Search by ItemID / Name / type / typeId...", g_itemSearch.data(), g_itemSearch.size());
        ImGui::SameLine();
        ImGui::Checkbox("Hide Unnamed (???)##unnamed", &g_hideUnnamedItems);

        if (ImGui::CollapsingHeader("Advanced Filters"))
        {
            ImGui::Indent();

            ImGui::TextDisabled("Faction Filter:");
            ImGui::RadioButton("Both##faction", &g_factionFilter, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Light##faction", &g_factionFilter, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Fury##faction", &g_factionFilter, 2);

            ImGui::Separator();
            ImGui::Checkbox("Enable Level Filter", &g_enableLevelFilter);
            if (g_enableLevelFilter)
            {
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputInt("Min##level", &g_minLevel, 1, 10);
                g_minLevel = std::clamp(g_minLevel, 0, 80);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputInt("Max##level", &g_maxLevel, 1, 10);
                g_maxLevel = std::clamp(g_maxLevel, 0, 80);
                if (g_minLevel > g_maxLevel)
                    g_minLevel = g_maxLevel;
            }

            ImGui::Separator();
            ImGui::Checkbox("Enable Grade Filter", &g_enableGradeFilter);
            if (g_enableGradeFilter)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputText("##grade_input", g_gradeFilter.data(), g_gradeFilter.size(), ImGuiInputTextFlags_CharsDecimal);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Category Filters:");
            ImGui::Checkbox("Armor", &g_filterArmor);
            ImGui::SameLine();
            ImGui::Checkbox("Weapons", &g_filterWeapons);
            ImGui::SameLine();
            ImGui::Checkbox("Accessories", &g_filterAccessories);
            ImGui::SameLine();
            ImGui::Checkbox("Lapis", &g_filterLapis);
            ImGui::SameLine();
            ImGui::Checkbox("Capes", &g_filterCapes);

            ImGui::Checkbox("Mounts", &g_filterMounts);
            ImGui::SameLine();
            ImGui::Checkbox("Consumables", &g_filterConsumables);
            ImGui::SameLine();
            ImGui::Checkbox("Pets", &g_filterPets);
            ImGui::SameLine();
            ImGui::Checkbox("Wings", &g_filterWings);
            ImGui::SameLine();
            ImGui::Checkbox("Costumes", &g_filterCostumes);

            ImGui::Unindent();
        }

        auto search = to_lower_ascii(g_itemSearch.data());
        std::vector<int> filteredIndices;
        filteredIndices.reserve(g_itemEntries.size());

        for (int i = 0; i < static_cast<int>(g_itemEntries.size()); ++i)
        {
            if (item_matches_search(g_itemEntries[i], search)
                && item_matches_category_filter(g_itemEntries[i])
                && item_matches_faction_filter(g_itemEntries[i])
                && item_matches_level_filter(g_itemEntries[i])
                && item_matches_unnamed_filter(g_itemEntries[i])
                && item_matches_grade_filter(g_itemEntries[i]))
            {
                filteredIndices.push_back(i);
            }
        }

        auto totalResults = static_cast<int>(filteredIndices.size());
        auto totalPages = std::max(1, (totalResults + kItemSearchPageSize - 1) / kItemSearchPageSize);
        g_itemSearchPage = std::clamp(g_itemSearchPage, 0, totalPages - 1);

        ImGui::TextDisabled("Results: %d", totalResults);
        ImGui::SameLine();
        ImGui::TextDisabled("Page %d/%d", g_itemSearchPage + 1, totalPages);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear All Filters##item_search"))
        {
            g_filterArmor = false;
            g_filterWeapons = false;
            g_filterAccessories = false;
            g_filterLapis = false;
            g_filterCapes = false;
            g_filterMounts = false;
            g_filterConsumables = false;
            g_filterPets = false;
            g_filterWings = false;
            g_filterCostumes = false;
            g_factionFilter = 0;
            g_enableLevelFilter = false;
            g_minLevel = 0;
            g_maxLevel = 80;
            g_hideUnnamedItems = false;
            g_enableGradeFilter = false;
            g_gradeFilter.fill('\0');
        }

        if (ImGui::SmallButton("Prev##item_page") && g_itemSearchPage > 0)
            --g_itemSearchPage;
        ImGui::SameLine();
        if (ImGui::SmallButton("Next##item_page") && g_itemSearchPage + 1 < totalPages)
            ++g_itemSearchPage;

        ImGui::Separator();
        ImGui::BeginChild("item_search_list", ImVec2(0.0f, 0.0f), true);

        if (totalResults == 0)
        {
            ImGui::TextDisabled("No items found.");
            ImGui::EndChild();
            return;
        }

        auto start = g_itemSearchPage * kItemSearchPageSize;
        auto end = std::min(start + kItemSearchPageSize, totalResults);
        for (int i = start; i < end; ++i)
        {
            const auto& entry = g_itemEntries[filteredIndices[i]];
            auto label = std::to_string(entry.itemId)
                + "  |  " + entry.name
                + "  |  Grade: " + std::to_string(entry.grade)
                + "  |  Level: " + std::to_string(entry.level)
                + "##item_search_" + std::to_string(entry.type) + "_" + std::to_string(entry.typeId);

            if (ImGui::Selectable(label.c_str(), false))
                send_getitem_type_typeid_to_chat(entry.type, entry.typeId);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Name: %s\nItemID: %d\nType: %d\nTypeID: %d\nLevel: %d\nGrade: %d\nClick: /getitem %d %d 1",
                    entry.name.c_str(),
                    entry.itemId,
                    entry.type,
                    entry.typeId,
                    entry.level,
                    entry.grade,
                    entry.type,
                    entry.typeId);
            }
        }

        ImGui::EndChild();
    }

    // -----------------------------------------------------------------------
    // Weapon Skin browser
    //   Mostra le skin definite dal server (whitelist, filtrate per classe).
    //   Click su una riga = invia "/getitem <type> <typeId> 1": prende l'item skin
    //   FISICO in inventario. La skin poi si equipaggia trascinando l'item sul box
    //   skin dell'inventario (server valida whitelist + classe/fazione + possesso).
    //   /wskin è stato RIMOSSO: nessun equip diretto, solo via item posseduto.
    // -----------------------------------------------------------------------
    static std::array<char, 128> g_skinSearch{};

    static void draw_weapon_skin()
    {
        // Current skin state
        if (weapon_skin::is_set())
        {
            ImGui::TextColored(ImVec4(0.31f, 0.63f, 1.0f, 1.0f),
                "Active skin: type %u  typeId %u",
                weapon_skin::skin_type(), weapon_skin::skin_type_id());
        }
        else
        {
            ImGui::TextDisabled("No active skin (real weapon model).");
        }
        ImGui::TextDisabled("Click a skin = /getitem (get the item), then drag it onto the "
                            "inventory skin box. Right-click the box to remove.");

        ImGui::Separator();
        ImGui::InputTextWithHint("##wskin_search", "Search skin by Name / type / typeId...",
            g_skinSearch.data(), g_skinSearch.size());

        auto search = to_lower_ascii(g_skinSearch.data());

        int total = weapon_skin::allowed_skin_count();
        ImGui::TextDisabled("Allowed skins: %d  (server, filtered by class)", total);

        ImGui::Separator();
        ImGui::BeginChild("wskin_list", ImVec2(0.0f, 0.0f), true);

        if (total == 0)
        {
            ImGui::TextDisabled("No skins available for your class.");
            ImGui::EndChild();
            return;
        }

        int shown = 0;
        for (int i = 0; i < total; ++i)
        {
            uint8_t type = 0, typeId = 0;
            bool usable = false;
            if (!weapon_skin::allowed_skin_get(i, &type, &typeId, &usable))
                continue;

            auto* info = CDataFile::GetItemInfo(type, typeId);
            std::string name = (info && info->name) ? info->name : "(?)";

            // filtro ricerca: nome / type / typeId
            if (!search.empty())
            {
                std::string hay = to_lower_ascii(name)
                    + " " + std::to_string(type) + " " + std::to_string(typeId);
                if (hay.find(search) == std::string::npos)
                    continue;
            }

            bool active = weapon_skin::is_set()
                && weapon_skin::skin_type() == type
                && weapon_skin::skin_type_id() == typeId;

            auto label = name
                + "  |  " + std::to_string(type) + ":" + std::to_string(typeId)
                + (active ? "   [ACTIVE]" : usable ? "" : "   [wrong class]")
                + "##wskin_" + std::to_string(type) + "_" + std::to_string(typeId);

            // Verde = equipaggiabile dal personaggio; rosso = classe/fazione errata
            // (il server la rifiuterebbe → "Cannot equip"). Admin vede comunque tutte.
            ImVec4 color = usable ? ImVec4(0.55f, 1.0f, 0.55f, 1.0f)
                                  : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            bool clicked = ImGui::Selectable(label.c_str(), active);
            ImGui::PopStyleColor();
            if (clicked)
                send_getitem_type_typeid_to_chat(type, typeId);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Name: %s\nType: %d\nTypeID: %d\n%s\nClick: /getitem %d %d 1  "
                    "(then drag onto inventory skin box)",
                    name.c_str(), type, typeId,
                    usable ? "Equippable by your class" : "NOT for your class (server will deny)",
                    type, typeId);
            }
            ++shown;
        }

        if (shown == 0)
            ImGui::TextDisabled("No skin matches the search.");

        ImGui::EndChild();
    }

    // -----------------------------------------------------------------------
    // Screen Coordinate Capture
    // -----------------------------------------------------------------------
    struct ScreenPointEntry
    {
        LONG x;
        LONG y;
        DWORD tick;
    };

    static constexpr int kMaxScreenPointEntries = 64;
    static ScreenPointEntry g_screenPointRing[kMaxScreenPointEntries]{};
    static int g_screenPointHead = 0;
    static int g_screenPointCount = 0;
    static int g_selectedScreenPointIdx = -1;

    static bool g_ctrlLmbCaptureEnabled = false;
    static bool g_captureClicksOnUi = false;
    static bool g_hasCapturedScreenPoint = false;
    static POINT g_lastCapturedScreenPoint{};

    static void push_screen_point(LONG x, LONG y, DWORD tick)
    {
        auto& entry = g_screenPointRing[g_screenPointHead % kMaxScreenPointEntries];
        entry.x = x;
        entry.y = y;
        entry.tick = tick;

        g_screenPointHead = (g_screenPointHead + 1) % kMaxScreenPointEntries;
        if (g_screenPointCount < kMaxScreenPointEntries)
            ++g_screenPointCount;

        g_selectedScreenPointIdx = (g_screenPointHead - 1 + kMaxScreenPointEntries) % kMaxScreenPointEntries;
    }

    static void clear_screen_points()
    {
        g_screenPointHead = 0;
        g_screenPointCount = 0;
        g_selectedScreenPointIdx = -1;
    }

    static void update_panel_entry_capture()
    {
        if (!g_ctrlLmbCaptureEnabled)
            return;

        auto& io = ImGui::GetIO();
        if (!io.KeyCtrl || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            return;

        if (!g_captureClicksOnUi && io.WantCaptureMouse)
            return;

        g_lastCapturedScreenPoint.x = static_cast<LONG>(io.MousePos.x);
        g_lastCapturedScreenPoint.y = static_cast<LONG>(io.MousePos.y);
        g_hasCapturedScreenPoint = true;

        auto now = GetTickCount();
        push_screen_point(g_lastCapturedScreenPoint.x, g_lastCapturedScreenPoint.y, now);

        char message[96]{};
        sprintf_s(message, "[custom-gm] Screen X:%ld Y:%ld", g_lastCapturedScreenPoint.x, g_lastCapturedScreenPoint.y);
        Static::DrawText_ChatBox(ChatType::Notice26, message, 0);
    }

    static void draw_coordinate_capture()
    {
        ImGui::Checkbox("Enable Ctrl+LMB capture", &g_ctrlLmbCaptureEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Capture UI clicks", &g_captureClicksOnUi);
        ImGui::TextDisabled("Hold Ctrl and left click to capture screen coordinates.");

        if (g_hasCapturedScreenPoint)
        {
            ImGui::Text("Last captured: X=%ld Y=%ld", g_lastCapturedScreenPoint.x, g_lastCapturedScreenPoint.y);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy Last##screen_point"))
            {
                char value[64]{};
                sprintf_s(value, "%ld, %ld", g_lastCapturedScreenPoint.x, g_lastCapturedScreenPoint.y);
                ImGui::SetClipboardText(value);
            }
        }
        else
        {
            ImGui::TextDisabled("No coordinates captured yet.");
        }

        ImGui::Separator();
        if (ImGui::SmallButton("Clear points##screen_points"))
            clear_screen_points();

        if (g_screenPointCount == 0)
        {
            ImGui::TextDisabled("No captured points in history.");
            return;
        }

        ImGui::Columns(3, "screen_points_cols", true);
        ImGui::SetColumnWidth(0, 120.0f);
        ImGui::SetColumnWidth(1, 70.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Point");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Ago");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Actions");
        ImGui::NextColumn();
        ImGui::Separator();

        auto now = GetTickCount();
        for (int i = 0; i < g_screenPointCount; ++i)
        {
            auto idx = (g_screenPointHead - 1 - i + kMaxScreenPointEntries * 2) % kMaxScreenPointEntries;
            auto& entry = g_screenPointRing[idx];

            char pointLabel[64]{};
            sprintf_s(pointLabel, "(%ld, %ld)##pt_%d", entry.x, entry.y, idx);
            if (ImGui::Selectable(pointLabel, g_selectedScreenPointIdx == idx))
                g_selectedScreenPointIdx = idx;
            ImGui::NextColumn();

            auto elapsed = now - entry.tick;
            if (elapsed < 60000)
                ImGui::Text("%us", elapsed / 1000);
            else
                ImGui::Text("%um%us", elapsed / 60000, (elapsed % 60000) / 1000);
            ImGui::NextColumn();

            char copyLabel[32]{};
            sprintf_s(copyLabel, "Copy##pt_copy_%d", idx);
            if (ImGui::SmallButton(copyLabel))
            {
                char value[64]{};
                sprintf_s(value, "%ld, %ld", entry.x, entry.y);
                ImGui::SetClipboardText(value);
                g_selectedScreenPointIdx = idx;
            }
            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        if (g_selectedScreenPointIdx >= 0)
        {
            auto& selected = g_screenPointRing[g_selectedScreenPointIdx % kMaxScreenPointEntries];
            if (ImGui::SmallButton("Copy selected##screen_point_selected"))
            {
                char value[64]{};
                sprintf_s(value, "%ld, %ld", selected.x, selected.y);
                ImGui::SetClipboardText(value);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Main Panel
    // -----------------------------------------------------------------------
    void toggle_if_f7(bool& wasDown)
    {
        auto isDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (isDown && !wasDown && is_admin())
            g_showCustomPanel = !g_showCustomPanel;
        wasDown = isDown;
    }

    void render()
    {
        toggle_if_f7(g_f7Down);

        if (!g_showCustomPanel || !is_admin())
            return;

        update_panel_entry_capture();

        if (g_showCustomPanel)
        {
            ImGui::SetNextWindowSize(ImVec2(kPanelW, kPanelH), ImGuiCond_FirstUseEver);
            auto flags = ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

            if (!ImGui::Begin("Custom GM Tools [F7]", &g_showCustomPanel, flags))
            {
                ImGui::End();
            }
            else
            {
                if (ImGui::BeginTabBar("custom_gm_tabs"))
                {
                    if (ImGui::BeginTabItem("Item Search"))
                    {
                        draw_item_search();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Weapon Skin"))
                    {
                        draw_weapon_skin();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("GM Commands"))
                    {
                        draw_gm_commands();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("EFT Browser"))
                    {
                        draw_eft_browser();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Screen Capture"))
                    {
                        draw_coordinate_capture();
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::End();
            }
        }
    }

    bool is_open()
    {
        return g_showCustomPanel && is_admin();
    }
}
