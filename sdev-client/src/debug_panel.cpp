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
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/CPlayerData.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/Static.h"

using namespace shaiya;

// ---------------------------------------------------------------------------
// F9 GM debug panel — admin-only chat type monitor.
//
// Records incoming chat types with preview text so GMs can identify which
// chat type IDs correspond to which visual messages.  Useful for tuning
// upper-bar exclusions and verifying custom chat routing.
//
// The monitor only runs when the user explicitly activates it from the
// panel (per-session toggle, no CONFIG.ini persistence).
// ---------------------------------------------------------------------------

namespace debug_panel
{
    // -----------------------------------------------------------------------
    // Panel state
    // -----------------------------------------------------------------------
    static bool g_showDebugPanel = false;
    static bool g_showEftBrowser = false;
    static bool g_f9Down = false;

    static constexpr float kPanelW = 546.0f;
    static constexpr float kPanelH = 400.0f;
    static constexpr float kEftBrowserW = 720.0f;
    static constexpr float kEftBrowserH = 560.0f;

    // -----------------------------------------------------------------------
    // Chat Type Monitor
    // -----------------------------------------------------------------------
    static bool g_chatMonitorActive = false;

    struct ChatTypeEntry
    {
        int chatType;
        DWORD tick;
        char preview[64];
    };

    static constexpr int kMaxChatEntries = 64;
    static ChatTypeEntry g_chatTypeRing[kMaxChatEntries]{};
    static int g_chatTypeHead = 0;
    static int g_chatTypeCount = 0;

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

    void record_chat_type(int chatType, const char* text)
    {
        if (!g_chatMonitorActive)
            return;

        auto& entry = g_chatTypeRing[g_chatTypeHead % kMaxChatEntries];
        entry.chatType = chatType;
        entry.tick = GetTickCount();
        if (text)
        {
            strncpy_s(entry.preview, text, _TRUNCATE);
            entry.preview[sizeof(entry.preview) - 1] = '\0';
        }
        else
        {
            entry.preview[0] = '\0';
        }
        g_chatTypeHead = (g_chatTypeHead + 1) % kMaxChatEntries;
        if (g_chatTypeCount < kMaxChatEntries)
            ++g_chatTypeCount;
    }

    static void clear_chat_entries()
    {
        g_chatTypeHead = 0;
        g_chatTypeCount = 0;
    }

    static void draw_chat_monitor()
    {
        ImGui::Checkbox("Active##chat", &g_chatMonitorActive);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##chat"))
            clear_chat_entries();

        ImGui::Separator();

        if (!g_chatMonitorActive && g_chatTypeCount == 0)
        {
            ImGui::TextDisabled("Enable to start recording chat types.");
            return;
        }

        if (g_chatTypeCount == 0)
        {
            ImGui::TextDisabled("Waiting for chat messages...");
            return;
        }

        ImGui::Columns(3, "chat_types_cols", true);
        ImGui::SetColumnWidth(0, 50.0f);
        ImGui::SetColumnWidth(1, 80.0f);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Type");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Ago");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Preview");
        ImGui::NextColumn();
        ImGui::Separator();

        auto now = GetTickCount();
        for (int i = 0; i < g_chatTypeCount; ++i)
        {
            auto idx = (g_chatTypeHead - 1 - i + kMaxChatEntries * 2) % kMaxChatEntries;
            auto& entry = g_chatTypeRing[idx];

            auto type = entry.chatType;
            ImVec4 typeColor(1.0f, 1.0f, 1.0f, 1.0f);
            if (type == 15)      typeColor = ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
            else if (type == 16 || type == 17) typeColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            else if (type == 18) typeColor = ImVec4(1.0f, 1.0f, 0.3f, 1.0f);
            else if (type == 19) typeColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
            else if (type == 20) typeColor = ImVec4(0.7f, 0.3f, 1.0f, 1.0f);
            else if (type == 21) typeColor = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
            else if (type == 22) typeColor = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            else if (type == 34) typeColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);

            ImGui::TextColored(typeColor, "%d", type);
            ImGui::NextColumn();

            auto elapsed = now - entry.tick;
            if (elapsed < 60000)
                ImGui::Text("%ds", elapsed / 1000);
            else
                ImGui::Text("%dm%ds", elapsed / 60000, (elapsed % 60000) / 1000);
            ImGui::NextColumn();

            ImGui::TextUnformatted(entry.preview);
            ImGui::NextColumn();
        }

        ImGui::Columns(1);
    }

    // -----------------------------------------------------------------------
    // GM Commands
    // -----------------------------------------------------------------------
    struct GmCommandEntry
    {
        const char* category;
        const char* templateText;
        const char* insertText;
        const char* description;
    };

    static constexpr auto kGmCommands = std::to_array<GmCommandEntry>({
        { "Character", "/char on", "/char on", "Enable character mode/visibility state." },
        { "Character", "/char off", "/char off", "Disable character mode/visibility state." },
        { "Character", "/attack on", "/attack on", "Enable attack mode." },
        { "Character", "/attack off", "/attack off", "Disable attack mode." },

        { "Movement", "/cmove <MapID>", "/cmove", "Move yourself to a map id." },
        { "Movement", "/amove <UserName>", "/amove", "Move to target player." },
        { "Movement", "/bmove <X> <Y> <MapID>", "/bmove", "Move to explicit coordinates/map." },

        { "Summon", "/asummon <UserName>", "/asummon", "Summon one player to you." },
        { "Summon", "/bsummon <UserName> <X> <Z> <MapID>", "/bsummon", "Summon player to specific map coordinates." },
        { "Summon", "/xcall <Faction#>", "/xcall", "Summon an entire faction (1 Light, 2 Fury)." },

        { "Items", "/getitem <ItemType> <ItemTypeID> <ItemCount>", "/getitem", "Create item by type/typeId/count." },
        { "Items", "/getitem <type> <typeid>", "/getitem", "Short item create form." },
        { "Items", "/imake \"Item Name Here\"", "/imake", "Create item by item name text." },

        { "NPC", "/nmake <TypeID>", "/nmake", "Spawn NPC by type id." },
        { "NPC", "/nera <TypeID>", "/nera", "Delete NPC by type id." },

        { "MOB", "/mmake <MobID>", "/mmake", "Spawn mob by id." },
        { "MOB", "/mera <TypeID>", "/mera", "Delete mobs in range / by type id." },
        { "MOB", "/mera t", "/mera t", "Delete currently targeted mob." },

        { "Notices", "/notice \"Message Here\"", "/notice", "Global server notice." },
        { "Notices", "/bnotice \"Message Here\"", "/bnotice", "Broadcast notice." },
        { "Notices", "/znotice \"Message Here\"", "/znotice", "Notice on current map only." },
        { "Notices", "/wnotice <UserName> \"Message Here\"", "/wnotice", "Notice-style whisper to one user." },
        { "Notices", "/cnotice \"Message Here\"", "/cnotice", "Faction-only notice." },
        { "Notices", "/gmnotice \"Message Here\"", "/gmnotice", "GM-only notice." },
        { "Notices", "/warning <UserName> \"Message Here\"", "/warning", "Warning message to one user." },

        { "Moderation", "/kick <UserName>", "/kick", "Kick player offline." },
        { "Moderation", "/stopon <UserName>", "/stopon", "Apply movement restriction." },
        { "Moderation", "/stopoff <UserName>", "/stopoff", "Remove movement restriction." },
        { "Moderation", "/silence on <UserName>", "/silence on", "Enable chat silence on player." },
        { "Moderation", "/silence off <UserName>", "/silence off", "Disable chat silence on player." },
        { "Moderation", "/watch <UserName>", "/watch", "Locate or track a player." },

        { "Inventory/Admin", "/eclear <UserName>", "/eclear", "Clear equipped items of user." },
        { "Inventory/Admin", "/iclear <UserName>", "/iclear", "Clear inventory of user." },
        { "Inventory/Admin", "/cure <UserName>", "/cure", "Cure target user or self." },
        { "Inventory/Admin", "/set <UserName> <skillpoint|money|kill|str|dex|luc|wis|int|rec> <Amount>", "/set", "Set selected user stat/value." },
        { "Inventory/Admin", "/apowercan <UserName>", "/apowercan", "Remove GM powers for user." },
        { "Inventory/Admin", "/auctionsearch <UserName>", "/auctionsearch", "Inspect player auction listings." },
        { "Inventory/Admin", "/auctionrecall <UserName>", "/auctionrecall", "Recall player market items." },

        { "Camera", "/camlimit on", "/camlimit on", "Enable camera limit mode." },
        { "Camera", "/camlimit off", "/camlimit off", "Disable camera limit mode." },

        { "Untested", "/addpoints [CharName] [count]", "/addpoints", "Untested: add points." },
        { "Untested", "/addapall [x]", "/addapall", "Untested: add AP all." },
        { "Untested", "/getpoints", "/getpoints", "Untested: query points." },
        { "Untested", "/promote [GM CharName]", "/promote", "Untested: promote GM." },
        { "Untested", "/demote [GM CharName]", "/demote", "Untested: demote GM." },
        { "Untested", "/tempban [CharName] [count] [m/h/d]", "/tempban", "Untested: temporary ban." },
        { "Untested", "/ban [CharName]", "/ban", "Untested: ban." },
        { "Untested", "/fullban [CharName]", "/fullban", "Untested: full ban." },
        { "Untested", "/unban [CharName]", "/unban", "Untested: unban." },
        { "Untested", "/makegsl [CharName]", "/makegsl", "Untested: grant GSL." },
        { "Untested", "/removegsl [CharName]", "/removegsl", "Untested: remove GSL." },
        { "Untested", "/makegs [CharName]", "/makegs", "Untested: grant GS." },
        { "Untested", "/removegs [CharName]", "/removegs", "Untested: remove GS." },
        { "Untested", "/makegm [CharName]", "/makegm", "Untested: grant GM." },
        { "Untested", "/removegm [CharName]", "/removegm", "Untested: remove GM." },
        { "Untested", "/makea [CharName]", "/makea", "Untested: grant admin." },
        { "Untested", "/removea [CharName]", "/removea", "Untested: remove admin." },
        { "Untested", "/reschar [CharName]", "/reschar", "Untested: restore character." },
        { "Untested", "/delchar [CharName]", "/delchar", "Untested: delete character." },
        { "Untested", "/disableacc [CharName]", "/disableacc", "Untested: disable account." },
        { "Untested", "/enableacc [CharName]", "/enableacc", "Untested: enable account." }
    });

    static constexpr auto kGmCommandCategories = std::to_array<const char*>({
        "Character",
        "Movement",
        "Summon",
        "Items",
        "NPC",
        "MOB",
        "Notices",
        "Moderation",
        "Inventory/Admin",
        "Camera",
        "Untested"
    });

    static std::array<char, 128> g_gmCommandSearch{};

    static void post_gm_command_to_chat(const char* command)
    {
        ensure_client_sysmsg_dispatch_ready();
        if (!command || !command[0] || !g_var->hwnd || !IsWindow(g_var->hwnd))
            return;

        SendMessageA(g_var->hwnd, kClientGMCommandWindowMessage, 0, reinterpret_cast<LPARAM>(command));
    }

    static bool command_matches_search(const GmCommandEntry& entry, const std::string& lowerSearch)
    {
        if (lowerSearch.empty())
            return true;

        auto lowerTemplate = to_lower_ascii(entry.templateText);
        auto lowerCategory = to_lower_ascii(entry.category);
        auto lowerDescription = to_lower_ascii(entry.description);
        return lowerTemplate.find(lowerSearch) != std::string::npos
            || lowerCategory.find(lowerSearch) != std::string::npos
            || lowerDescription.find(lowerSearch) != std::string::npos;
    }

    static void draw_gm_commands()
    {
        ImGui::InputTextWithHint("##gm_cmd_search", "Search command...", g_gmCommandSearch.data(), g_gmCommandSearch.size());
        auto search = to_lower_ascii(g_gmCommandSearch.data());
        ImGui::TextDisabled("Click a command to insert it into the chat input.");

        for (const auto* category : kGmCommandCategories)
        {
            bool hasVisible = false;
            for (const auto& entry : kGmCommands)
            {
                if (std::strcmp(entry.category, category) == 0 && command_matches_search(entry, search))
                {
                    hasVisible = true;
                    break;
                }
            }

            if (!hasVisible)
                continue;

            if (!ImGui::TreeNode(category))
                continue;

            for (std::size_t i = 0; i < kGmCommands.size(); ++i)
            {
                const auto& entry = kGmCommands[i];
                if (std::strcmp(entry.category, category) != 0)
                    continue;

                if (!command_matches_search(entry, search))
                    continue;

                auto label = std::string(entry.templateText) + "##gm_cmd_" + std::to_string(i);
                if (ImGui::Selectable(label.c_str()))
                    post_gm_command_to_chat(entry.insertText);

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\nInsert: %s", entry.description, entry.insertText);
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

    static bool is_armor_type(int type)
    {
        return type == 16 || type == 17 || type == 18 || type == 20 || type == 21
            || type == 31 || type == 32 || type == 33 || type == 35 || type == 36
            || type == 67 || type == 68 || type == 70 || type == 71 || type == 72
            || type == 73 || type == 74 || type == 76 || type == 77
            || type == 82 || type == 83 || type == 85 || type == 86 || type == 87
            || type == 88 || type == 89 || type == 91 || type == 92;
    }

    static bool is_weapon_type(int type)
    {
        return type == 1 || type == 2 || type == 3 || type == 4 || type == 5
            || type == 6 || type == 7 || type == 8 || type == 9 || type == 10
            || type == 11 || type == 12 || type == 13 || type == 14 || type == 15
            || type == 19 || type == 34 || type == 45 || type == 46 || type == 47
            || type == 48 || type == 49 || type == 50 || type == 51 || type == 52
            || type == 53 || type == 54 || type == 55 || type == 56 || type == 57
            || type == 58 || type == 59 || type == 60 || type == 61 || type == 62
            || type == 63 || type == 64 || type == 65 || type == 69 || type == 84;
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

        if (ImGui::SmallButton("Rebuild##item_search"))
        {
            g_itemCatalogLoaded = false;
            ensure_item_catalog_loaded();
        }

        ImGui::InputTextWithHint("##item_search_input", "Search by ItemID / Name / type / typeId...", g_itemSearch.data(), g_itemSearch.size());

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

        if (ImGui::SmallButton("Clear Filters##item_search"))
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
        }

        auto search = to_lower_ascii(g_itemSearch.data());
        std::vector<int> filteredIndices;
        filteredIndices.reserve(g_itemEntries.size());

        for (int i = 0; i < static_cast<int>(g_itemEntries.size()); ++i)
        {
            if (item_matches_search(g_itemEntries[i], search) && item_matches_category_filter(g_itemEntries[i]))
                filteredIndices.push_back(i);
        }

        auto totalResults = static_cast<int>(filteredIndices.size());
        auto totalPages = std::max(1, (totalResults + kItemSearchPageSize - 1) / kItemSearchPageSize);
        g_itemSearchPage = std::clamp(g_itemSearchPage, 0, totalPages - 1);

        ImGui::TextDisabled("Results: %d", totalResults);
        ImGui::SameLine();
        ImGui::TextDisabled("Page %d/%d", g_itemSearchPage + 1, totalPages);

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
                + "##item_search_" + std::to_string(entry.type) + "_" + std::to_string(entry.typeId);

            if (ImGui::Selectable(label.c_str(), false))
                send_getitem_type_typeid_to_chat(entry.type, entry.typeId);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Name: %s\nItemID: %d\nType: %d\nTypeID: %d\nClick: /getitem %d %d 1",
                    entry.name.c_str(),
                    entry.itemId,
                    entry.type,
                    entry.typeId,
                    entry.type,
                    entry.typeId);
            }
        }

        ImGui::EndChild();
    }

    // -----------------------------------------------------------------------
    // Panel entry points
    // -----------------------------------------------------------------------
    static bool is_admin()
    {
        return g_pPlayerData && g_pPlayerData->isAdmin;
    }

    void toggle_if_f9(bool& wasDown)
    {
        auto isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (isDown && !wasDown && is_admin())
        {
            auto newState = !g_showDebugPanel;
            g_showDebugPanel = newState;
            g_showEftBrowser = newState;
        }
        wasDown = isDown;
    }

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

    static ImVec2 g_debugWindowPos{};
    static ImVec2 g_debugWindowSize{};
    static bool g_hasDebugWindowMetrics = false;

    static ImVec2 g_eftWindowPos{};
    static ImVec2 g_eftWindowSize{};
    static bool g_hasEftWindowMetrics = false;

    void draw_eft_browser()
    {
        ensure_eft_catalog_loaded();

        ImGui::SetNextWindowSize(ImVec2(kEftBrowserW, kEftBrowserH), ImGuiCond_FirstUseEver);
        auto flags = ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (!ImGui::Begin("GM EFT Browser", &g_showEftBrowser, flags))
        {
            ImGui::End();
            return;
        }

        g_eftWindowPos = ImGui::GetWindowPos();
        g_eftWindowSize = ImGui::GetWindowSize();
        g_hasEftWindowMetrics = true;

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
        ImGui::End();
    }

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
        sprintf_s(message, "[gm-debug] Screen X:%ld Y:%ld", g_lastCapturedScreenPoint.x, g_lastCapturedScreenPoint.y);
        Static::DrawText_ChatBox(ChatType::Notice26, message, 0);
    }

    static void draw_panel_entry_points()
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
        ImGui::Text("Window metrics");

        if (g_hasDebugWindowMetrics)
            ImGui::Text("GM Debug  pos(%.0f, %.0f) size(%.0f, %.0f)", g_debugWindowPos.x, g_debugWindowPos.y, g_debugWindowSize.x, g_debugWindowSize.y);
        if (g_hasEftWindowMetrics)
            ImGui::Text("EFT Browser pos(%.0f, %.0f) size(%.0f, %.0f)", g_eftWindowPos.x, g_eftWindowPos.y, g_eftWindowSize.x, g_eftWindowSize.y);

        if ((g_hasDebugWindowMetrics || g_hasEftWindowMetrics) && ImGui::SmallButton("Copy window metrics##panel"))
        {
            char value[192]{};
            sprintf_s(
                value,
                "GM_Debug:{x:%.0f,y:%.0f,w:%.0f,h:%.0f} | EFT_Browser:{x:%.0f,y:%.0f,w:%.0f,h:%.0f}",
                g_debugWindowPos.x,
                g_debugWindowPos.y,
                g_debugWindowSize.x,
                g_debugWindowSize.y,
                g_eftWindowPos.x,
                g_eftWindowPos.y,
                g_eftWindowSize.x,
                g_eftWindowSize.y);
            ImGui::SetClipboardText(value);
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

    void render()
    {
        toggle_if_f9(g_f9Down);

        if (!is_admin())
            return;

        update_panel_entry_capture();

        if (g_showDebugPanel)
        {
            ImGui::SetNextWindowSize(ImVec2(kPanelW, kPanelH), ImGuiCond_FirstUseEver);
            auto flags = ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

            if (!ImGui::Begin("GM Debug", &g_showDebugPanel, flags))
            {
                ImGui::End();
            }
            else
            {
                g_debugWindowPos = ImGui::GetWindowPos();
                g_debugWindowSize = ImGui::GetWindowSize();
                g_hasDebugWindowMetrics = true;

                if (ImGui::BeginTabBar("gm_debug_tabs"))
                {
                    if (ImGui::BeginTabItem("Chat Type Monitor"))
                    {
                        draw_chat_monitor();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("GM Commands"))
                    {
                        draw_gm_commands();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Item Search"))
                    {
                        draw_item_search();
                        ImGui::EndTabItem();
                    }

                    if (ImGui::BeginTabItem("Panel entry points"))
                    {
                        draw_panel_entry_points();
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                ImGui::End();
            }
        }

        if (g_showEftBrowser)
            draw_eft_browser();
    }
}
