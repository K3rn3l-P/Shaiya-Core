#include <array>
#include <cstring>
#include <random>
#include <shaiya/include/network/game/outgoing/0800.h>
#include "CGameData.h"
#include "CItem.h"
#include "CUser.h"
#include "CWorld.h"
#include "ItemInfo.h"
#include "Roulette.h"
#include "SConnection.h"
#include "UserHelper.h"
using namespace shaiya;

namespace
{
    constexpr int kInventorySlotCount = 24;
    constexpr uint32_t kRouletteSpinDurationMs = 4500U;

    bool find_item_stack(CUser* user, uint8_t type, uint8_t typeId, uint8_t count, int& outBag, int& outSlot)
    {
        if (!user || !type || !typeId || !count)
            return false;

        for (int bag = 0; bag <= user->bagsUnlocked; ++bag)
        {
            for (int slot = 0; slot < kInventorySlotCount; ++slot)
            {
                auto& item = user->inventory[bag][slot];
                if (!item || !item->count)
                    continue;
                if (item->type != type || item->typeId != typeId)
                    continue;
                if (item->count < count)
                    continue;

                outBag = bag;
                outSlot = slot;
                return true;
            }
        }

        return false;
    }

    bool has_inventory_capacity(CUser* user, ItemInfo* itemInfo, uint8_t count)
    {
        if (!user || !itemInfo || !count)
            return false;

        for (int bag = 1; bag <= user->bagsUnlocked; ++bag)
        {
            for (int slot = 0; slot < kInventorySlotCount; ++slot)
            {
                auto& item = user->inventory[bag][slot];
                if (!item)
                    return true;

                if (item->type != itemInfo->type || item->typeId != itemInfo->typeId)
                    continue;

                if (itemInfo->count > item->count && itemInfo->count - item->count >= count)
                    return true;
            }
        }

        return false;
    }

    void send_spin_result(CUser* user, GameRouletteResult result)
    {
        GameRouletteSpinOutgoing outgoing{};
        outgoing.result = result;
        SConnection::Send(user, &outgoing, sizeof(GameRouletteSpinOutgoing));
    }

    void send_reward_result(CUser* user, GameRouletteResult result, uint8_t rewardIndex = 0, uint8_t rewardType = 0, uint8_t rewardTypeId = 0, uint8_t rewardCount = 0)
    {
        GameRouletteRewardOutgoing outgoing{};
        outgoing.result = result;
        outgoing.rewardIndex = rewardIndex;
        outgoing.rewardType = rewardType;
        outgoing.rewardTypeId = rewardTypeId;
        outgoing.rewardCount = rewardCount;
        SConnection::Send(user, &outgoing, sizeof(GameRouletteRewardOutgoing));
    }

    std::mt19937& roulette_rng()
    {
        static thread_local std::mt19937 rng([]()
            {
                std::random_device seed;
                std::array<std::uint32_t, 8> seedData{};
                for (auto& value : seedData)
                    value = seed();

                std::seed_seq seq(seedData.begin(), seedData.end());
                return std::mt19937(seq);
            }());

        return rng;
    }

    uint8_t choose_reward_index()
    {
        uint16_t totalChance = 0;
        for (uint8_t i = 0; i < g_rouletteConfig.rewardCount; ++i)
            totalChance = static_cast<uint16_t>(totalChance + g_rouletteConfig.rewards[i].chance);

        if (!totalChance)
            return 0;

        std::uniform_int_distribution<int> dist(1, totalChance);
        int roll = dist(roulette_rng());

        int accum = 0;
        for (uint8_t i = 0; i < g_rouletteConfig.rewardCount; ++i)
        {
            accum += g_rouletteConfig.rewards[i].chance;
            if (roll <= accum)
                return i;
        }

        return static_cast<uint8_t>(g_rouletteConfig.rewardCount - 1);
    }

    uint8_t read_spin_progress(uint32_t billingId)
    {
        auto it = g_rouletteSpinProgress.find(billingId);
        return it != g_rouletteSpinProgress.end() ? it->second : 0;
    }

    void write_spin_progress(uint32_t billingId, uint8_t value)
    {
        if (!billingId)
            return;

        if (value == 0)
        {
            g_rouletteSpinProgress.erase(billingId);
            return;
        }

        g_rouletteSpinProgress[billingId] = value;
    }

    void record_recent_winner(CUser* user, uint8_t rewardIndex)
    {
        if (!user || rewardIndex >= g_rouletteConfig.rewardCount)
            return;

        const auto& reward = g_rouletteConfig.rewards[rewardIndex];
        if (!reward.bigWinner)
            return;

        RouletteRecentWinner winner{};
        strncpy_s(winner.name.data(), winner.name.size(), user->charName.data(), _TRUNCATE);
        winner.rewardIndex = rewardIndex;
        winner.wonAt = std::chrono::system_clock::now();

        g_rouletteRecentWinners.push_front(winner);
        while (g_rouletteRecentWinners.size() > kRouletteRecentWinnerCount)
            g_rouletteRecentWinners.pop_back();
    }

    void send_list_locked(CUser* user)
    {
        if (!user)
            return;

        GameRouletteListOutgoing outgoing{};
        outgoing.tokenType = g_rouletteConfig.tokenType;
        outgoing.tokenTypeId = g_rouletteConfig.tokenTypeId;
        outgoing.tokenCount = g_rouletteConfig.tokenCount;
        outgoing.itemCount = g_rouletteConfig.rewardCount;
        outgoing.featuredRewardIndex = g_rouletteConfig.featuredRewardIndex;
        outgoing.guaranteedRewardIndex = g_rouletteConfig.guaranteedRewardIndex;
        outgoing.guaranteedSpinTarget = g_rouletteConfig.guaranteedSpinTarget;
        outgoing.guaranteedSpinProgress = read_spin_progress(user->billingId);

        for (uint8_t i = 0; i < g_rouletteConfig.rewardCount && i < g_rouletteConfig.rewards.size(); ++i)
        {
            outgoing.rewardType[i] = g_rouletteConfig.rewards[i].type;
            outgoing.rewardTypeId[i] = g_rouletteConfig.rewards[i].typeId;
            outgoing.rewardCount[i] = g_rouletteConfig.rewards[i].count;
            outgoing.rewardChance[i] = g_rouletteConfig.rewards[i].chance;
        }

        auto now = std::chrono::system_clock::now();
        const auto recentWinnerCount = std::min(g_rouletteRecentWinners.size(), static_cast<std::size_t>(kGameRouletteRecentWinnerCount));
        outgoing.recentWinnerCount = static_cast<uint8_t>(recentWinnerCount);
        for (std::size_t i = 0; i < outgoing.recentWinnerCount; ++i)
        {
            const auto& winner = g_rouletteRecentWinners[i];
            outgoing.recentWinnerRewardIndex[i] = winner.rewardIndex;
            int64_t ageMinutes = std::chrono::duration_cast<std::chrono::minutes>(now - winner.wonAt).count();
            ageMinutes = std::clamp<int64_t>(ageMinutes, int64_t{ 0 }, int64_t{ UINT16_MAX });
            outgoing.recentWinnerAgeMinutes[i] = static_cast<uint16_t>(ageMinutes);
            outgoing.recentWinnerName[i] = winner.name;
        }

        SConnection::Send(user, &outgoing, sizeof(GameRouletteListOutgoing));
    }
}

void roulette::send_list(CUser* user)
{
    if (!user)
        return;

    std::lock_guard lock(g_rouletteMutex);
    send_list_locked(user);
}

void roulette::handle_spin(CUser* user)
{
    if (!user)
        return;

    std::lock_guard lock(g_rouletteMutex);

    if (!g_rouletteConfig.valid || !g_rouletteConfig.rewardCount)
    {
        send_spin_result(user, GameRouletteResult::NotConfigured);
        return;
    }

    if (g_roulettePending.contains(user->billingId))
    {
        send_spin_result(user, GameRouletteResult::Busy);
        return;
    }

    const auto spinProgress = read_spin_progress(user->billingId);
    uint8_t rewardIndex = choose_reward_index();
    if (g_rouletteConfig.guaranteedRewardIndex < g_rouletteConfig.rewardCount
        && g_rouletteConfig.guaranteedSpinTarget > 0
        && static_cast<uint16_t>(spinProgress + 1) >= g_rouletteConfig.guaranteedSpinTarget)
    {
        rewardIndex = g_rouletteConfig.guaranteedRewardIndex;
    }

    const auto& reward = g_rouletteConfig.rewards[rewardIndex];
    auto itemInfo = CGameData::GetItemInfo(reward.type, reward.typeId);
    if (!itemInfo)
    {
        send_spin_result(user, GameRouletteResult::Failure);
        return;
    }

    int tokenBag = 0;
    int tokenSlot = 0;
    if (!find_item_stack(user, g_rouletteConfig.tokenType, g_rouletteConfig.tokenTypeId, g_rouletteConfig.tokenCount, tokenBag, tokenSlot))
    {
        send_spin_result(user, GameRouletteResult::NoToken);
        return;
    }

    if (!has_inventory_capacity(user, itemInfo, reward.count))
    {
        send_spin_result(user, GameRouletteResult::InventoryFull);
        return;
    }

    RoulettePendingSpin pending{};
    pending.rewardIndex = rewardIndex;
    pending.reward = reward;
    pending.tokenType = g_rouletteConfig.tokenType;
    pending.tokenTypeId = g_rouletteConfig.tokenTypeId;
    pending.tokenCount = g_rouletteConfig.tokenCount;
    pending.spinProgressAtStart = spinProgress;
    pending.completeAt = std::chrono::system_clock::now() + std::chrono::milliseconds(kRouletteSpinDurationMs);

    auto [pendingIt, inserted] = g_roulettePending.emplace(user->billingId, pending);
    if (!inserted)
    {
        send_spin_result(user, GameRouletteResult::Busy);
        return;
    }

    if (!UserHelper::ItemRemove(user, tokenBag, tokenSlot, g_rouletteConfig.tokenCount))
    {
        g_roulettePending.erase(pendingIt);
        send_spin_result(user, GameRouletteResult::Failure);
        return;
    }

    GameRouletteSpinOutgoing outgoing{};
    outgoing.result = GameRouletteResult::Success;
    outgoing.rewardIndex = rewardIndex;
    outgoing.rewardType = reward.type;
    outgoing.rewardTypeId = reward.typeId;
    outgoing.rewardCount = reward.count;
    outgoing.spinDurationMs = kRouletteSpinDurationMs;
    SConnection::Send(user, &outgoing, sizeof(GameRouletteSpinOutgoing));
}

void roulette::update_pending_spins()
{
    std::lock_guard lock(g_rouletteMutex);

    if (g_roulettePending.empty())
        return;

    auto now = std::chrono::system_clock::now();
    for (auto it = g_roulettePending.begin(); it != g_roulettePending.end();)
    {
        if (now < it->second.completeAt)
        {
            ++it;
            continue;
        }

        const RoulettePendingSpin pending = it->second;
        const uint32_t billingId = it->first;

        auto user = CWorld::FindUserBill(billingId);
        if (!user)
        {
            it = g_roulettePending.erase(it);
            continue;
        }

        const auto rewardIndex = pending.rewardIndex;
        const auto& reward = pending.reward;

        auto itemInfo = CGameData::GetItemInfo(reward.type, reward.typeId);
        if (!itemInfo)
        {
            send_reward_result(user, GameRouletteResult::Failure, rewardIndex);
            it = g_roulettePending.erase(it);
            continue;
        }

        if (!has_inventory_capacity(user, itemInfo, reward.count))
        {
            send_reward_result(user, GameRouletteResult::InventoryFull, rewardIndex, reward.type, reward.typeId, reward.count);
            ++it;
            continue;
        }

        it = g_roulettePending.erase(it);

        if (!CUser::ItemCreate(user, itemInfo, reward.count))
        {
            if (auto tokenInfo = CGameData::GetItemInfo(pending.tokenType, pending.tokenTypeId))
                CUser::ItemCreate(user, tokenInfo, pending.tokenCount);

            send_reward_result(user, GameRouletteResult::Failure, rewardIndex, reward.type, reward.typeId, reward.count);
            continue;
        }

        if (g_rouletteConfig.guaranteedSpinTarget > 0)
        {
            auto nextProgress = static_cast<uint8_t>(std::min<int>(
                static_cast<int>(pending.spinProgressAtStart) + 1,
                static_cast<int>(g_rouletteConfig.guaranteedSpinTarget)));

            if (rewardIndex == g_rouletteConfig.guaranteedRewardIndex)
                nextProgress = 0;

            write_spin_progress(billingId, nextProgress);
        }

        record_recent_winner(user, rewardIndex);
        send_reward_result(user, GameRouletteResult::Success, rewardIndex, reward.type, reward.typeId, reward.count);
        send_list_locked(user);
    }
}
