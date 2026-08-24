#pragma once
#include <shaiya/include/common.h>
#include <shaiya/include/common/UserTypes.h>

namespace shaiya
{
    #pragma pack(push, 1)
    // TPC_PC_MOVE
    struct GameCharMoveIncoming
    {
        uint16_t opcode{ 0x501 };
        uint8_t motion;
        uint16_t direction;
        float x;
        float y;
        float z;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    struct GameCharAttackUserIncoming
    {
        uint16_t opcode{ 0x502 };
        uint32_t targetId;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    struct GameCharAttackMobIncoming
    {
        uint16_t opcode{ 0x503 };
        uint32_t targetId;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    // TPC_PC_SKILL_USE — offset+0x2 = slot (verificato via Ghidra su handler ps_game.exe
    // 0x478bb2: MOV CL,[EBP+0x2]; CMP CL,0xFF = cancel/stop skill, non un cast).
    struct GameCharSkillUseIncoming
    {
        uint16_t opcode{ 0x511 };
        uint8_t slot;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    // TPC_PC_ITEM_USE_ON_TARGET — 0x557, handler ps_game.exe 0x479006 -> FUN_00472da0.
    // bag/slot risolvono l'item (CUser+0x1C0 + (slot+bag*0x18)*4 -> CItem*), targetId è
    // il personaggio bersaglio (usato da PartyMemberSummon/PartyMemberMoveTo tra gli altri).
    struct GameItemUseOnTargetIncoming
    {
        uint16_t opcode{ 0x557 };
        uint8_t bag;
        uint8_t slot;
        uint32_t targetId;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    // TPC_PC_ITEM_USE — 0x50A, handler ps_game.exe 0x478f8d -> FUN_00472da0 (target
    // implicito = self, [EDI+0xdc]). Variante SENZA bersaglio esplicito — usata da
    // PartyMemberSummon (somma l'intero party, non serve scegliere chi).
    struct GameItemUseIncoming
    {
        uint16_t opcode{ 0x50A };
        uint8_t bag;
        uint8_t slot;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    struct GameCharMotionIncoming
    {
        uint16_t opcode{ 0x506 };
        MotionType motionType;
    };
    #pragma pack(pop)

    enum struct GameCharRebirthType : uint8_t
    {
        NearestTown = 2,
        LeaderResurrect,
        KillSoulByItem,
        KillSoulByItemNoMove
    };

    #pragma pack(push, 1)
    struct GameCharRebirthIncoming
    {
        uint16_t opcode{ 0x553 };
        GameCharRebirthType rebirthType;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    // TPC_ITEM_REPAIR
    struct GameItemRepairIncoming
    {
        uint16_t opcode{ 0x554 };
        uint8_t bag;
        uint8_t slot;
        uint32_t npcId;
    };
    #pragma pack(pop)

    #pragma pack(push, 1)
    struct GameTownMoveScrollIncoming
    {
        uint16_t opcode{ 0x55A };
        uint8_t bag;
        uint8_t slot;
        uint8_t gateIndex;
    };
    #pragma pack(pop)
}
