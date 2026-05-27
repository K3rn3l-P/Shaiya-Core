#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "MapPos.h"

namespace shaiya
{
#pragma pack(push, 1)
    struct BattlefieldMoveInfo
    {
        int32_t id;
        int32_t levelMin;
        int32_t levelMax;
        int32_t enabled;
        std::array<MapPos, 4> mapPos;
    };
#pragma pack(pop)

    struct BattlefieldLvInRange
    {
        explicit BattlefieldLvInRange(int level)
            : level(level)
        {
        }

        bool operator()(const BattlefieldMoveInfo& info) const
        {
            return level >= info.levelMin && level <= info.levelMax;
        }

        int level;
    };

    struct BattlefieldIdEquals
    {
        explicit BattlefieldIdEquals(int id)
            : id(id)
        {
        }

        bool operator()(const BattlefieldMoveInfo& info) const
        {
            return info.id == id;
        }

        int id;
    };

    inline std::vector<BattlefieldMoveInfo> g_battlefieldMoveData;

    // New teleport system: named destinations with optional level gates.
    // Each destination has separate map/position per faction (Light=0, Fury=1).
    struct TeleportFactionData
    {
        int32_t mapId;
        float x;
        float y;
        float z;
    };

    struct TeleportDestination
    {
        std::string name;
        int32_t levelMin;
        int32_t levelMax;
        TeleportFactionData factionData[2]; // 0=Light, 1=Fury
    };

    inline std::vector<TeleportDestination> g_teleportDestinations;
}
