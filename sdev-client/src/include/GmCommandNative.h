#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shaiya::gm
{
    constexpr std::uintptr_t kDispatchAddress = 0x47FFD0;
    constexpr std::uintptr_t kPermissionCheckAddress = 0x47A970;
    constexpr std::uintptr_t kFeedbackOverlayAddress = 0x47F8B0;

    constexpr std::uint8_t kLevelNone = 0;
    constexpr std::uint8_t kLevelAdmin = 1;
    constexpr std::uint8_t kLevelGM2 = 2;
    constexpr std::uint8_t kLevelGM3 = 3;
    constexpr std::uint8_t kLevelGM4 = 4;
    constexpr std::uint8_t kLevelGM5 = 5;

    constexpr std::uint8_t kNoAction = 0xFF;
    constexpr int kNativeFeedbackChatType = 31;

    enum ActionId : std::uint8_t
    {
        kActionCharOn = 0x00,
        kActionCharOff = 0x01,
        kActionAttackOn = 0x02,
        kActionAttackOff = 0x03,
        kActionAMove = 0x0C,
        kActionWarning = 0x13,
        kActionCwatchOff = 0x16,
        kActionGuildGrade = 0x21,
        kActionGuildGrade2 = 0x22,
        kActionGMNotice = 0x28,
        kActionCNotice = 0x29,
        kActionNotice = 0x2B,
        kActionZNotice = 0x2C,
        kActionPvpOn = 0x37,
        kActionPvpOff = 0x38,
        kActionEventOn = 0x3B,
        kActionEventOff = 0x3C,
        kActionImake = 0x43,
        kActionMMake = 0x47,
        kActionMeraTarget = 0x48,
        kActionMera = 0x4A,
        kActionNMake = 0x4C,
        kActionNEra = 0x50,
        kActionBSummon = 0x1F,
        kActionPartySummon = 0x20,
        kActionServerOn = 0x56,
        kActionServerOff = 0x57,
        kActionShutdown = 0x58,
    };

    enum class Category : std::uint8_t
    {
        Mode,
        Movement,
        Moderation,
        Notice,
        SpawnItem,
        Guild,
        QuestEvent,
        Server,
        WorldDebug,
        Auction,
        AdminPower,
        ExtendsCommand,
        Other,
    };

    constexpr std::uint16_t kNoSysMsg = 0;
    constexpr std::uintptr_t kGetMsgAddress = 0x420DB0;

    struct CommandEntry
    {
        const char* prefix;
        Category category;
        std::uint8_t required_level;
        std::uint8_t action_id;
        std::uint16_t feedback_sysmsg_id;
        const char* summary;
    };

    inline constexpr CommandEntry kNativeCatalog[] =
    {
        { "/char on", Category::Mode, kLevelGM5, kActionCharOn, 263, "Show GM character" },
        { "/char off", Category::Mode, kLevelGM5, kActionCharOff, 264, "Hide GM character" },
        { "/attack on", Category::Mode, kLevelGM5, kActionAttackOn, 265, "Enable GM attack" },
        { "/attack off", Category::Mode, kLevelGM5, kActionAttackOff, 266, "Disable GM attack" },
        { "/onekillon", Category::Mode, kLevelGM2, kNoAction, 0, "One-hit kill ON" },
        { "/onekilloff", Category::Mode, kLevelGM2, kNoAction, 0, "One-hit kill OFF" },
        { "/autocure", Category::Mode, kLevelNone, kNoAction, 0, "GM auto-heal ON [no perm check]" },
        { "/noautocure", Category::Mode, kLevelNone, kNoAction, 0, "GM auto-heal OFF [no perm check]" },
        { "/cure", Category::Mode, kLevelGM3, kNoAction, 0, "Instant heal self or <name>" },
        { "/camlimit on", Category::Mode, kLevelNone, kNoAction, 0, "Camera limit ON [no perm check]" },
        { "/camlimit off", Category::Mode, kLevelNone, kNoAction, 0, "Camera limit OFF [no perm check]" },
        { "/chatcolor off", Category::Mode, kLevelNone, kNoAction, 0, "Reset GM chat color [no perm check]" },
        { "/chatcolor", Category::Mode, kLevelNone, kNoAction, 0, "Set GM chat color <1-8> [no perm check]" },
        { "/pvpon", Category::Mode, kLevelNone, kActionPvpOn, 266, "PVP ON [no perm check]" },
        { "/pvpoff", Category::Mode, kLevelNone, kActionPvpOff, 265, "PVP OFF [no perm check]" },

        { "/amove ", Category::Movement, kLevelGM5, kActionAMove, 267, "Absolute teleport <name>" },
        { "/bmove ", Category::Movement, kLevelGM5, kNoAction, 268, "Relative teleport B <x y z>" },
        { "/cmove", Category::Movement, kLevelGM5, kNoAction, 269, "Relative teleport C [mapId]" },
        { "/gmove ", Category::Movement, kLevelGM5, kNoAction, 268, "Group teleport <x y z guildName>" },
        { "/effectpos", Category::Movement, kLevelGM5, kNoAction, 0, "Adjust effect position <name x|y|z val>" },

        { "/silence off", Category::Moderation, kLevelGM5, kNoAction, 0, "Remove silence <name>" },
        { "/silence ", Category::Moderation, kLevelGM5, kNoAction, 0, "Silence player <duration name>" },
        { "/stopon ", Category::Moderation, kLevelGM5, kNoAction, 0, "Stop player ON <duration name>" },
        { "/stopoff ", Category::Moderation, kLevelGM5, kNoAction, 0, "Stop player OFF <name>" },
        { "/warning ", Category::Moderation, kLevelGM5, kActionWarning, 270, "Warn player <name message>" },
        { "/watch ", Category::Moderation, kLevelGM5, kNoAction, 0, "Watch player <name>" },
        { "/cwatch off", Category::Moderation, kLevelGM5, kActionCwatchOff, 273, "Stop watching current character" },
        { "/cwatch ", Category::Moderation, kLevelGM5, kNoAction, 0, "Watch character <name>" },
        { "/kick ", Category::Moderation, kLevelGM3, kNoAction, 0, "Kick player <name>" },
        { "/bancan ", Category::Moderation, kLevelGM3, kNoAction, 0, "Cancel ban <name>" },
        { "/ban ", Category::Moderation, kLevelGM3, kNoAction, 0, "Ban player <duration name>" },
        { "/akick ", Category::Moderation, kLevelGM2, kNoAction, 0, "Absolute kick <name>" },
        { "/abancan ", Category::Moderation, kLevelGM2, kNoAction, 0, "Cancel absolute ban <name>" },
        { "/aban ", Category::Moderation, kLevelGM2, kNoAction, 0, "Absolute ban <duration name>" },

        { "/gmnotice ", Category::Notice, kLevelGM4, kActionGMNotice, 257, "GM notice <message>" },
        { "/cnotice ", Category::Notice, kLevelGM4, kActionCNotice, 257, "Channel notice <channel message>" },
        { "/wnotice ", Category::Notice, kLevelGM4, kNoAction, 0, "World notice <type message>" },
        { "/notice ", Category::Notice, kLevelGM4, kActionNotice, 257, "Global notice <type message>" },
        { "/znotice ", Category::Notice, kLevelGM4, kActionZNotice, 257, "Zone notice <zone message>" },
        { "/bnotice ", Category::Notice, kLevelGM4, kNoAction, 0, "Base notice <side type message>" },

        { "/item ", Category::SpawnItem, kLevelGM4, kNoAction, 0, "Look up item by name" },
        { "/mob ", Category::SpawnItem, kLevelGM4, kNoAction, 0, "Look up mob by name/id" },
        { "/mobkillarea ", Category::SpawnItem, kLevelGM5, kNoAction, 0, "Kill mobs within radius" },
        { "/asummon ", Category::SpawnItem, kLevelGM4, kNoAction, 0, "Summon player to GM position <name>" },
        { "/bsummon ", Category::SpawnItem, kLevelGM4, kActionBSummon, 0, "Summon player to coords <name x y z>" },
        { "/partysummon ", Category::SpawnItem, kLevelGM4, kActionPartySummon, 0, "Party summon to coords <name x y z>" },
        { "/itemlist ", Category::SpawnItem, kLevelGM5, kNoAction, 0, "List items in category <name>" },
        { "/itemlv", Category::SpawnItem, kLevelGM5, kNoAction, 0, "Set item level filter <level>" },
        { "/imake ", Category::SpawnItem, kLevelGM2, kActionImake, 0, "Spawn item by ID <type id qty>" },
        { "/getitem ", Category::SpawnItem, kLevelGM2, kNoAction, 0, "Obtain item <type id qty>" },
        { "/recallitem ", Category::SpawnItem, kLevelGM2, kNoAction, 0, "Recall item from player <name item>" },
        { "/mmakeall ", Category::SpawnItem, kLevelGM2, kNoAction, 0, "Spawn all of mob type <id qty>" },
        { "/mmake ", Category::SpawnItem, kLevelGM2, kActionMMake, 0, "Spawn mob by ID <id qty>" },
        { "/nmake ", Category::SpawnItem, kLevelGM2, kNoAction, 0, "Spawn NPC <type id>" },
        { "/nera ", Category::SpawnItem, kLevelGM2, kActionNEra, 0, "Remove NPC from area <type id>" },
        { "/mera t", Category::SpawnItem, kLevelGM2, kActionMeraTarget, 0, "Remove targeted entity" },
        { "/mera ", Category::SpawnItem, kLevelGM2, kActionMera, 0, "Remove entity by id <type id>" },
        { "/iclear", Category::SpawnItem, kLevelGM3, kNoAction, 0, "Clear inventory <name>" },
        { "/eclear", Category::SpawnItem, kLevelGM3, kNoAction, 0, "Clear equipment <name>" },

        { "/guildgrade2 ", Category::Guild, kLevelGM4, kActionGuildGrade2, 0, "Set guild grade v2 <guild item qty>" },
        { "/guildgrade ", Category::Guild, kLevelGM4, kActionGuildGrade, 0, "Set guild grade <guild item qty>" },
        { "/guildrank ", Category::Guild, kLevelGM4, kNoAction, 0, "Set guild rank <name rank>" },
        { "/guildpoint ", Category::Guild, kLevelGM4, kNoAction, 0, "Set guild points <name pts>" },

        { "/queston ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Enable quest by ID" },
        { "/questoff ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Disable quest by ID" },
        { "/questadd ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Add quest to player <name questId>" },
        { "/questrem ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Remove quest from player <name questId>" },
        { "/event on ", Category::QuestEvent, kLevelGM3, kActionEventOn, 0, "Enable event <eventName>" },
        { "/event off ", Category::QuestEvent, kLevelGM3, kActionEventOff, 0, "Disable event <eventName>" },
        { "/eventadd ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Add event to player <name eventName>" },
        { "/eventrem ", Category::QuestEvent, kLevelGM3, kNoAction, 0, "Remove event from player <name eventName>" },
        { "/tacticszone", Category::QuestEvent, kLevelGM5, kNoAction, 0, "Toggle tactics zone flag" },

        { "/server on", Category::Server, kLevelGM2, kActionServerOn, 252, "Open server" },
        { "/server off", Category::Server, kLevelGM2, kActionServerOff, 252, "Close server" },
        { "/shutdown", Category::Server, kLevelGM2, kActionShutdown, 252, "Shutdown server" },

        { "/CheckBuildVersion", Category::WorldDebug, kLevelGM5, kNoAction, 0, "Print build version" },
        { "/fogstart ", Category::WorldDebug, kLevelGM5, kNoAction, 0, "Set fog start distance <val>" },
        { "/fogend ", Category::WorldDebug, kLevelGM5, kNoAction, 0, "Set fog end distance <val>" },
        { "/newrandsimulate ", Category::WorldDebug, kLevelNone, kNoAction, 0, "Random item simulation [no perm check]" },
        { "/quiry ", Category::WorldDebug, kLevelGM4, kNoAction, 0, "Server query/inquiry <name>" },
        { "/xcall", Category::WorldDebug, kLevelGM5, kNoAction, 0, "Cross-server call <1=A|2=B>" },

        { "/auctionrecall ", Category::Auction, kLevelGM4, kNoAction, 0, "Auction recall <name itemId>" },
        { "/auctionsearch ", Category::Auction, kLevelGM4, kNoAction, 0, "Auction search <name>" },

        { "/apower ", Category::AdminPower, kLevelAdmin, kNoAction, 0, "Grant admin power <name type>" },
        { "/apowercan ", Category::AdminPower, kLevelAdmin, kNoAction, 0, "Revoke admin power <name>" },
        { "/cmd ", Category::AdminPower, kLevelAdmin, kNoAction, 0, "Send raw server command <args>" },
        { "/set ", Category::AdminPower, kLevelGM2, kNoAction, 0, "Set player stat/flag <name field val>" },
    };

    inline constexpr std::size_t kNativeCatalogCount =
        sizeof(kNativeCatalog) / sizeof(kNativeCatalog[0]);

    namespace extendsCommands
    {
        struct Entry
        {
            const char* command;
            const char* summary;
            bool push_success_sysmsg;
        };

        inline constexpr Entry kExtendsCommands[] =
        {
            { "/effects on", "Enable visual effects", true },
            { "/effects off", "Disable visual effects", true },
            { "/pets on", "Show pets", true },
            { "/pets off", "Hide pets", true },
            { "/wings on", "Show wings", true },
            { "/wings off", "Hide wings", true },
            { "/costumes on", "Show costumes", true },
            { "/costumes off", "Hide costumes", true },
            { "/fpsboost on", "Enable FPS boost", true },
            { "/fpsboost off", "Disable FPS boost", true },
            { "/font", "Open chat font picker", false },
            { "/titles on", "Show titles", true },
            { "/titles off", "Hide titles", true },
            { "/colour on", "Enable name colour", true },
            { "/colour off", "Disable name colour", true },
            { "/color on", "Enable name color (US spelling)", true },
            { "/color off", "Disable name color (US spelling)", true },
        };

        inline constexpr std::size_t kExtendsCommandCount =
            sizeof(kExtendsCommands) / sizeof(kExtendsCommands[0]);

        inline const Entry* find(const char* text) noexcept
        {
            if (!text || text[0] != '/')
                return nullptr;
            const std::string_view sv(text);
            for (std::size_t i = 0; i < kExtendsCommandCount; ++i)
            {
                if (sv == kExtendsCommands[i].command)
                    return &kExtendsCommands[i];
            }
            return nullptr;
        }

        inline bool matches(const char* text) noexcept
        {
            return find(text) != nullptr;
        }
    }

    inline bool is_extends_command(const char* text) noexcept
    {
        return extendsCommands::matches(text);
    }

    inline const extendsCommands::Entry* find_extends_command(const char* text) noexcept
    {
        return extendsCommands::find(text);
    }

    using DispatchFn = int(__thiscall*)(void* chatWindow, char* text);

    inline constexpr bool starts_with(std::string_view text, std::string_view prefix) noexcept
    {
        return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
    }

    inline bool is_native_catalog_command(const char* text) noexcept
    {
        if (!text || text[0] != '/')
            return false;
        const std::string_view sv(text);
        for (std::size_t i = 0; i < kNativeCatalogCount; ++i)
        {
            if (starts_with(sv, kNativeCatalog[i].prefix))
                return true;
        }
        return false;
    }

    inline const CommandEntry* find_catalog_entry(const char* text) noexcept
    {
        if (!text || text[0] != '/')
            return nullptr;

        const std::string_view sv(text);
        const CommandEntry* best = nullptr;
        std::size_t best_len = 0;

        for (std::size_t i = 0; i < kNativeCatalogCount; ++i)
        {
            const std::string_view pfx(kNativeCatalog[i].prefix);
            if (pfx.size() > best_len && starts_with(sv, pfx))
            {
                best = &kNativeCatalog[i];
                best_len = pfx.size();
            }
        }

        return best;
    }

    inline bool looks_like_native_gm_command(const char* text) noexcept
    {
        return text && text[0] == '/' && !is_extends_command(text);
    }

    inline bool is_catalogued_command(const char* text) noexcept
    {
        return find_catalog_entry(text) != nullptr;
    }

    inline bool should_route_to_native_dispatcher(const char* text, bool is_admin) noexcept
    {
        return is_admin && looks_like_native_gm_command(text);
    }

    inline int dispatch(void* chatWindow, char* text) noexcept
    {
        if (!chatWindow || !text || text[0] == '\0')
            return 0;

        return reinterpret_cast<DispatchFn>(kDispatchAddress)(chatWindow, text);
    }
}
