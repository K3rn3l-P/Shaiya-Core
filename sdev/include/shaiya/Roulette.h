#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace shaiya
{
    inline constexpr std::size_t kRouletteMaxRewards = 20;
    inline constexpr std::size_t kRouletteRecentWinnerCount = 5;
    inline constexpr std::size_t kRouletteWinnerNameLength = 21;

    struct CUser;

    struct RouletteReward
    {
        uint8_t type{};
        uint8_t typeId{};
        uint8_t count{};
        uint16_t chance{};
        bool bigWinner{};
    };

    struct RouletteConfig
    {
        uint8_t tokenType{};
        uint8_t tokenTypeId{};
        uint8_t tokenCount{ 1 };
        uint8_t rewardCount{};
        uint8_t featuredRewardIndex{ 0xFF };
        uint8_t guaranteedRewardIndex{ 0xFF };
        uint8_t guaranteedSpinTarget{};
        std::array<RouletteReward, kRouletteMaxRewards> rewards{};
        bool valid{};
    };

    struct RoulettePendingSpin
    {
        uint8_t rewardIndex{};
        RouletteReward reward{};
        uint8_t tokenType{};
        uint8_t tokenTypeId{};
        uint8_t tokenCount{};
        uint8_t spinProgressAtStart{};
        std::chrono::system_clock::time_point completeAt{};
    };

    struct RouletteRecentWinner
    {
        std::array<char, kRouletteWinnerNameLength> name{};
        uint8_t rewardIndex{ 0xFF };
        std::chrono::system_clock::time_point wonAt{};
    };

    inline RouletteConfig g_rouletteConfig{};
    inline std::map<uint32_t, RoulettePendingSpin> g_roulettePending{};
    inline std::unordered_map<uint32_t, uint8_t> g_rouletteSpinProgress{};
    inline std::deque<RouletteRecentWinner> g_rouletteRecentWinners{};
    inline std::mutex g_rouletteMutex{};
}

namespace roulette
{
    void send_list(shaiya::CUser* user);
    void handle_spin(shaiya::CUser* user);
    void update_pending_spins();
}
