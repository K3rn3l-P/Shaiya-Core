#include <cmath>
#include <mutex>
#include <util/util.h>
#include <shaiya/include/network/game/incoming/0500.h>
#include <shaiya/include/network/game/outgoing/0200.h>
#include "include/main.h"
#include "include/etain_shield_config.h"
#include "include/security_logger.h"
#include "include/shaiya/CObject.h"
#include "include/shaiya/CItem.h"
#include "include/shaiya/CMob.h"
#include "include/shaiya/CUser.h"
#include "include/shaiya/CZone.h"
#include "include/shaiya/ItemInfo.h"
#include "include/shaiya/MobInfo.h"
#include "include/shaiya/NetworkHelper.h"
#include "include/shaiya/SkillInfo.h"
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

    // Mutex del file anticutting-{data}.log — condiviso con v3 (log_channeled_item_use_core),
    // per questo resta fuori dal blocco #if 0 del v1 sotto.
    static std::mutex g_anticutLogMtx;

#if 0
    // RITIRATO (v1) — falsi positivi 16-28% sul gioco legittimo, misurati su dati reali (vedi
    // piano skillcut). Tenuto per riferimento futuro, non compilato.
    // ===================================================================
    //  AntiCutting — anti attack-move-cancel  (default LOG-ONLY)
    // ===================================================================
    //  lock_movement_for_attack() segna la finestra post-attacco
    //  (etainCuttingUntil = now + cuttingLockMs), chiamata dai detour attacco.
    //  In validate_movement, una 0x501 dentro la finestra viene:
    //    - LogOnly=1 : LOGGATA (intervallo attacco->move) e LASCIATA passare
    //    - LogOnly=0 : DROPPATA (enforce — player non si muove)
    //  Log: C:\ShaiyaServer\Logs\EtainShield\anticutting-{data}.log

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

        char line[384];
        int len = std::snprintf(line, sizeof(line),
            "[%02d:%02d:%02d.%03d] user=%-20s interval=%4ums lockMs=%u type=%-6s skillId=%d reset=%u\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            user->username.data(), intervalMs, g_etainConfig.cuttingLockMs,
            (user->etainCuttingSkillId >= 0) ? "Skill" : "Normal",
            user->etainCuttingSkillId,
            static_cast<unsigned>(user->etainCuttingResetTime));
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
#endif

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

    // SkillInfo* dell'attacco skill corrente. Offset verificati dal dispatch ps_game.exe
    // (FUN_00478490): CUser+0xABC = numero skill, CUser+0xAC0 = array puntatori slot,
    // skillObj+0x34 = SkillInfo*. nullptr = normale/non risolto.
    static SkillInfo* current_skill_info(CUser* user)
    {
        if (user->attackType != UserAttackType::Skill)
            return nullptr;
        char* base = reinterpret_cast<char*>(user);
        int count = *reinterpret_cast<int*>(base + 0xABC);
        int idx   = static_cast<int>(user->prevSkillUseIndex);
        if (idx < 0 || idx >= count)
            return nullptr;
        void* skillObj = *reinterpret_cast<void**>(base + 0xAC0 + idx * 4);
        if (!skillObj) return nullptr;
        void* si = *reinterpret_cast<void**>(reinterpret_cast<char*>(skillObj) + 0x34);
        return reinterpret_cast<SkillInfo*>(si);
    }

    // SkillInfo* dallo slot esplicito del pacchetto 0x511 (packet+0x2) — stesso path di
    // risoluzione dell'handler nativo 0x478bb2 (CUser+0xABC=count, CUser+0xAC0+slot*4=array).
    // Più preciso di current_skill_info() per il cast IN ARRIVO ORA: il nostro hook sul
    // dispatch 0x4784D6 gira PRIMA che il nativo aggiorni attackType/prevSkillUseIndex,
    // quindi current_skill_info() rifletterebbe ancora il cast precedente, non questo.
    // Riattivato per v3: validate_skill_use la usa per catturare tick+skillId AL MOMENTO
    // del cast (immediato), non al momento del danno (troppo tardi per skill con windup).
    static SkillInfo* skill_info_from_slot(CUser* user, uint8_t slot)
    {
        char* base = reinterpret_cast<char*>(user);
        int count = *reinterpret_cast<int*>(base + 0xABC);
        if (slot >= count) return nullptr;
        void* skillObj = *reinterpret_cast<void**>(base + 0xAC0 + slot * 4);
        if (!skillObj) return nullptr;
        void* si = *reinterpret_cast<void**>(reinterpret_cast<char*>(skillObj) + 0x34);
        return reinterpret_cast<SkillInfo*>(si);
    }

    // Riga informativa per ogni attacco (skill/normale) nello stesso file anticutting-{data}.log
    // usato da v3 — pura visibilità/correlazione timestamp con ITEM_USE/SKILLCUT_CHAIN, nessuna
    // detection qui (quella è in log_channeled_item_use_core). Rispetta DetectSkill/DetectNormal:
    // se il tipo è disabilitato non contribuirà mai alla catena, quindi non lo logghiamo nemmeno
    // (rumore inutile, e coerente con l'aspettativa che DetectNormal=0 "spenga" il normale).
    static void attack_log(CUser* user, int skillId, uint16_t resetTime)
    {
        bool typeEnabled = (skillId >= 0) ? g_etainConfig.cuttingDetectSkill
                                          : g_etainConfig.cuttingDetectNormal;
        if (!typeEnabled) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs", nullptr);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs\\EtainShield", nullptr);

        char path[MAX_PATH];
        std::snprintf(path, sizeof(path),
            "C:\\ShaiyaServer\\Logs\\EtainShield\\anticutting-%04d-%02d-%02d.log",
            st.wYear, st.wMonth, st.wDay);

        char line[192];
        int len = std::snprintf(line, sizeof(line),
            "[%02d:%02d:%02d.%03d] user=%-20s ATTACK   type=%-14s skillId=%d reset=%u\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            user->username.data(), (skillId >= 0) ? "Skill" : "Normal", skillId,
            static_cast<unsigned>(resetTime));
        if (len <= 0) return;

        std::lock_guard<std::mutex> lk(g_anticutLogMtx);
        HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(h);
    }

    // Chiamata dai detour attacco (0x457F50/0x458000) e dai handler 0x502/0x503.
    // Per le SKILL: NON tocca più nulla — validate_skill_use (sotto) cattura già
    // tick+skillId al momento del CAST (pacchetto 0x511, immediato). Questa funzione
    // (chiamata più tardi, al momento in cui il colpo si risolve/passa il check di
    // range nativo) arriverebbe troppo tardi per skill con windup/cast time, facendo
    // perdere la finestra ChainWindowMs rispetto a un item-uso immediato dopo il cast.
    // Per gli attacchi NORMALI (0x502/0x503) resta l'unico punto di cattura, ed è già
    // immediato (chiamata diretta dal packet handler).
    void lock_movement_for_attack(CUser* user)
    {
        if (!user || !g_etainConfig.enabled || !g_etainConfig.cuttingEnabled)
            return;

        if (current_skill_info(user) != nullptr)
            return;   // skill: già catturata da validate_skill_use, non sovrascrivere

        user->etainLastAttackTick   = GetTickCount();
        user->etainCuttingSkillId   = -1;
        user->etainCuttingResetTime = 0;

        attack_log(user, user->etainCuttingSkillId, user->etainCuttingResetTime);
    }

    bool validate_movement(CUser* user, GameCharMoveIncoming* packet)
    {
        if (!g_etainConfig.enabled)
            return true;

        auto now = GetTickCount();

#if 0
        // RITIRATO (v1) — falsi positivi 16-28% sul gioco legittimo, misurati su dati reali
        // (vedi piano skillcut). Tenuto per riferimento futuro, non compilato.
        // --- AntiCutting (log-only / enforce) ---
        // 0x501 arrivata dentro la finestra post-attacco.
        // DetectSkill/DetectNormal: filtro tipo attacco (etainCuttingSkillId settato da
        // lock_movement_for_attack tramite current_skill_id(); >=0 = skill, -1 = normale).
        bool cuttingTypeEnabled = (user->etainCuttingSkillId >= 0)
            ? g_etainConfig.cuttingDetectSkill
            : g_etainConfig.cuttingDetectNormal;
        if (g_etainConfig.cuttingEnabled && cuttingTypeEnabled && user->etainCuttingUntil != 0
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
#endif

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
        user->etainLastPos           = {};
        user->etainLastMoveTick      = 0;
        user->etainViolationCount    = 0;
        user->etainCuttingUntil      = 0;
        user->etainCuttingSkillId    = -1;
        user->etainLastAttackTick    = 0;
        user->etainCuttingResetTime  = 0;
        user->etainLastSkillCastTick = 0;
        user->etainChainCount        = 0;
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

#if 0
    // RITIRATO (v2) — gate 250ms tra cast skill: mai scattato nei test mirati skillcut (tranne
    // una volta, non conclusivo). Il vero segnale è la catena attacco->item "a canale" (v3, vedi
    // log_channeled_item_use_core più sotto). Tenuto per riferimento futuro, non compilato.
    // ===================================================================
    //  AntiCutting v2 — skillcut server-authoritative (gate 250ms tra cast skill)
    // ===================================================================
    //
    //  Il client (game.exe) impone un gate GLOBALE di 250ms tra due cast skill
    //  qualsiasi: global 0x7c49f8 = tick ultimo cast, check identico a
    //  0x44a7d4 e 0x44b331 ("EAX = now - [0x7c49f8]; CMP EAX,0xfa; JBE reject").
    //  Il fix noto "je -> jmp" a 0x44b308 (essential)/0x44af80 (eric) bypassa
    //  esattamente questo blocco — è il meccanismo dello skillcut.
    //
    //  Client è untrusted: replichiamo lo stesso gate qui, server-side, dove è
    //  autoritativo — un client patchato non può aggirarlo.
    //
    //  Punto di aggancio: opcode 0x511 (cast skill), handler nativo 0x478bb2
    //  verificato via Ghidra — packet+0x2 (byte) = slot skill, 0xFF = cancel/
    //  stop (non un cast, va ignorato).
    static std::mutex g_skillcutLogMtx;

    static void skillcut_log(CUser* user, uint32_t deltaMs, int skillId, uint16_t resetTime)
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
            "[%02d:%02d:%02d.%03d] user=%-20s SKILLCUT delta=%ums minGap=%ums skillId=%d reset=%u\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            user->username.data(), deltaMs, g_etainConfig.cuttingMinSkillGapMs, skillId,
            static_cast<unsigned>(resetTime));
        if (len <= 0) return;

        std::lock_guard<std::mutex> lk(g_skillcutLogMtx);
        HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
        CloseHandle(h);
    }

    int validate_skill_use_v2_retired(CUser* user, GameCharSkillUseIncoming* packet)
    {
        if (!user || !packet) return 1;
        if (!g_etainConfig.enabled || !g_etainConfig.cuttingEnabled)
            return 1;

        // slot 0xFF = cancel/stop skill (verificato: handler 0x478bb2 lo tratta come
        // percorso separato, non è un cast) — non misurare, non conta come cast.
        if (packet->slot == 0xFF)
            return 1;

        auto now = GetTickCount();

        // Primo cast osservato (dopo login/reset) — nessun riferimento precedente, non flaggare.
        if (user->etainLastSkillCastTick == 0)
        {
            user->etainLastSkillCastTick = now;
            return 1;
        }

        uint32_t delta = now - user->etainLastSkillCastTick;

        if (delta < g_etainConfig.cuttingMinSkillGapMs)
        {
            // Risolve SkillInfo dallo slot esplicito del pacchetto (skill che sta per
            // partire ORA) per loggare skillId/resetTime — informativo, non usato per
            // la soglia (v. plan).
            SkillInfo* si = skill_info_from_slot(user, packet->slot);
            int skillId = si ? si->skillId : -1;
            uint16_t resetTime = si ? si->resetTime : 0;

            skillcut_log(user, delta, skillId, resetTime);

            if (!g_etainConfig.cuttingLogOnly)
            {
                if (g_etainConfig.cuttingKickOnViolation)
                    kick_user(user);
                if (g_etainConfig.cuttingBanOnViolation)
                {
                    security_logger::ban_user((int)user->billingId, "AutoBan:SKILLCUT");
                    kick_user(user);
                }
                return 0;   // drop: il client non riceve conferma, la skill non parte
            }
        }

        user->etainLastSkillCastTick = now;
        return 1;
    }
#endif

    // AntiCutting v2 (gate 250ms) RITIRATO (vedi #if 0 sopra) — non droppa più nulla, sempre
    // return 1. Cattura invece qui, AL MOMENTO DEL CAST (pacchetto 0x511, immediato), tick +
    // skillId + resetTime per il check catena v3 (log_channeled_item_use_core): è il punto
    // corretto, prima che l'eventuale windup/cast time della skill ritardi la risoluzione del
    // colpo (lock_movement_for_attack, chiamata dal check nativo di range, arriverebbe troppo
    // tardi rispetto a un item-uso immediato dopo il cast — bug osservato nei log reali).
    int validate_skill_use(CUser* user, GameCharSkillUseIncoming* packet)
    {
        if (!user || !packet) return 1;
        if (!g_etainConfig.enabled || !g_etainConfig.cuttingEnabled)
            return 1;

        // slot 0xFF = cancel/stop skill, non un cast — non aggiorna lo stato catena.
        if (packet->slot == 0xFF)
            return 1;

        SkillInfo* si = skill_info_from_slot(user, packet->slot);
        user->etainLastAttackTick = GetTickCount();
        if (si)
        {
            user->etainCuttingSkillId   = si->skillId;
            user->etainCuttingResetTime = si->resetTime;
        }
        else
        {
            user->etainCuttingSkillId   = -1;
            user->etainCuttingResetTime = 0;
        }

        attack_log(user, user->etainCuttingSkillId, user->etainCuttingResetTime);
        return 1;
    }

    // ===================================================================
    //  AntiCutting v3 — catena attacco -> item "a canale" (rilevatore attivo)
    // ===================================================================
    //
    //  9 tipi di effetto item condividono lo stesso stato differito lato server
    //  CUser+0x58B4 (verificato via Ghidra su FUN_00472da0, ps_game.exe): l'utente
    //  ha confermato che usarli SUBITO DOPO una skill/attacco ne annulla il recovery
    //  ("skillcut") — il canale non si completa, l'item non si consuma.
    //
    //  Qui NON si tocca mai il pacchetto item-use (Party Summon/Move To restano
    //  perfettamente funzionanti, essenziali in PvP) — si rileva la CATENA
    //  (attacco -> item entro cuttingChainWindowMs) e si punisce solo l'account
    //  se si ripete oltre cuttingChainTolerance volte di fila. Un rate-limit
    //  puro sull'item era stato scartato: su un server con migliaia di utenti,
    //  l'uso legittimo ravvicinato di questi item (senza relazione con un
    //  attacco) non deve mai contare come violazione.
    //
    //  Lookup item: stesso path del nativo (FUN_00472da0) — CUser+0x1C0 è l'array
    //  inventario (CItem* per slot), indicizzato (slot + bag*0x18); CItem::info
    //  (0x30) -> ItemInfo*; ItemInfo::effect (0x46) = tipo effetto.
    static const char* channeled_item_label(uint8_t rawEffect)
    {
        switch (rawEffect)
        {
            case static_cast<uint8_t>(ItemEffect::PartyMemberSummon): return "SUMMON";
            case static_cast<uint8_t>(ItemEffect::PartyMemberMoveTo): return "MOVETO";
            case static_cast<uint8_t>(ItemEffect::Teleportation):     return "TELEPORT";
            case static_cast<uint8_t>(ItemEffect::CapitalRecall):     return "CAPITAL_RECALL";
            case static_cast<uint8_t>(ItemEffect::BootleggeryRecall): return "BOOTLEG_RECALL";
            case static_cast<uint8_t>(ItemEffect::ArenaRecall):       return "ARENA_RECALL";
            case static_cast<uint8_t>(ItemEffect::GuildHouseRecall):  return "GUILD_RECALL";
            case static_cast<uint8_t>(ItemEffect::MoveWar):           return "MOVEWAR";
            // Non nominato in ItemEffect.h (gap 74/75 tra MobGen=73 e PremiumLapisiaLv1=76) ma
            // condivide lo stesso tail differito (LAB_00473E05) di CapitalRecall/BootleggeryRecall.
            case 0x4A:                                                return "RECALL_0x4A";
            default:                                                  return nullptr;
        }
    }

    static void log_channeled_item_use_core(CUser* user, uint8_t bag, uint8_t slot,
                                             uint32_t targetId, bool hasTarget)
    {
        if (!user) return;
        if (!g_etainConfig.enabled || !g_etainConfig.cuttingEnabled) return;

        char* base = reinterpret_cast<char*>(user);
        int index = slot + bag * 0x18;
        CItem* item = *reinterpret_cast<CItem**>(base + 0x1C0 + index * 4);
        if (!item || !item->info) return;

        const char* label = channeled_item_label(static_cast<uint8_t>(item->info->effect));
        if (!label) return;   // non uno dei tipi "a canale" monitorati

        auto now = GetTickCount();

        // Catena: l'ultimo attacco (skill/normale) è abbastanza recente E il suo tipo
        // è abilitato da DetectSkill/DetectNormal?
        bool hasRecentAttack = (user->etainLastAttackTick != 0);
        uint32_t deltaFromAttack = hasRecentAttack
            ? (now - user->etainLastAttackTick) : 0;
        bool withinWindow = hasRecentAttack && deltaFromAttack <= g_etainConfig.cuttingChainWindowMs;
        bool typeEnabled = (user->etainCuttingSkillId >= 0)
            ? g_etainConfig.cuttingDetectSkill
            : g_etainConfig.cuttingDetectNormal;
        bool chained = withinWindow && typeEnabled;

        if (chained)
        {
            ++user->etainChainCount;
        }
        else
        {
            // Nessun attacco recente/abilitato entro la finestra: uso isolato dell'item,
            // non è una catena — non lo logghiamo nemmeno (rumore, non skillcut), TRANNE
            // se DebugVerbose=1 (tuning ChainWindowMs: vogliamo vedere il delta reale che
            // stiamo scartando, per capire se la finestra è troppo stretta).
            //
            // IMPORTANTE: NON azzeriamo etainChainCount qui. Un uso isolato scollegato
            // (es. summon usato normalmente qualche secondo dopo una skill, senza intento
            // di skillcut) non deve cancellare le catene VERE già rilevate in precedenza —
            // altrimenti un giocatore che intervalla i tentativi di skillcut con un uso
            // normale non supererebbe mai la tolleranza (bug osservato: 3 catene genuine,
            // ognuna isolata da un uso scollegato in mezzo, mai accumulate). Il contatore
            // si azzera solo su reset_tracking() (teleport/cambio zona/login).
            if (!g_etainConfig.cuttingDebugVerbose)
                return;

            SYSTEMTIME dst;
            GetLocalTime(&dst);
            CreateDirectoryA("C:\\ShaiyaServer\\Logs", nullptr);
            CreateDirectoryA("C:\\ShaiyaServer\\Logs\\EtainShield", nullptr);
            char dpath[MAX_PATH];
            std::snprintf(dpath, sizeof(dpath),
                "C:\\ShaiyaServer\\Logs\\EtainShield\\anticutting-%04d-%02d-%02d.log",
                dst.wYear, dst.wMonth, dst.wDay);
            char dline[192];
            int dlen = hasRecentAttack
                ? std::snprintf(dline, sizeof(dline),
                    "[%02d:%02d:%02d.%03d] user=%-20s ITEM_USE_SKIPPED type=%-14s deltaFromAttack=%ums "
                    "windowMs=%u typeEnabled=%d\r\n",
                    dst.wHour, dst.wMinute, dst.wSecond, dst.wMilliseconds,
                    user->username.data(), label, deltaFromAttack,
                    g_etainConfig.cuttingChainWindowMs, typeEnabled ? 1 : 0)
                : std::snprintf(dline, sizeof(dline),
                    "[%02d:%02d:%02d.%03d] user=%-20s ITEM_USE_SKIPPED type=%-14s deltaFromAttack=none\r\n",
                    dst.wHour, dst.wMinute, dst.wSecond, dst.wMilliseconds,
                    user->username.data(), label);
            if (dlen > 0)
            {
                std::lock_guard<std::mutex> dlk(g_anticutLogMtx);
                HANDLE dh = CreateFileA(dpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (dh != INVALID_HANDLE_VALUE)
                {
                    DWORD dwritten = 0;
                    WriteFile(dh, dline, static_cast<DWORD>(dlen), &dwritten, nullptr);
                    CloseHandle(dh);
                }
            }
            return;
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs", nullptr);
        CreateDirectoryA("C:\\ShaiyaServer\\Logs\\EtainShield", nullptr);

        char path[MAX_PATH];
        std::snprintf(path, sizeof(path),
            "C:\\ShaiyaServer\\Logs\\EtainShield\\anticutting-%04d-%02d-%02d.log",
            st.wYear, st.wMonth, st.wDay);

        char line[288];
        int len;
        if (hasTarget)
            len = std::snprintf(line, sizeof(line),
                "[%02d:%02d:%02d.%03d] user=%-20s ITEM_USE type=%-14s bag=%u slot=%u target=%u chain=%u\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                user->username.data(), label,
                static_cast<unsigned>(bag), static_cast<unsigned>(slot),
                static_cast<unsigned>(targetId), static_cast<unsigned>(user->etainChainCount));
        else
            len = std::snprintf(line, sizeof(line),
                "[%02d:%02d:%02d.%03d] user=%-20s ITEM_USE type=%-14s bag=%u slot=%u target=self chain=%u\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                user->username.data(), label,
                static_cast<unsigned>(bag), static_cast<unsigned>(slot),
                static_cast<unsigned>(user->etainChainCount));
        if (len > 0)
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

        if (user->etainChainCount <= g_etainConfig.cuttingChainTolerance)
            return;   // ancora dentro la tolleranza, nessuna violazione

        // Violazione: catena skillcut confermata oltre la tolleranza.
        char detail[160];
        std::snprintf(detail, sizeof(detail),
            "item=%s skillId=%d chain=%u tolerance=%u windowMs=%u",
            label, user->etainCuttingSkillId, static_cast<unsigned>(user->etainChainCount),
            static_cast<unsigned>(g_etainConfig.cuttingChainTolerance), g_etainConfig.cuttingChainWindowMs);
        etain_log("skillcut_chain", "SKILLCUT_CHAIN", user, detail);
        security_logger::log("server", user->username.data(), "SKILLCUT_CHAIN", detail);

        if (g_etainConfig.cuttingLogOnly)
            return;

        if (g_etainConfig.cuttingKickOnViolation)
            kick_user(user);
        if (g_etainConfig.cuttingBanOnViolation)
        {
            security_logger::ban_user((int)user->billingId, "AutoBan:SKILLCUT_CHAIN");
            kick_user(user);  // ban = rimuove anche la sessione live (come GMSecurity)
        }
    }

    void log_channeled_item_use(CUser* user, GameItemUseOnTargetIncoming* packet)
    {
        if (!packet) return;
        log_channeled_item_use_core(user, packet->bag, packet->slot, packet->targetId, true);
    }

    void log_channeled_item_use_self(CUser* user, GameItemUseIncoming* packet)
    {
        if (!packet) return;
        log_channeled_item_use_core(user, packet->bag, packet->slot, 0, false);
    }

    // AntiDupe: true se l'item slot è bound e il toggle [AntiDupe] BlockBoundItems è attivo.
    // item+0x30 = ItemInfo*; ItemInfo+0x31 = reqOg (0=Tradable, 1=AccountBound, 2=CharacterBound).
    // >=1 → blocca (account/character bound). Null-guard completo.
    bool block_bound_item(void* item)
    {
        if (!item) return false;
        // Weapon skin lock (B): l'arma-istanza in uso come skin non è droppabile/
        // vendibile/tradabile. Indipendente dal toggle anti-dupe.
        if (weapon_skin::is_item_locked(item)) return true;
        if (!g_etainConfig.blockBoundItems) return false;
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
