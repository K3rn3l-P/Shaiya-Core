#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <util/util.h>
#include "include/main.h"

// ===========================================================================
//  ConnectionGuard
//
//  GAP 1  Per-IP connection rate limit
//  Hook: FUN_004ec170 (SIPFilter::Allow) at 0x004ec170  __thiscall
//  Sliding window: MAX_CONN connections per IP within CONN_WIN_MS.
//  No disconnect tracking needed — old entries expire automatically.
//  Comes before native whitelist/blacklist logic.
//
//  GAP 3  Per-connection packet rate limit
//  Hook: FUN_004f1620 (packet dispatch entry) at 0x004f1620  __cdecl
//  Drop silently if connection sends > MAX_PKT packets in PKT_WIN_MS.
//  Caller (FUN_004f20b0) re-posts WSARecv after the call regardless.
//  Dropped buffer freed via FUN_004f33a0 (same path as FUN_004ef120 early-exit).
// ===========================================================================

// ---------------------------------------------------------------------------
//  GAP 1: per-IP connection rate limit
// ---------------------------------------------------------------------------
namespace ip_guard
{
    static constexpr int   MAX_CONN    = 5;      // connections per IP per window
    static constexpr DWORD CONN_WIN_MS = 60000;  // 60-second sliding window

    static std::unordered_map<unsigned, std::deque<DWORD>> g_times;
    static std::mutex g_mtx;

    // Returns 1 (allow) or 0 (block). Called before native IP filter.
    int __cdecl check(unsigned ip)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_mtx);
        auto& q = g_times[ip];
        while (!q.empty() && (now - q.front() > CONN_WIN_MS))
            q.pop_front();
        if (static_cast<int>(q.size()) >= MAX_CONN)
            return 0;
        q.push_back(now);
        return 1;
    }
}

// ---------------------------------------------------------------------------
//  GAP 3: per-connection packet rate limit
// ---------------------------------------------------------------------------
namespace pkt_guard
{
    static constexpr int   MAX_PKT    = 300;  // packets per connection per second
    static constexpr DWORD PKT_WIN_MS = 1000; // 1-second window

    struct Rate { DWORD start; int count; };

    static std::unordered_map<int, Rate> g_rates;
    static std::mutex g_mtx;

    // Returns 1 (process) or 0 (drop). conn = param_1[0x32] in FUN_004f1620.
    int __cdecl check(int conn)
    {
        auto now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_mtx);
        auto& r = g_rates[conn];
        if (now - r.start >= PKT_WIN_MS) { r.start = now; r.count = 0; }
        return (++r.count <= MAX_PKT) ? 1 : 0;
    }
}

// C++ helper — __cdecl, called from naked_0x4f1620.
// Returns 1 = process, 0 = buffer freed + drop signal.
static int __cdecl pkt_dispatch_check(int* param_1, int param_2)
{
    if (!param_1) return 1;
    int conn = param_1[0x32]; // *(int*)(param_1 + 200) — connection object
    if (!conn) return 1;

    if (pkt_guard::check(conn))
        return 1;

    // Rate limit exceeded: free buffer (mirrors FUN_004ef120 early-exit path).
    // FUN_004f33a0 = SContext free/recycle (confirmed from SWorker::Run + FUN_004f1620).
    typedef void(__cdecl* FnFree)(int);
    static auto fn = reinterpret_cast<FnFree>(0x004f33a0);
    fn(param_2);
    return 0;
}

// ===========================================================================
//  Naked assembly detours
// ===========================================================================

// --- GAP 1: FUN_004ec170 ---
// __thiscall: ECX=this (SIPFilter*), [ESP+4]=IP (unsigned)
// Displaced 8 bytes (verified disassemble_at 0x4ec170):
//   51          PUSH ECX
//   8B 44 24 08 MOV EAX,[ESP+8]
//   56          PUSH ESI
//   8B F1       MOV ESI,ECX
unsigned u0x4ec178 = 0x4ec178;
void __declspec(naked) naked_0x4ec170()
{
    __asm
    {
        // __thiscall: ECX=this, [ESP]=ret, [ESP+4]=IP
        pushad                   // ESP-=32; [ESP+32]=ret, [ESP+36]=IP
        mov  eax, [esp + 36]     // IP
        push eax
        call ip_guard::check     // __cdecl: returns 1(allow)/0(block)
        add  esp, 4
        test eax, eax
        popad                    // ECX=this restored
        jnz  _allow

        // Blocked: __thiscall callee cleans 1 DWORD arg
        xor  eax, eax
        retn 4

    _allow:
        // Displaced (8 bytes): PUSH ECX / MOV EAX,[ESP+8] / PUSH ESI / MOV ESI,ECX
        push ecx
        mov  eax, [esp + 8]
        push esi
        mov  esi, ecx
        jmp  u0x4ec178
    }
}

// --- GAP 3: FUN_004f1620 ---
// __cdecl: [ESP+4]=param_1 (int*), [ESP+8]=param_2 (int)
// Displaced 5 bytes (verified disassemble_at 0x4f1620):
//   56          PUSH ESI
//   8B 74 24 08 MOV ESI,[ESP+8]
unsigned u0x4f1625 = 0x4f1625;
void __declspec(naked) naked_0x4f1620()
{
    __asm
    {
        // [ESP]=ret, [ESP+4]=param_1, [ESP+8]=param_2
        pushad                        // ESP-=32; [ESP+36]=param_1, [ESP+40]=param_2
        mov  edx, [esp + 40]          // param_2
        mov  eax, [esp + 36]          // param_1
        push edx
        push eax
        call pkt_dispatch_check       // __cdecl: 1=ok, 0=drop+freed
        add  esp, 8
        test eax, eax
        popad
        jz   _drop

        // Proceed: displaced (5 bytes): PUSH ESI / MOV ESI,[ESP+8]
        // After popad stack: [ESP]=ret, [ESP+4]=param_1, [ESP+8]=param_2
        // After PUSH ESI:    [ESP]=ESI, [ESP+4]=ret, [ESP+8]=param_1, [ESP+0xC]=param_2
        push esi
        mov  esi, [esp + 8]           // param_1
        jmp  u0x4f1625

    _drop:
        // Buffer freed by pkt_dispatch_check. Caller (FUN_004f20b0) re-posts recv.
        retn                          // __cdecl: caller cleans args
    }
}

// ===========================================================================
//  hook::connection_guard() — called once from Main()
// ===========================================================================
void hook::connection_guard()
{
    // GAP 1: per-IP connection rate limit at accept
    util::detour((void*)0x4ec170, naked_0x4ec170, 8);
    // GAP 3: per-connection packet rate limit at dispatch
    util::detour((void*)0x4f1620, naked_0x4f1620, 5);
}
