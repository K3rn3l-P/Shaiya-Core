#include <cmath>
#include <mutex>
#include <util/util.h>
#include <shaiya/include/network/game/incoming/0500.h>
#include <shaiya/include/network/game/outgoing/0200.h>
#include "include/main.h"
#include "include/etain_shield_config.h"
#include "include/security_logger.h"
#include "include/shaiya/CObject.h"
#include "include/shaiya/CMob.h"
#include "include/shaiya/CUser.h"
#include "include/shaiya/CZone.h"
#include "include/shaiya/MobInfo.h"
#include "include/shaiya/NetworkHelper.h"
#include "include/shaiya/SVector.h"
using namespace shaiya;

// ===========================================================================
//  EtainShield — Server-side anticheat module for ps_game.exe
// ===========================================================================
//
//  All protections are configurable via Data/EtainShield.ini (see config header).
//
//  PROTECTIONS
//  -----------
//  AntiSpeedHack   — Calibrated per-packet movement speed validation
//                    Uses real-world speed ceilings measured from the client:
//                      On foot:  max ~12 units/s  (config: 12.5 with 0.5 margin)
//                      Mounted:  max ~13 units/s  (config: 13.5 with 0.5 margin)
//                    Mounted state detected via CUser::vehicleStatus == Riding.
//                    Consecutive violations are forgiven up to the configured
//                    threshold to absorb latency/desync spikes; repeated
//                    violations trigger a rubber-band correction.
//                    Detects speed hacks >= 1.1x reliably.
//                    Also patches four native timing constants in ps_game.exe.
//  AntiRangeHack   — Static euclidean distance checks for attacks and skills
//
//  ARCHITECTURE
//  ------------
//  - This file contains the C++ validation logic and the naked assembly detours.
//  - Packet-level hooks (0x501/0x502/0x503 dispatch) live in packet_pc.cpp.
//  - Per-user state fields live in CUser.h (etainLastPos, etc.).
//  - The single entry point hook::etain_shield() installs all memory patches
//    and detours; it is called once from Main().
//
//  ADDRESS NOTATION
//  ----------------
//  ps_game.exe base = 0x400000.  CE notation "ps_game.exe+OFFSET" maps to
//  absolute VA = 0x400000 + OFFSET.
// ===========================================================================

namespace etain_shield
{
    // ===================================================================
    //  Helpers
    // ===================================================================

    /// Euclidean 2D ground distance (speed hack: ignora Y per non flaggare cadute).
    static float distance_2d(const SVector* a, const SVector* b)
    {
        float dx = a->x - b->x;
        float dz = a->z - b->z;
        return std::sqrt(dx * dx + dz * dz);
    }

    /// Euclidean 3D distance (range hack: include altezza per bloccare attacchi da quota).
    static float distance_3d(const SVector* a, const SVector* b)
    {
        float dx = a->x - b->x;
        float dy = a->y - b->y;
        float dz = a->z - b->z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Cap sul skillRange ricevuto dal client — impedisce che un client modificato
    // mandi skillRange=9999 e bypassi il range check.
    // 80 unità = massimo skill range legittimo del gioco (bow/mage long-range skills).
    static constexpr int kMaxSkillRange = 80;

    /// Read the server-side position from a CUser (CObject.pos at offset 0xD0).
    static SVector* user_pos(CUser* user)
    {
        return reinterpret_cast<SVector*>(reinterpret_cast<char*>(user) + 0xD0);
    }

    /// Read the server-side position from a CMob (CObject.pos at offset 0x7C).
    static SVector* mob_pos(CMob* mob)
    {
        return reinterpret_cast<SVector*>(reinterpret_cast<char*>(mob) + 0x7C);
    }

    // --- MovingGrace: tolleranza range extra quando il bersaglio si muove ---
    // La pos server è in ritardo sul client durante il movimento → attacchi
    // mid-movement falsano la distanza. Aggiungere rangeMovingGrace solo se il
    // target si muove può SOLO ridurre i falsi positivi (è una leniency).

    /// True se il mob sta inseguendo (in movimento).
    static bool is_mob_moving(CMob* mob)
    {
        return mob->status == MobStatus::Chase;
    }

    /// True se l'utente bersaglio si è mosso di recente (~500ms).
    static bool is_user_moving(CUser* target)
    {
        if (target->etainLastMoveTick == 0)
            return false;
        return (GetTickCount() - target->etainLastMoveTick) < 500;
    }

    /// Send a position correction packet to a single client.
    static void send_pos_correction(CUser* user, const SVector& pos)
    {
        GameUserSetMapPosOutgoing pkt{};
        pkt.objectId = user->id;
        pkt.mapId    = user->mapId;
        pkt.x        = pos.x;
        pkt.y        = pos.y;
        pkt.z        = pos.z;
        NetworkHelper::Send(user, &pkt, sizeof(pkt));
    }

    // ===================================================================
    //  kick_user — disconnessione reale della sessione
    // ===================================================================
    //
    //  IMPORTANTE: settare user->closeBySession = 1 NON disconnette.
    //  Verificato su ps_game.exe (Ghidra):
    //    - closeBySession (CUser+0x5858) e' solo un GUARD: una volta != 0
    //      l'utente viene SALTATO nei loop di send/process (0x414171,0x4143cc),
    //      ma il socket resta aperto.
    //    - La disconnessione vera la fa FUN_00414b20: incrementa closeBySession
    //      e, SOLO se diventa == 1, chiama FUN_004ec760(this=CUser, reason, 0)
    //      che fa closesocket(CUser+0x68).
    //  => Pre-settare closeBySession = 1 e' DANNOSO: quando la close nativa
    //     gira fa INC->2, CMP 2==1 falso e SALTA closesocket.
    //
    //  Qui replichiamo il corpo nativo di FUN_00414b20 usando il CUser* che
    //  gia' possediamo (saltando il lookup per uid FUN_00415480), sotto la
    //  stessa critical section del manager utenti (*0x587960 + 0xA0).
    //  reason 0x67 = codice kick standard (casi 0x802/0x809 del dispatcher).
    void kick_user(CUser* user)
    {
        if (!user)
            return;

        LPCRITICAL_SECTION cs =
            reinterpret_cast<LPCRITICAL_SECTION>(*reinterpret_cast<uintptr_t*>(0x00587960) + 0xA0);
        typedef void(__thiscall* CloseConn_t)(void* self, int reason, int zero);
        auto CloseConn = reinterpret_cast<CloseConn_t>(0x004EC760);

        EnterCriticalSection(cs);
        if (++user->closeBySession == 1)   // closeBySession = int32 @0x5858
            CloseConn(user, 0x67, 0);      // FUN_004ec760: closesocket(CUser+0x68)
        LeaveCriticalSection(cs);
    }

    // ===================================================================
    //  AntiSpeedHack — Constant Patching
    // ===================================================================
    //
    //  Overwrites four hardcoded timing constants in the ps_game.exe data
    //  section to tighten the native speed-validation window.  These are
    //  fixed values (not configurable via INI) — the real protection comes
    //  from validate_movement() with calibrated speed ceilings.

    static void apply_speedhack_constants()
    {
        constexpr double c1 = 10.0;
        util::write_memory((void*)0x5740D8, &c1, sizeof(c1));

        constexpr float c2 = 0.13f;
        util::write_memory((void*)0x5740E4, &c2, sizeof(c2));

        constexpr float c3 = 3.0f;
        util::write_memory((void*)0x5740D0, &c3, sizeof(c3));

        constexpr double c4 = 2.0;
        util::write_memory((void*)0x5740C8, &c4, sizeof(c4));
    }

    // ===================================================================
    //  AntiSpeedHack — Active Movement Validation
    // ===================================================================
    //
    //  validate_movement() is the single entry point for every 0x501
    //  packet.  It runs the speed validation.
    //
    //  Speed validation uses calibrated coefficients measured server-side
    //  via OutputDebugString logging of actual 0x501 packet distances/times.
    //  Server-observed speeds differ from the client speed monitor (~7/~10)
    //  because packets arrive at irregular intervals (~100–600ms) rather
    //  than per-frame, and GetTickCount() resolution differs from QPC.
    //
    //  Calibrated ceilings (server-side measurement):
    //    On foot: ~12 units/s  → threshold 12.5  (0.5 margin)
    //    Mounted: ~13 units/s  → threshold 13.5  (0.5 margin)
    //
    //  Violation policy:
    //    Below threshold: forgiven (possible latency spike, desync, packet burst)
    //    At threshold+:   rubber-band correction (every violation)
    //  Violations reset to 0 on any legitimate movement packet.
    //
    //  IMPORTANT: on violation, etainLastPos MUST advance to the packet
    //  position.  Otherwise subsequent packets measure accumulated distance
    //  from a stale position, causing a cascading false-positive spiral.

    // ===================================================================
    //  AntiCutting — anti attack-move-cancel  (default LOG-ONLY)
    // ===================================================================
    //  lock_movement_for_attack() segna la finestra post-attacco
    //  (etainCuttingUntil = now + cuttingLockMs), chiamata dai detour attacco.
    //  In validate_movement, una 0x501 dentro la finestra viene:
    //    - LogOnly=1 : LOGGATA (intervallo attacco->move) e LASCIATA passare
    //    - LogOnly=0 : DROPPATA (enforce — player non si muove)
    //  Log: C:\ShaiyaServer\Logs\EtainShield\anticutting-{data}.log

    static std::mutex g_anticutLogMtx;

    static void anticutting_log(CUser* user, uint32_t intervalMs)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs", nullptr);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs\\EtainShield", nullptr);

        char path[MAX_PATH];
        std::snprintf(path, sizeof(path),
            "C:\\ShaiyaServer\\Logs\\EtainShield\\anticutting-%04d-%02d-%02d.log",
            st.wYear, st.wMonth, st.wDay);

        char line[256];
        int len = std::snprintf(line, sizeof(line),
            "[%02d:%02d:%02d] user=%-20s interval=%4ums lockMs=%u type=%-6s skillId=%d\r\n",
            st.wHour, st.wMinute, st.wSecond,
            user->username.data(), intervalMs, g_etainConfig.cuttingLockMs,
            (user->etainCuttingSkillId >= 0) ? "Skill" : "Normal",
            user->etainCuttingSkillId);
        if (len <= 0) return;

        {
            std::lock_guard<std::mutex> lk(g_anticutLogMtx);
            HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
                CloseHandle(h);
            }
        }

        // AntiCutting è ancora in test → resta SOLO nel suo file anticutting-{data}.log.
        // Per attivarlo anche nell'aggregato GMSecurity (security_{data}.log) + DB
        // SecurityEvents, decommentare il blocco sotto.
        //char detail[128];
        //std::snprintf(detail, sizeof(detail),
        //    "interval=%ums lockMs=%u type=%s skillId=%d",
        //    intervalMs, g_etainConfig.cuttingLockMs,
        //    (user->etainCuttingSkillId >= 0) ? "Skill" : "Normal",
        //    user->etainCuttingSkillId);
        //security_logger::log("server", user->username.data(), "CUTTING", detail);
    }

    // Log EtainShield → cartella dedicata, un file per rilevazione (come AntiCutting):
    //   C:\ShaiyaServer\Logs\EtainShield\{logName}-{data}.log
    //   es. speedhack-2026-06-23.log, rangehack-2026-06-23.log
    static std::mutex g_etainLogMtx;

    static void etain_log(const char* logName, const char* event, CUser* user, const char* detail)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs", nullptr);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs\\EtainShield", nullptr);

        char path[MAX_PATH];
        std::snprintf(path, sizeof(path),
            "C:\\ShaiyaServer\\Logs\\EtainShield\\%s-%04d-%02d-%02d.log",
            logName, st.wYear, st.wMonth, st.wDay);

        char line[384];
        int len = std::snprintf(line, sizeof(line),
            "[%02d:%02d:%02d] %-16s user=%-20s %s\r\n",
            st.wHour, st.wMinute, st.wSecond,
            event, user->username.data(), detail);
        if (len <= 0) return;

        std::lock_guard<std::mutex> lk(g_etainLogMtx);
        HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(h);
    }

    // skillId dell'attacco skill corrente. Offset verificati dal dispatch ps_game.exe
    // (FUN_00478490): CUser+0xABC = numero skill, CUser+0xAC0 = array puntatori slot,
    // skillObj+0x34 = SkillInfo*, SkillInfo+0x00 = skillId. -1 = normale/non risolto.
    static int current_skill_id(CUser* user)
    {
        if (user->attackType != UserAttackType::Skill)
            return -1;
        char* base = reinterpret_cast<char*>(user);
        int count = *reinterpret_cast<int*>(base + 0xABC);
        int idx   = static_cast<int>(user->prevSkillUseIndex);
        if (idx < 0 || idx >= count)
            return -1;
        void* skillObj = *reinterpret_cast<void**>(base + 0xAC0 + idx * 4);
        if (!skillObj) return -1;
        void* si = *reinterpret_cast<void**>(reinterpret_cast<char*>(skillObj) + 0x34);
        if (!si) return -1;
        return *reinterpret_cast<uint16_t*>(si);   // SkillInfo+0x00
    }

    // Chiamata dai detour attacco (0x457F50/0x458000) e dai handler 0x502/0x503.
    // Segna la finestra + cattura lo skillId (per SkipSkillIds futuro + tuning log).
    // Log-only: nessuna esenzione SkipSkillIds (registriamo tutto, incl. dash).
    void lock_movement_for_attack(CUser* user)
    {
        if (!user || !g_etainConfig.enabled || !g_etainConfig.cuttingEnabled)
            return;
        user->etainCuttingSkillId = current_skill_id(user);
        user->etainCuttingUntil   = GetTickCount() + g_etainConfig.cuttingLockMs;
    }

    bool validate_movement(CUser* user, GameCharMoveIncoming* packet)
    {
        if (!g_etainConfig.enabled)
            return true;

        auto now = GetTickCount();

        // --- AntiCutting (log-only / enforce) ---
        // 0x501 arrivata dentro la finestra post-attacco.
        if (g_etainConfig.cuttingEnabled && user->etainCuttingUntil != 0
            && static_cast<int32_t>(now - user->etainCuttingUntil) < 0)
        {
            uint32_t intervalMs = g_etainConfig.cuttingLockMs
                - static_cast<uint32_t>(user->etainCuttingUntil - now);
            anticutting_log(user, intervalMs);

            if (!g_etainConfig.cuttingLogOnly)
            {
                // Enforce: kick/ban opzionali, poi droppa la move e sync speed
                // tracking per non falsare il check.
                if (g_etainConfig.cuttingKickOnViolation)
                    kick_user(user);
                if (g_etainConfig.cuttingBanOnViolation)
                {
                    security_logger::ban_user((int)user->billingId, "AutoBan:CUTTING");
                    kick_user(user);  // ban = rimuove anche la sessione live (come GMSecurity)
                }
                user->etainLastMoveTick = now;
                return false;
            }
            // Log-only: logga solo la prima move post-attacco, poi consuma la finestra.
            user->etainCuttingUntil = 0;
        }

        // --- AntiSpeedHack ---
        if (!g_etainConfig.speedHackEnabled)
            return true;

        auto& c = g_etainConfig;

        SVector packetPos = { packet->x, packet->y, packet->z };

        // First packet after login / teleport — seed tracking state.
        if (user->etainLastMoveTick == 0)
        {
            user->etainLastPos        = packetPos;
            user->etainLastMoveTick   = now;
            user->etainViolationCount = 0;
            return true;
        }

        auto elapsed = now - user->etainLastMoveTick;

        // Sub-50ms: too short to measure meaningfully — accept and advance.
        if (elapsed < 50)
        {
            user->etainLastPos      = packetPos;
            user->etainLastMoveTick = now;
            return true;
        }

        // 2D ground distance from last validated position.
        float distance = distance_2d(&packetPos, &user->etainLastPos);

        // Player didn't move (or moved negligibly) — not a speed hack.
        // Accept and advance the timestamp so the next real move is
        // measured from this point.
        if (distance < 0.1f)
        {
            user->etainLastPos      = packetPos;
            user->etainLastMoveTick = now;
            // Don't reset violations — standing still doesn't prove innocence.
            return true;
        }

        // Pick the right ceiling based on mounted state.
        bool mounted = (user->vehicleStatus == UserVehicleStatus::Riding);
        float maxSpeed = mounted ? c.speedMaxMounted : c.speedMaxOnFoot;

        // maxDist = how far this player could legitimately travel.
        float maxDist = maxSpeed * (static_cast<float>(elapsed) / 1000.0f);

        if (distance <= maxDist)
        {
            // Legitimate movement — advance tracking, clear violations.
            user->etainLastPos        = packetPos;
            user->etainLastMoveTick   = now;
            user->etainViolationCount = 0;
            return true;
        }

        // --- Violation ---
        ++user->etainViolationCount;

        // CRITICAL: always advance tracking position and time.
        // If we don't, the next packet measures distance from the stale
        // position with a fresh elapsed → cascading false positives
        // (speed appears to grow exponentially each packet).
        user->etainLastMoveTick = now;
        user->etainLastPos      = packetPos;

        // Early violations are forgiven (latency spikes, desync).
        if (user->etainViolationCount < c.speedViolationThreshold)
            return true;

        // Soglia raggiunta: logga questo pacchetto in violazione.
        char detail[128];
        std::snprintf(detail, sizeof(detail),
            "dist=%.2f max=%.2f elapsed=%ums viol=%d",
            distance, maxDist, elapsed, (int)user->etainViolationCount);
        etain_log("speedhack", "SPEED_HACK", user, detail);
        security_logger::log("server", user->username.data(), "SPEED_HACK", detail);

        // Log-only: osserva senza enforce — lascia passare la move (niente rubber-band).
        if (c.speedLogOnly)
            return true;

        // Enforce: kick/ban opzionali + rubber-band.
        if (c.speedKickOnViolation)
            kick_user(user);
        if (c.speedBanOnViolation)
        {
            security_logger::ban_user((int)user->billingId, "AutoBan:SPEED_HACK");
            kick_user(user);  // ban = rimuove anche la sessione live (come GMSecurity)
        }

        send_pos_correction(user, user->etainLastPos);
        return false;
    }

    void reset_tracking(CUser* user)
    {
        user->etainLastPos          = {};
        user->etainLastMoveTick     = 0;
        user->etainViolationCount   = 0;
        user->etainCuttingUntil     = 0;
        user->etainCuttingSkillId   = -1;
    }

    // ===================================================================
    //  AntiRangeHack — Attack Distance Validation
    // ===================================================================
    //
    //  Two layers:
    //    A) Packet interception (0x502 / 0x503) — basic attacks only
    //    B) Native function detours (0x458000 / 0x457F50) — all attacks + skills
    //
    //  Both layers compute the real 2D euclidean distance from server-side
    //  positions and compare against:
    //    allowed = max(abilityAttackRange, skillRange) + targetSize + margin

    int validate_pve_range(CUser* user, CMob* mob, int skillRange)
    {
        if (!g_etainConfig.enabled || !g_etainConfig.rangeHackEnabled)
            return 1;

        if (!user || !mob || !mob->info)
            return 0;

        if (skillRange > kMaxSkillRange) skillRange = kMaxSkillRange;

        float dist = distance_3d(user_pos(user), mob_pos(mob));

        int range = user->abilityAttackRange;
        if (skillRange > range) range = skillRange;

        int grace = is_mob_moving(mob) ? g_etainConfig.rangeMovingGrace : 0;
        float allowed = static_cast<float>(
            range + static_cast<int>(mob->info->size) + g_etainConfig.rangeMargin + grace);

        return (dist <= allowed) ? 1 : 0;
    }

    int validate_pvp_range(CUser* attacker, CUser* target, int skillRange)
    {
        if (!g_etainConfig.enabled || !g_etainConfig.rangeHackEnabled)
            return 1;

        if (!attacker || !target)
            return 0;

        if (skillRange > kMaxSkillRange) skillRange = kMaxSkillRange;

        float dist = distance_3d(user_pos(attacker), user_pos(target));

        int range = attacker->abilityAttackRange;
        if (skillRange > range) range = skillRange;

        int grace = is_user_moving(target) ? g_etainConfig.rangeMovingGrace : 0;
        float allowed = static_cast<float>(range + 1 + g_etainConfig.rangeMargin + grace);

        return (dist <= allowed) ? 1 : 0;
    }

    int validate_attack_user(CUser* user, GameCharAttackUserIncoming* packet)
    {
        lock_movement_for_attack(user);  // AntiCutting: finestra su attacco base 0x502

        if (!g_etainConfig.enabled || !g_etainConfig.rangeHackEnabled)
            return 1;

        if (!user || !user->zone)
            return 1;

        auto* target = CZone::FindUser(user->zone, packet->targetId);
        if (!target)
            return 1;

        float dist = distance_3d(user_pos(user), user_pos(target));
        int grace = is_user_moving(target) ? g_etainConfig.rangeMovingGrace : 0;
        float allowed = static_cast<float>(
            user->abilityAttackRange + 1 + g_etainConfig.rangeMargin + grace);

        if (dist <= allowed) return 1;

        char detail[128];
        std::snprintf(detail, sizeof(detail),
            "dist=%.2f allowed=%.2f target=%u",
            dist, allowed, packet->targetId);
        etain_log("rangehack", "RANGE_HACK_PVP", user, detail);
        security_logger::log("server", user->username.data(), "RANGE_HACK_PVP", detail);
        if (g_etainConfig.rangeLogOnly)
            return 1;   // osserva: logga e lascia passare l'attacco
        if (g_etainConfig.rangeKickOnViolation)
            kick_user(user);
        if (g_etainConfig.rangeBanOnViolation)
        {
            security_logger::ban_user((int)user->billingId, "AutoBan:RANGE_HACK_PVP");
            kick_user(user);  // ban = rimuove anche la sessione live (come GMSecurity)
        }
        return 0;
    }

    int validate_attack_mob(CUser* user, GameCharAttackMobIncoming* packet)
    {
        lock_movement_for_attack(user);  // AntiCutting: finestra su attacco base 0x503

        if (!g_etainConfig.enabled || !g_etainConfig.rangeHackEnabled)
            return 1;

        if (!user || !user->zone)
            return 1;

        auto* mob = CZone::FindMob(user->zone, packet->targetId);
        if (!mob || !mob->info)
            return 1;

        float dist = distance_3d(user_pos(user), mob_pos(mob));
        int grace = is_mob_moving(mob) ? g_etainConfig.rangeMovingGrace : 0;
        float allowed = static_cast<float>(
            user->abilityAttackRange + static_cast<int>(mob->info->size) +
            g_etainConfig.rangeMargin + grace);

        if (dist <= allowed) return 1;

        char detail[128];
        std::snprintf(detail, sizeof(detail),
            "dist=%.2f allowed=%.2f mob=%u",
            dist, allowed, packet->targetId);
        etain_log("rangehack", "RANGE_HACK_PVE", user, detail);
        security_logger::log("server", user->username.data(), "RANGE_HACK_PVE", detail);
        if (g_etainConfig.rangeLogOnly)
            return 1;   // osserva: logga e lascia passare l'attacco
        if (g_etainConfig.rangeKickOnViolation)
            kick_user(user);
        if (g_etainConfig.rangeBanOnViolation)
        {
            security_logger::ban_user((int)user->billingId, "AutoBan:RANGE_HACK_PVE");
            kick_user(user);  // ban = rimuove anche la sessione live (come GMSecurity)
        }
        return 0;
    }

    // AntiDupe: true se l'item slot è bound e il toggle [AntiDupe] BlockBoundItems è attivo.
    // item+0x30 = ItemInfo*; ItemInfo+0x31 = reqOg (0=Tradable, 1=AccountBound, 2=CharacterBound).
    // >=1 → blocca (account/character bound). Null-guard completo.
    bool block_bound_item(void* item)
    {
        if (!g_etainConfig.blockBoundItems) return false;
        if (!item) return false;
        void* info = *reinterpret_cast<void**>(reinterpret_cast<char*>(item) + 0x30);
        if (!info) return false;
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(info) + 0x31) >= 1;
    }
}

// ===========================================================================
//  Exported function — backward compatibility with RangeHack.CT
// ===========================================================================

extern "C" __declspec(dllexport)
bool __cdecl EnableAttackRange(
    shaiya::CUser* user, shaiya::SVector* targetPos,
    int targetSize, int skillRange, int margin)
{
    int range = user->abilityAttackRange;
    if (skillRange > range) range = skillRange;

    float dist = etain_shield::distance_2d(
        etain_shield::user_pos(user), targetPos);

    return dist <= static_cast<float>(range + targetSize + margin);
}

// ===========================================================================
//  Naked assembly detours — AntiRangeHack Layer B
// ===========================================================================
//
//  Each detour replaces the prologue of a native range-check function.
//  On pass: restore original prologue and jump in.
//  On fail: return al=0 immediately.

// --- PVE: 0x458000 ---
// Registers: ebx = user.  Stack: [esp+4] = mob, [esp+8] = skillRange.
// Returns al.  retn 0x08.
// Original 8 bytes: sub esp,0x10 / push ebp / mov ebp,[esp+0x18]
unsigned u0x458008 = 0x458008;
void __declspec(naked) naked_0x458000()
{
    __asm
    {
        pushad
        mov eax,[esp+0x28]      // skillRange (stack, after pushad)
        mov edx,[esp+0x24]      // mob        (stack, after pushad)
        push eax
        push edx
        push ebx                // user
        call etain_shield::validate_pve_range
        add esp,0xC
        test eax,eax
        popad
        jz blocked

        // AntiCutting: segna finestra post-attacco (ebx = user)
        pushad
        push ebx
        call etain_shield::lock_movement_for_attack
        add esp,0x4
        popad

        sub esp,0x10
        push ebp
        mov ebp,[esp+0x18]
        jmp u0x458008

    blocked:
        xor al,al
        retn 0x08
    }
}

// --- PVP: 0x457F50 ---
// Registers: edi = attacker, ebx = target, ecx = skillRange.
// Returns al.  retn.
// Original 9 bytes: sub esp,0x18 / mov eax,[edi+0x12F0]
unsigned u0x457F59 = 0x457F59;
void __declspec(naked) naked_0x457F50()
{
    __asm
    {
        pushad
        push ecx                // skillRange
        push ebx                // target
        push edi                // attacker
        call etain_shield::validate_pvp_range
        add esp,0xC
        test eax,eax
        popad
        jz blocked

        // AntiCutting: segna finestra post-attacco (edi = attacker)
        pushad
        push edi
        call etain_shield::lock_movement_for_attack
        add esp,0x4
        popad

        sub esp,0x18
        mov eax,[edi+0x12F0]
        jmp u0x457F59

    blocked:
        xor al,al
        retn
    }
}

// ===========================================================================
//  hook::etain_shield() — public entry point, called once from Main()
// ===========================================================================

void hook::etain_shield()
{
    if (!g_etainConfig.enabled)
        return;

    // AntiSpeedHack: patch timing constants.
    if (g_etainConfig.speedHackEnabled)
        etain_shield::apply_speedhack_constants();

    // AntiRangeHack: detour native range-check functions.
    if (g_etainConfig.rangeHackEnabled)
    {
        util::detour((void*)0x458000, naked_0x458000, 8);
        util::detour((void*)0x457F50, naked_0x457F50, 9);
    }

    // AntiSpeedHack active validation is checked at runtime inside
    // validate_movement() — its hook lives in packet_pc.cpp (0x501 dispatch).
}
