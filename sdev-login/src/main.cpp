#include <array>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <util/util.h>
#include "include/gm_security.h"
#include "include/login_logger.h"
#include "include/main.h"
#include "include/shaiya/SDatabase.h"
using namespace shaiya;

// ===========================================================================
// HWID cache: mappa ip → {hwid, sig}, popolata al momento del 0xA101 RSA handshake.
// Usata da hook_0x406A8C per associare HWID + sig allo username al momento del login.
// La sig viene passata a verify_hwid() per verifica HMAC server-side (chiave in GMSecurity.ini).
// ===========================================================================
namespace hwid_cache
{
    struct Entry { std::string hwid; std::string sig; };
    static std::unordered_map<std::string, Entry> g_map;
    static std::mutex g_mtx;

    static void store(const char* ip, const char* hwid, const char* sig)
    {
        if (!ip || !hwid) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        size_t hlen = 0;
        while (hwid[hlen] && hlen < 32) ++hlen;
        size_t slen = 0;
        while (sig && sig[slen] && slen < 64) ++slen;
        Entry e;
        e.hwid = std::string(hwid, hlen);
        e.sig  = sig ? std::string(sig, slen) : std::string();
        g_map[ip] = std::move(e);
    }

    static bool fetch(const char* ip, char* hwid_out, size_t hwid_len,
                      char* sig_out, size_t sig_len)
    {
        if (!ip || !hwid_out || hwid_len < 2) return false;
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_map.find(ip);
        if (it == g_map.end()) return false;
        strncpy_s(hwid_out, hwid_len, it->second.hwid.c_str(), hwid_len - 1);
        if (sig_out && sig_len >= 2)
            strncpy_s(sig_out, sig_len, it->second.sig.c_str(), sig_len - 1);
        return true;
    }

    static void remove(const char* ip)
    {
        if (!ip) return;
        std::lock_guard<std::mutex> lk(g_mtx);
        g_map.erase(ip);
    }
}

// ===========================================================================
// Brute force guard — due livelli: per-IP (fast) + per-account (global)
//
// Per-IP: 5 fail / 10 min → blocca l'IP. Bypassabile con IP rotation.
// Per-account: 15 fail → lock 30 min. Resiste a IP rotation.
//   Progressivo: 15=30min, 20=2h, 25=8h. Resettato a successo.
// ===========================================================================
namespace login_guard
{
    // -----------------------------------------------------------------------
    // Layer 1 — per IP
    // -----------------------------------------------------------------------
    static constexpr int   MAX_FAILS_IP  = 5;
    static constexpr DWORD WINDOW_IP_MS  = 600000; // 10 minuti

    struct IpEntry { int fails; DWORD first_fail; };
    static std::unordered_map<std::string, IpEntry> g_ip_fails;
    static std::mutex g_ip_mtx;

    static bool ip_is_allowed(const char* ip)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_ip_mtx);
        auto it = g_ip_fails.find(ip);
        if (it == g_ip_fails.end()) return true;
        auto& e = it->second;
        if (now - e.first_fail > WINDOW_IP_MS) { g_ip_fails.erase(it); return true; }
        return e.fails < MAX_FAILS_IP;
    }

    static void ip_on_fail(const char* ip)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_ip_mtx);
        auto& e = g_ip_fails[ip];
        if (e.fails == 0 || now - e.first_fail > WINDOW_IP_MS) { e.first_fail = now; e.fails = 1; }
        else e.fails++;
    }

    static void ip_on_success(const char* ip)
    {
        std::lock_guard<std::mutex> lk(g_ip_mtx);
        g_ip_fails.erase(ip);
    }

    // -----------------------------------------------------------------------
    // Layer 2 — per account (username, case-insensitive)
    // -----------------------------------------------------------------------
    struct AccEntry { int fails; DWORD locked_until; };
    static std::unordered_map<std::string, AccEntry> g_acc_fails;
    static std::mutex g_acc_mtx;

    static DWORD lock_duration_ms(int fails)
    {
        if (fails >= 25) return 8 * 3600000u;  // 8 ore
        if (fails >= 20) return 2 * 3600000u;  // 2 ore
        if (fails >= 15) return 1800000u;       // 30 min
        return 0;
    }

    static std::string normalize_username(const char* u)
    {
        std::string s(u ? u : "");
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool acc_is_allowed(const char* username)
    {
        auto now = GetTickCount();
        auto key = normalize_username(username);
        std::lock_guard<std::mutex> lk(g_acc_mtx);
        auto it = g_acc_fails.find(key);
        if (it == g_acc_fails.end()) return true;
        auto& e = it->second;
        if (e.locked_until && now - e.locked_until < 0x80000000u) return false;
        return true;
    }

    static void acc_on_fail(const char* username)
    {
        auto now = GetTickCount();
        auto key = normalize_username(username);
        std::lock_guard<std::mutex> lk(g_acc_mtx);
        auto& e = g_acc_fails[key];
        ++e.fails;
        DWORD dur = lock_duration_ms(e.fails);
        e.locked_until = dur ? (now + dur) : 0;
        if (dur) {
            char detail[64];
            std::snprintf(detail, sizeof(detail), "fails=%d lock=%us", e.fails, dur / 1000);
            security_logger::log("0.0.0.0", username, "ACC_LOCKOUT", detail);
        }
    }

    static void acc_on_success(const char* username)
    {
        auto key = normalize_username(username);
        std::lock_guard<std::mutex> lk(g_acc_mtx);
        g_acc_fails.erase(key);
    }

    // -----------------------------------------------------------------------
    // API pubblica (backward-compatible)
    // -----------------------------------------------------------------------
    static bool is_allowed(const char* ip)          { return ip_is_allowed(ip); }
    static bool is_acc_allowed(const char* username) { return acc_is_allowed(username); }
    static void on_fail(const char* ip)             { ip_on_fail(ip); }
    static void on_acc_fail(const char* username)   { acc_on_fail(username); }
    static void on_success(const char* ip)          { ip_on_success(ip); }
    static void on_acc_success(const char* username){ acc_on_success(username); }
}

// ===========================================================================
// Login hook
// ===========================================================================

short hook_0x406A8C(SDatabase* db, char* username, char* password, unsigned lowPart, unsigned highPart, char* ipv4)
{
    if (!login_guard::is_allowed(ipv4))
    {
        login_logger::log_fail(ipv4, username, "BRUTE_FORCE_BLOCKED_IP");
        return -1;
    }

    if (!login_guard::is_acc_allowed(username))
    {
        login_logger::log_fail(ipv4, username, "BRUTE_FORCE_BLOCKED_ACC");
        return -1;
    }

    if (!gm_security::is_login_allowed(db, username, ipv4))
    {
        login_logger::log_fail(ipv4, username, "WHITELIST_DENIED");
        return -1;
    }

    ULARGE_INTEGER sessionId{ lowPart, highPart };
    std::array<char, 1024> buffer{};

    int argonResult = 0;

    if (!gm_security::verify_password(db, username, password, &argonResult))
    {
        login_guard::on_fail(ipv4);
        login_guard::on_acc_fail(username);
        login_logger::log_fail(ipv4, username, "PASSWORD_FAIL");
        return -1;
    }

    login_guard::on_success(ipv4);
    login_guard::on_acc_success(username);
    login_logger::log_ok(ipv4, username);

    // Sprint 1: verifica HWID + sig (cache ip→{hwid,sig} popolata da 0xA101)
    {
        char hwid_buf[33] = {};
        char sig_buf[65]  = {};
        bool hwid_ok;
        if (hwid_cache::fetch(ipv4, hwid_buf, sizeof(hwid_buf), sig_buf, sizeof(sig_buf)))
            hwid_ok = gm_security::verify_hwid(db, username, hwid_buf, sig_buf, ipv4);
        else
            hwid_ok = gm_security::verify_hwid(db, username, "", "", ipv4);
        hwid_cache::remove(ipv4);
        if (!hwid_ok)
            return -1; // disconnect: HWID bloccato, sig invalida o HWID mancante con EnforceKick=1
    }

    std::snprintf(buffer.data(), buffer.size(),
        "EXEC [PS_UserData].[dbo].[usp_Try_GameLogin_Taiwan]"
        " @UserID=?, @InPassword='', @SessionID=%llu, @UserIP='%s', @ArgonResult=%d;",
        sessionId.QuadPart, ipv4, argonResult);

    if (SDatabase::PrepareSql(db, buffer.data()))
        return -1;

    // Check each bind independently: previously the first BindParameter's
    // result was overwritten by the second, swallowing a param-1 failure.
    if (SDatabase::BindParameter(db, 1, 32, SQL_C_CHAR, SQL_VARCHAR, username, nullptr, SQL_PARAM_INPUT))
        return -1;
    if (SDatabase::BindParameter(db, 2, 32, SQL_C_CHAR, SQL_VARCHAR, password, nullptr, SQL_PARAM_INPUT))
        return -1;

    return SDatabase::ExecuteSql(db);
}

unsigned u0x406B24 = 0x406B24;
unsigned u0x406ADF = 0x406ADF;
void __declspec(naked) naked_0x406A8C()
{
    __asm
    {
        pushad

        mov ebp,[esp+0x44C]
        mov eax,[esp+0x448]
        mov ecx,[esp+0x444]
        mov esi,[esp+0x440]
        mov edi,[esp+0x43C]

        push ebp // ipv4

        // sessionId
        push eax
        push ecx

        push esi // password
        push edi // username
        push ebx // database
        call hook_0x406A8C
        add esp,0x18
        test ax,ax

        popad

        jne _0x406B24
        jmp u0x406ADF

        _0x406B24:
        jmp u0x406B24
    }
}

// Author: Cups
// Date: 31/10/2017

unsigned u0x404E89 = 0x404E89;
unsigned u0x404FAE = 0x404FAE;
void __declspec(naked) naked_0x404E84()
{
    __asm
    {
        // original
        push eax
        mov ebx,ecx
        mov eax,esi

        // rsa key length
        cmp ecx,0x80
        jne _0x404FAE
        jmp u0x404E89

        _0x404FAE:
        jmp u0x404FAE
    }
}

// Estrae l'IPv4 da CUser* (SConnection::address.ipv4 a offset 0x70 da CUser*).
// SInternetAddress::ipv4 è in network byte order → b[0].b[1].b[2].b[3] è corretto.
static void cuser_get_ip(const void* cuser, char out[16])
{
    if (!cuser) { out[0] = '?'; out[1] = '\0'; return; }
    uint32_t raw = 0;
    std::memcpy(&raw, static_cast<const char*>(cuser) + 0x70, 4);
    const auto* b = reinterpret_cast<const uint8_t*>(&raw);
    std::snprintf(out, 16, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
}

// Verifica auth_token + game_hash + HWID e logga il motivo del fallimento con IP.
// Chiamato da naked_0x404548; cuser_ptr = EDI = CUser*.
// hwid_raw = puntatore a 32 byte ASCII non null-terminated nel buffer pacchetto (buf[143]).
// sig_raw  = puntatore a 64 byte ASCII non null-terminated nel buffer pacchetto (buf[176]).
// tamper_flags = buf[175]: bit0=DLL_INJECT, bit1=DEBUGGER, bit2=MEM_PATCH
static bool verify_a101_and_log(uint32_t auth_token, const uint8_t* game_hash,
                                 const void* cuser_ptr, const char* hwid_raw,
                                 uint8_t tamper_flags, const char* sig_raw,
                                 uint32_t pkt_len)
{
    char ip[16];
    cuser_get_ip(cuser_ptr, ip);

    // Bounds: il pacchetto 0xA101 esteso (sdev-client) è 240 byte — hwid_sig finisce a buf[239].
    // Length dichiarata < 240 = pacchetto legacy/forgiato → campi estesi non affidabili → reject (fail-closed).
    if (pkt_len < 240)
    {
        login_logger::log_fail(ip, "-", "A101_TRUNCATED (pacchetto troppo corto)");
        return false;
    }

    if (!gm_security::verify_rsa_client_token(auth_token))
    {
        login_logger::log_fail(ip, "-", "TOKEN_INVALID (client non autorizzato)");
        return false;
    }
    // verify_sah_token logga già il motivo (e sempre il valore ricevuto)
    if (!gm_security::verify_sah_token(game_hash, ip))
        return false;

    // Block-at-connection: HWID bannato (BlockedHWID) → disconnette QUI, al handshake,
    // prima della schermata di login. Copre il caso BlockHwidOnBan=1 (auto-ban tamper).
    // Fail-open se il DB è giù: il check autoritativo resta a login (verify_hwid).
    if (hwid_raw)
    {
        char hwid_chk[33] = {};
        std::memcpy(hwid_chk, hwid_raw, 32);
        if (security_logger::is_hwid_blocked(hwid_chk))
        {
            security_logger::log(ip, "-", "HWID_BLOCKED_HANDSHAKE", hwid_chk);
            return false;  // → _reject → FUN_00410190 (disconnect)
        }
    }

    // Salva HWID + sig in cache ip→{hwid,sig} — usato da hook_0x406A8C quando arriva lo username
    if (hwid_raw)
    {
        char hwid_str[33] = {};
        std::memcpy(hwid_str, hwid_raw, 32);
        char sig_str[65] = {};
        if (sig_raw)
            std::memcpy(sig_str, sig_raw, 64);
        hwid_cache::store(ip, hwid_str, sig_str);
    }

    // Log tamper_flags ricevuto dal client
    if (tamper_flags != 0)
    {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
            "flags=0x%02X%s%s%s%s%s",
            tamper_flags,
            (tamper_flags & 0x01) ? " DLL_INJECT"      : "",
            (tamper_flags & 0x02) ? " DEBUGGER"         : "",
            (tamper_flags & 0x04) ? " MEM_PATCH"        : "",
            (tamper_flags & 0x08) ? " CHEAT_PROC"       : "",
            (tamper_flags & 0x10) ? " HWID_SIG_ABSENT"  : "");
        security_logger::log(ip, "-", "TAMPER_DETECTED", detail);
    }

    return true;
}

// Hook at 0x404548 — 0xA101 (RSA handshake) handler.
// Struttura pacchetto 0xA101 estesa (lato client sdev-client):
//   buf[131..134] = auth_token   (4 byte) → [EBP+0x83]
//   buf[135..142] = sah_token    (8 byte) → [EBP+0x87]
//   buf[143..174] = hwid         (32 byte) → [EBP+0x8F]
//   buf[175]      = tamper_flags (1 byte)  → [EBP+0xAF]
//   buf[176..239] = hwid_sig     (64 byte) → [EBP+0xB0]
// Se token invalido → FUN_00410190(this,9,0) = disconnect.
// Displaced: ADD EBP,3 (3) + PUSH EBP (1) + MOVZX ECX,AL (3) = 7 bytes.
unsigned u0x40454f = 0x40454f;
unsigned u0x404555 = 0x404555;
void __declspec(naked) naked_0x404548()
{
    __asm
    {
        pushad

        // verify_a101_and_log(auth_token, game_hash, cuser_ptr, hwid_raw, tamper_flags, sig_raw, pkt_len)
        // cdecl: push last arg first
        movzx eax, word ptr [ebp + 0x0]    // pkt_len (7° arg) — u16 length @ buf[0]
        push eax
        lea  eax, [ebp + 0xB0]
        push eax                           // sig_raw (6° arg) — &buf[176], 64 byte
        movzx eax, byte ptr [ebp + 0xAF]
        push eax                           // tamper_flags (5° arg) — buf[175]
        lea  eax, [ebp + 0x8F]
        push eax                           // hwid_raw (4° arg) — &buf[143], 32 byte
        push edi                           // cuser_ptr (3° arg) — EDI = this (CUser*)
        lea  eax, [ebp + 0x87]
        push eax                           // game_hash (2° arg) — &buf[135]
        mov  eax, dword ptr [ebp + 0x83]
        push eax                           // auth_token (1° arg) — buf[131..134]
        call verify_a101_and_log
        add  esp, 28
        test al, al
        jz   _reject

        popad
        // Displaced: ADD EBP,3; PUSH EBP; MOVZX ECX,AL (AL=0x80 preserved by popad)
        add  ebp, 3
        push ebp
        movzx ecx, al
        jmp  u0x40454f          // → PUSH EBX; CALL FUN_00404e60

        _reject:
        popad
        push 0
        push 9
        mov  ecx, edi           // this (__thiscall)
        mov  eax, 0x00410190
        call eax                // FUN_00410190(this, 9, 0) — disconnect
        jmp  u0x404555          // exit (LeaveCriticalSection)
    }
}

// Hook at 0x4043BC — 0xA202 handler, after precondition checks.
// [EBP+3] = server IPv4 echoed by client (uint32_t, little-endian).
// Custom client sends (serverIP ^ token); hook XORs back to restore original IP
// before FUN_00407400 uses it for server-list lookup.
// Stock client sends raw IP → XOR corrupts it → lookup fails → natural reject.
// Displaced: LEA EDX,[ESP+0x14] + LEA ESI,[ESP+0x34] = 8 bytes.
unsigned u0x4043C4 = 0x4043C4;
void __declspec(naked) naked_0x4043BC()
{
    __asm
    {
        pushad

        call gm_security::get_client_auth_token   // EAX = token (no args, cdecl)
        xor  dword ptr [ebp + 3], eax             // un-XOR server IP in-place

        popad

        // displaced instructions (ESP restored by popad)
        lea edx, [esp + 0x14]
        lea esi, [esp + 0x34]
        jmp  u0x4043C4
    }
}

void Main()
{
    // GetUser (GM Security)
    util::detour((void*)0x406A8C, naked_0x406A8C, 7);
    // CUserCrypto::KeyInit
    util::detour((void*)0x404E84, naked_0x404E84, 5);
    // 0xA101 RSA handshake: verifica token client PRIMA del login
    util::detour((void*)0x404548, naked_0x404548, 7);
    // 0xA202 select-server: XOR token sul server IP
    util::detour((void*)0x4043BC, naked_0x4043BC, 8);
}
