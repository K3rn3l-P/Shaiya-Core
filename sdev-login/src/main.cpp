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
// Per-IP login brute force guard
// ===========================================================================
namespace login_guard
{
    static constexpr int   MAX_FAILS  = 5;
    static constexpr DWORD WINDOW_MS  = 600000; // 10 minuti sliding window

    struct Entry { int fails; DWORD first_fail; };
    static std::unordered_map<std::string, Entry> g_fails;
    static std::mutex g_mtx;

    static bool is_allowed(const char* ip)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_fails.find(ip);
        if (it == g_fails.end()) return true;
        auto& e = it->second;
        if (now - e.first_fail > WINDOW_MS) { g_fails.erase(it); return true; }
        return e.fails < MAX_FAILS;
    }

    static void on_fail(const char* ip)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_mtx);
        auto& e = g_fails[ip];
        if (e.fails == 0 || now - e.first_fail > WINDOW_MS) { e.first_fail = now; e.fails = 1; }
        else e.fails++;
    }

    static void on_success(const char* ip)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_fails.erase(ip);
    }
}

// ===========================================================================
// Login hook
// ===========================================================================

short hook_0x406A8C(SDatabase* db, char* username, char* password, unsigned lowPart, unsigned highPart, char* ipv4)
{
    if (!login_guard::is_allowed(ipv4))
    {
        login_logger::log_fail(ipv4, username, "BRUTE_FORCE_BLOCKED");
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
        login_logger::log_fail(ipv4, username, "PASSWORD_FAIL");
        return -1;
    }

    login_guard::on_success(ipv4);
    login_logger::log_ok(ipv4, username);

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

// Verifica auth_token + game_hash e logga il motivo del fallimento con IP.
// Chiamato da naked_0x404548; cuser_ptr = EDI = CUser*.
static bool verify_a101_and_log(uint32_t auth_token, const uint8_t* game_hash, const void* cuser_ptr)
{
    char ip[16];
    cuser_get_ip(cuser_ptr, ip);

    if (!gm_security::verify_rsa_client_token(auth_token))
    {
        login_logger::log_fail(ip, "-", "TOKEN_INVALID (client non autorizzato)");
        return false;
    }
    if (!gm_security::verify_sah_token(game_hash))
    {
        login_logger::log_fail(ip, "-", "GAME_HASH_INVALID (game.exe modificato)");
        return false;
    }
    return true;
}

// Hook at 0x404548 — 0xA101 (RSA handshake) handler.
// Punto: dopo il check key_size==0x80, prima di processare la chiave RSA.
// At entry: EBP = iVar2+2, AL = 0x80, EDI = this (CUser*).
// Client invia auth_token[4] a [EBP+0x83] (iVar2+133) + sah_token[8] a [EBP+0x87] (iVar2+137).
// Se token invalido → FUN_00410190(this,9,0) = disconnect prima del login.
// Displaced: ADD EBP,3 (3) + PUSH EBP (1) + MOVZX ECX,AL (3) = 7 bytes.
unsigned u0x40454f = 0x40454f;
unsigned u0x404555 = 0x404555;
void __declspec(naked) naked_0x404548()
{
    __asm
    {
        pushad

        // verify_a101_and_log(uint32_t auth_token, const uint8_t* game_hash, const void* cuser_ptr)
        // cdecl: push last arg first
        push edi                           // cuser_ptr (3° arg) — EDI = this (CUser*)
        lea  eax, [ebp + 0x87]
        push eax                           // game_hash (2° arg) — &buf[135]
        mov  eax, dword ptr [ebp + 0x83]
        push eax                           // auth_token (1° arg) — buf[131..134]
        call verify_a101_and_log
        add  esp, 12
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
