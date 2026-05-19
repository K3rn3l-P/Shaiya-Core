#include <windows.h>
#include <unordered_map>
#include <util/util.h>
#include "include/main.h"
#include "include/shaiya/CCharacter.h"
#include "include/shaiya/CDataFile.h"
#include "include/shaiya/CWorldMgr.h"
#include "include/shaiya/CEffectData.h"
#include "include/shaiya/HexColor.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/Static.h"
using namespace shaiya;

namespace name_color
{
    constexpr uint16_t kRainbowNameRange = 11;
    constexpr const char* kEffectFolders[] = {
        "data/effect",
        "Data\\Effect",
        "effect",
        "Effect"
    };

    // 8 pre-loaded slots: index 0..3 = Light tier A/AS/S/SS, index 4..7 = Fury tier A/AS/S/SS
    // Uses effectData ids 283..290 (unused slots in g_var)
    constexpr int kEnchantEftBaseId = 283;
    constexpr int kEnchantEftSubId  = 0;

    enum class EnchantEftSlot : int
    {
        LightA  = 0,
        LightAS = 1,
        LightS  = 2,
        LightSS = 3,
        FuryA   = 4,
        FuryAS  = 5,
        FuryS   = 6,
        FurySS  = 7,
        None    = -1
    };

    struct SlotInfo
    {
        EnchantEftSlot slot;
        const char*    fileName;
    };

    constexpr SlotInfo kSlotTable[] = {
        { EnchantEftSlot::LightA,  "Armor_Enchant_L_A.eft"  },
        { EnchantEftSlot::LightAS, "Armor_Enchant_L_AS.eft" },
        { EnchantEftSlot::LightS,  "Armor_Enchant_L_S.eft"  },
        { EnchantEftSlot::LightSS, "Armor_Enchant_L_SS.eft" },
        { EnchantEftSlot::FuryA,   "Armor_Enchant_F_A.eft"  },
        { EnchantEftSlot::FuryAS,  "Armor_Enchant_F_AS.eft" },
        { EnchantEftSlot::FuryS,   "Armor_Enchant_F_S.eft"  },
        { EnchantEftSlot::FurySS,  "Armor_Enchant_F_SS.eft" },
    };

    // Tracks the active EFT slot per character (keyed by CCharacter*)
    inline std::unordered_map<CCharacter*, EnchantEftSlot> g_charSlot;

    inline bool g_slotsLoaded = false;
    inline bool g_enchantEftEnabled = true;

    CEffectData* get_effect_data(EnchantEftSlot slot)
    {
        auto base = &g_var->effectData283;
        return base + static_cast<int>(slot);
    }

    int get_effect_data_id(EnchantEftSlot slot)
    {
        return kEnchantEftBaseId + static_cast<int>(slot);
    }

    bool load_all_slots()
    {
        if (g_slotsLoaded)
            return true;

        bool anyLoaded = false;
        for (const auto& info : kSlotTable)
        {
            for (auto path : kEffectFolders)
            {
                if (CEffectData::CreateFromFile(get_effect_data(info.slot), path, info.fileName))
                {
                    anyLoaded = true;
                    break;
                }
            }
        }

        g_slotsLoaded = anyLoaded;
        return g_slotsLoaded;
    }

    EnchantEftSlot resolve_slot(uint8_t enchantStep, bool fury)
    {
        if (enchantStep >= 20)
            return fury ? EnchantEftSlot::FurySS : EnchantEftSlot::LightSS;
        if (enchantStep >= 15)
            return fury ? EnchantEftSlot::FuryS  : EnchantEftSlot::LightS;
        if (enchantStep >= 10)
            return fury ? EnchantEftSlot::FuryAS : EnchantEftSlot::LightAS;
        if (enchantStep >= 5)
            return fury ? EnchantEftSlot::FuryA  : EnchantEftSlot::LightA;
        return EnchantEftSlot::None;
    }

    EnchantEftSlot resolve_slot_for_char(CCharacter* user)
    {
        auto enchantStep = user->equipment.enchantStep[ItemSlot::Weapon];
        bool fury = (user->family == Family::Deatheater || user->family == Family::Vail);
        return resolve_slot(enchantStep, fury);
    }

    void remove_char_effect(CCharacter* user, EnchantEftSlot slot)
    {
        CCharacter::RemoveEffect(user, get_effect_data_id(slot), kEnchantEftSubId);
    }

    void render_char_effect(CCharacter* user, EnchantEftSlot slot)
    {
        CCharacter::RenderEffect(
            user,
            get_effect_data_id(slot),
            kEnchantEftSubId,
            0.0f,
            &user->pos,
            &user->dir,
            &user->up,
            0);
    }

    void update_enchant_eft(CCharacter* user)
    {
        if (!g_enchantEftEnabled || !g_showEffects)
        {
            auto it = g_charSlot.find(user);
            if (it != g_charSlot.end())
            {
                remove_char_effect(user, it->second);
                g_charSlot.erase(it);
            }
            return;
        }

        if (!load_all_slots())
            return;

        auto desired = resolve_slot_for_char(user);

        auto it = g_charSlot.find(user);
        auto current = (it != g_charSlot.end()) ? it->second : EnchantEftSlot::None;

        if (desired == current)
            return;

        if (current != EnchantEftSlot::None)
            remove_char_effect(user, current);

        if (desired == EnchantEftSlot::None)
        {
            g_charSlot.erase(user);
            return;
        }

        g_charSlot[user] = desired;
        render_char_effect(user, desired);
    }

    void remove_char_enchant_eft_tracking(void* userPtr)
    {
        auto* user = static_cast<CCharacter*>(userPtr);
        auto it = g_charSlot.find(user);
        if (it != g_charSlot.end())
        {
            remove_char_effect(user, it->second);
            g_charSlot.erase(it);
        }
    }

    void disable_enchant_eft()
    {
        for (auto& [user, slot] : g_charSlot)
            remove_char_effect(user, slot);
        g_charSlot.clear();
    }

    void reset_helmet_eft_state()
    {
        // kept for character.cpp compatibility: removes local player tracking only
        auto localUser = g_pWorldMgr ? g_pWorldMgr->user : nullptr;
        if (localUser)
            remove_char_enchant_eft_tracking(localUser);
    }

    // ── name color helpers (unchanged) ──────────────────────────────────────

    D3DCOLOR make_rgb_color(uint8_t red, uint8_t green, uint8_t blue)
    {
        return 0xFF000000
            | (static_cast<D3DCOLOR>(red) << 16)
            | (static_cast<D3DCOLOR>(green) << 8)
            | static_cast<D3DCOLOR>(blue);
    }

    D3DCOLOR get_rainbow_name_color()
    {
        auto phase = (GetTickCount() / 450) % 6;
        auto step = static_cast<uint8_t>((GetTickCount() % 450) * 255 / 450);
        auto inverse = static_cast<uint8_t>(255 - step);

        switch (phase)
        {
        case 0: return make_rgb_color(255, step, 0);
        case 1: return make_rgb_color(inverse, 255, 0);
        case 2: return make_rgb_color(0, 255, step);
        case 3: return make_rgb_color(0, inverse, 255);
        case 4: return make_rgb_color(step, 0, 255);
        default: return make_rgb_color(255, 0, inverse);
        }
    }

    D3DCOLOR get_custom_color(uint16_t range)
    {
        switch (range)
        {
        case 1:  return std::to_underlying(HexColor::Red);
        case 2:  return std::to_underlying(HexColor::DodgerBlue);
        case 3:  return std::to_underlying(HexColor::Green);
        case 4:  return std::to_underlying(HexColor::Yellow);
        case 5:  return std::to_underlying(HexColor::Orange);
        case 6:  return std::to_underlying(HexColor::Purple);
        case 7:  return std::to_underlying(HexColor::Pink);
        case 8:  return std::to_underlying(HexColor::Cyan);
        case 9:  return std::to_underlying(HexColor::Gold);
        case 10: return std::to_underlying(HexColor::Silver);
        case kRainbowNameRange: return get_rainbow_name_color();
        default: return 0;
        }
    }

    bool is_name_color_helmet(const ItemInfo* itemInfo)
    {
        if (!itemInfo)
            return false;

        switch (itemInfo->type)
        {
        case 16: case 31: case 72: case 87:
            return true;
        default:
            return false;
        }
    }

    HexColor get_mob_name_color(int mobLevel)
    {
        int gap = mobLevel - g_pPlayerData->level;
        if (gap >= 10) return HexColor::Gray;

        switch (gap)
        {
        case 9: case 8:   return HexColor::Pink;
        case 7: case 6:   return HexColor::Red;
        case 5: case 4:   return HexColor::Orange;
        case 3: case 2:   return HexColor::Yellow;
        case 1: case 0: case -1: return HexColor::Green;
        case -2: case -3: return HexColor::Blue;
        case -4: case -5: return HexColor::LightBlue;
        default: break;
        }
        return HexColor::White;
    }

    D3DCOLOR get_helmet_name_color(CCharacter* user)
    {
        if (!g_showNameColors)
            return 0;

        update_enchant_eft(user);

        auto helmetType   = user->equipment.type[ItemSlot::Helmet];
        auto helmetTypeId = user->equipment.typeId[ItemSlot::Helmet];

        auto itemInfo = CDataFile::GetItemInfo(helmetType, helmetTypeId);
        if (!itemInfo || !is_name_color_helmet(itemInfo) || !itemInfo->range)
            return 0;

        return get_custom_color(itemInfo->range);
    }
}

void __declspec(naked) naked_0x4E50D0()
{
    __asm
    {
        push ebx
        push edi
        push esi

        movzx eax,word ptr[esp+0x10]
        push eax
        call name_color::get_mob_name_color
        add esp,0x4

        pop esi
        pop edi
        pop ebx

        retn 0x4
    }
}

unsigned u0x453CE1 = 0x453CE1;
void __declspec(naked) naked_0x453CDB()
{
    __asm
    {
        push ebx
        push edi
        push esi

        push esi // user
        call name_color::get_helmet_name_color
        add esp,0x4
        test eax,eax

        pop esi
        pop edi
        pop ebx

        je original

        mov ebp,eax

        original:
        mov edi,dword ptr [esi+0x314]
        jmp u0x453CE1
    }
}

void hook::name_color()
{
    // mobs
    util::detour((void*)0x4E50D0, naked_0x4E50D0, 5);
    // users: apply enchant EFT and helmet-driven name color immediately before char name draw
    util::detour((void*)0x453CDB, naked_0x453CDB, 6);
}

