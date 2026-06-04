#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <util/util.h>
#include "include/main.h"
#include "include/game_data_archive.h"

#pragma comment(lib, "bcrypt.lib")

// ---------------------------------------------------------------------------
// ESAH format (matches SahCrypt.cs):
//   [0-3]   "ESAH"
//   [4-5]   version u16
//   [6-7]   flags u16 (1 = AES-256-GCM)
//   [8-15]  plainSize u64
//   [16-27] nonce 12 bytes
//   [28-43] GCM auth tag 16 bytes
//   [44-63] padding
//   [64..]  ciphertext
// ---------------------------------------------------------------------------
namespace
{
    struct EsahHeader
    {
        char     magic[4];
        uint16_t version;
        uint16_t flags;
        uint64_t plainSize;
        uint8_t  nonce[12];
        uint8_t  tag[16];
        uint8_t  pad[20];
    };
    static_assert(sizeof(EsahHeader) == 64, "EsahHeader size mismatch");

    static bool parse_hex_key(const char* hex, uint8_t out[32])
    {
        if (!hex || strlen(hex) != 64) return false;
        for (int i = 0; i < 32; ++i)
        {
            char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
            out[i] = static_cast<uint8_t>(strtoul(b, nullptr, 16));
        }
        return true;
    }

    static std::vector<char> decrypt_esah_bcrypt(const std::vector<char>& esah, const uint8_t key[32])
    {
        if (esah.size() < sizeof(EsahHeader)) return {};
        const auto* hdr = reinterpret_cast<const EsahHeader*>(esah.data());
        if (memcmp(hdr->magic, "ESAH", 4) != 0) return {};

        const uint8_t* cipher    = reinterpret_cast<const uint8_t*>(esah.data()) + sizeof(EsahHeader);
        ULONG          cipherLen = static_cast<ULONG>(esah.size() - sizeof(EsahHeader));

        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_KEY_HANDLE hKey = nullptr;
        std::vector<char> plain(static_cast<size_t>(hdr->plainSize));

        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            return {};

        BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0);

        BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
            const_cast<PUCHAR>(key), 32, 0);

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(hdr->nonce);
        info.cbNonce = 12;
        info.pbTag   = const_cast<PUCHAR>(hdr->tag);
        info.cbTag   = 16;

        ULONG cbResult = 0;
        NTSTATUS st = BCryptDecrypt(hKey,
            const_cast<PUCHAR>(cipher), cipherLen, &info,
            nullptr, 0,
            reinterpret_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()),
            &cbResult, 0);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);

        return BCRYPT_SUCCESS(st) ? plain : std::vector<char>{};
    }
}

// ---------------------------------------------------------------------------
// Diagnostics — compiled only in Debug builds
// ---------------------------------------------------------------------------
#ifdef _DEBUG
static void diag_log(const char* msg, DWORD extra = 0)
{
    auto path = game_data::relative_path("_sdev_log.txt");
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, nullptr, FILE_END);
    char buf[256];
    int len = wsprintfA(buf, "[%lu] %s: %lu\r\n", GetTickCount(), msg, extra);
    DWORD w = 0;
    WriteFile(h, buf, static_cast<DWORD>(len), &w, nullptr);
    CloseHandle(h);
}
#else
static void diag_log(const char*, DWORD = 0) {}
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
std::vector<char> g_sah_plain;           // decrypted SAH in RAM, used by scan_sah_files

static uint8_t g_sah_key[32]   = {};
static bool    g_sah_key_ready  = false;

// fd returned by game.exe's own _open() for the temp SAH file.
// Lives in game.exe's static CRT fd table — game's __read/__close work on it.
static int  g_datasah_fd        = -1;

// Path of the temp file in %TEMP% — stored for background deletion thread.
static char g_sah_temp_path[MAX_PATH] = {};

// ---------------------------------------------------------------------------
// Background thread: delete temp SAH file once game closes its read handle.
// DeleteFileA fails with ERROR_SHARING_VIOLATION while game's fd is open;
// succeeds as soon as FUN_0040e270 calls __close(fd) after VFS is built.
// ---------------------------------------------------------------------------
static DWORD WINAPI sah_temp_cleanup_thread(LPVOID)
{
    for (int i = 0; i < 120; ++i)   // up to 60s
    {
        Sleep(500);
        if (DeleteFileA(g_sah_temp_path)) break;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) break;
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Hook at CALL _open (0x0040e43d, 5 bytes E8 rel32) inside FUN_0040e270.
//
// game.exe pushes 3 args (path, oflag, pmode) before the CALL.
// We replaced the CALL with JMP so no return address was pushed.
// Stack on entry: [ESP+0]=path, [ESP+4]=oflag, [ESP+8]=pmode
//
// Strategy: return g_datasah_fd in EAX (already open, in game's CRT table),
// then jump to 0x0040e442 where the caller's ADD ESP,0xC cleans the 3 args.
// ---------------------------------------------------------------------------
static unsigned g_cont_0040e442 = 0x0040e442u;

static void __declspec(naked) hook_open_datasah()
{
    __asm
    {
        mov  eax, g_datasah_fd    // return pre-opened fd (in game's CRT table)
        push g_cont_0040e442      // continuation: 0x0040e442 (after original CALL)
        ret                       // jump there; caller's ADD ESP,0xC cleans 3 args
    }
}

// ---------------------------------------------------------------------------
// do_sah_decrypt_work
// Called from hook_FUN_0040e270 — OUTSIDE DllMain loader lock.
// Safe: CRT file I/O, BCrypt (loads bcrypt.dll), std::vector, CreateThread.
//
// Flow:
//   1. Read data.sah (ESAH) → decrypt → g_sah_plain
//   2. Write plaintext to %TEMP%\sah<random>.tmp  (not in game directory!)
//   3. Call game.exe's own _open(tempPath) → g_datasah_fd in game's CRT table
//   4. Install hook at 0x40e43d: intercepts _open("data.sah"), returns g_datasah_fd
//   5. Launch cleanup thread: deletes temp file once game closes the fd
//   6. No string patch needed — hook intercepts the call entirely
// ---------------------------------------------------------------------------
static void do_sah_decrypt_work()
{
    if (!g_sah_key_ready) return;

    diag_log("START");

    // 1. Read data.sah
    std::ifstream f(game_data::relative_path("data.sah"), std::ios::binary);
    if (!f) { SecureZeroMemory(g_sah_key, 32); g_sah_key_ready = false; return; }
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
    f.close();
    diag_log("READ_DATASAH", static_cast<DWORD>(raw.size()));

    if (raw.size() < 4 || memcmp(raw.data(), "ESAH", 4) != 0)
    {
        SecureZeroMemory(g_sah_key, 32);
        g_sah_key_ready = false;
        diag_log("NOT_ESAH");
        return;
    }

    g_sah_plain = decrypt_esah_bcrypt(raw, g_sah_key);
    SecureZeroMemory(g_sah_key, 32);
    g_sah_key_ready = false;
    diag_log("DECRYPT_DONE", static_cast<DWORD>(g_sah_plain.size()));

    if (g_sah_plain.empty()) { diag_log("ERR_DECRYPT_EMPTY"); return; }

    // 2. Write to %TEMP%\sah<random>.tmp  (not in game directory)
    char tempDir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tempDir);
    if (!GetTempFileNameA(tempDir, "~df", 0, g_sah_temp_path))
    {
        diag_log("ERR_GETTEMPFILE", GetLastError());
        return;
    }

    // GetTempFileNameA creates an empty file — open it for writing
    HANDLE hWrite = CreateFileA(
        g_sah_temp_path,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hWrite == INVALID_HANDLE_VALUE)
    {
        diag_log("ERR_CREATE_TEMP", GetLastError());
        DeleteFileA(g_sah_temp_path);
        return;
    }

    const BYTE* ptr = reinterpret_cast<const BYTE*>(g_sah_plain.data());
    DWORD rem = static_cast<DWORD>(g_sah_plain.size());
    while (rem > 0)
    {
        DWORD written = 0;
        DWORD chunk = (rem < 65536u) ? rem : 65536u;
        if (!WriteFile(hWrite, ptr, chunk, &written, nullptr) || written == 0)
        {
            diag_log("ERR_WRITEFILE", GetLastError());
            CloseHandle(hWrite);
            DeleteFileA(g_sah_temp_path);
            return;
        }
        ptr += written;
        rem -= written;
    }
    CloseHandle(hWrite);
    diag_log("WRITE_DONE");

    // 3. Call game.exe's own _open(tempPath, _O_RDONLY|_O_BINARY, 0)
    //    Target address computed from the CALL rel32 at 0x0040e43d:
    //      E8 xx xx xx xx → target = 0x0040e442 + (int32_t)rel32
    //    Using game.exe's _open puts the fd in game's static CRT table so that
    //    game's __read and __close operate on it correctly.
    const int32_t rel = *reinterpret_cast<const int32_t*>(0x0040e43eu);
    typedef int (__cdecl *pfn_open_t)(const char*, int, int);
    auto game_open = reinterpret_cast<pfn_open_t>(
        0x0040e442u + static_cast<uint32_t>(static_cast<int32_t>(rel)));

    g_datasah_fd = game_open(g_sah_temp_path, 0x8000 /*_O_BINARY|_O_RDONLY*/, 0);
    diag_log("GAME_OPEN_FD", static_cast<DWORD>(g_datasah_fd));

    if (g_datasah_fd < 0)
    {
        diag_log("ERR_GAME_OPEN");
        DeleteFileA(g_sah_temp_path);
        return;
    }

    // 4. Hook CALL at 0x0040e43d (5 bytes E8→E9): intercept _open("data.sah")
    //    hook_open_datasah returns g_datasah_fd and jumps to 0x0040e442.
    util::detour(reinterpret_cast<void*>(0x0040e43du), hook_open_datasah, 5);
    diag_log("HOOK_43D_OK");

    // 5. Launch cleanup thread: deletes temp file once game closes its fd
    HANDLE hThread = CreateThread(nullptr, 0, sah_temp_cleanup_thread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);

    diag_log("ALL_DONE");
}

// ---------------------------------------------------------------------------
// Hook at FUN_0040e270 entry (0x0040e270)
//
// util::detour(0x0040e270, hook_FUN_0040e270, 7) replaces 7 bytes:
//   Original: PUSH -1 (6A FF, 2 bytes) + PUSH 0x72c651 (68 51 C6 72 00, 5 bytes)
//   Replaced: E9 XX XX XX XX 90 90 (5-byte JMP + 2 NOPs)
//   Continue: 0x0040e277
//
// Runs do_sah_decrypt_work (heavy work deferred here from DllMain), then
// re-executes displaced bytes and continues original function body.
// ---------------------------------------------------------------------------
static unsigned g_cont_0040e277 = 0x0040e277u;

static void __declspec(naked) hook_FUN_0040e270()
{
    __asm
    {
        pushad
        call do_sah_decrypt_work
        popad
        push -1                    // displaced: 6A FF  (PUSH -1)
        push 0x0072c651            // displaced: 68 51 C6 72 00  (PUSH 0x72c651)
        jmp  g_cont_0040e277       // indirect JMP to 0x0040e277
    }
}

// ---------------------------------------------------------------------------
// hook::sah_decrypt_init  (called from Main() — DllMain DLL_PROCESS_ATTACH)
// CRITICAL: Win32 API only — no CRT I/O, no BCrypt, no std::vector.
// ---------------------------------------------------------------------------
void hook::sah_decrypt_init()
{
    char keyHex[65] = {};
    if (!GetEnvironmentVariableA("SDEV_SAH_KEY", keyHex, sizeof(keyHex))) return;
    SetEnvironmentVariableA("SDEV_SAH_KEY", nullptr);

    bool ok = parse_hex_key(keyHex, g_sah_key);
    SecureZeroMemory(keyHex, sizeof(keyHex));
    if (!ok) return;

    g_sah_key_ready = true;
    // size=7: displaces PUSH -1 (2 bytes) + PUSH 0x72c651 (5 bytes)
    util::detour(reinterpret_cast<void*>(0x0040e270u), hook_FUN_0040e270, 7);
}

// ---------------------------------------------------------------------------
// hook::sah_decrypt_teardown  (called from DllMain DLL_PROCESS_DETACH)
// ---------------------------------------------------------------------------
void hook::sah_decrypt_teardown()
{
    // Safety: delete temp file if cleanup thread hasn't done it yet
    if (g_sah_temp_path[0])
        DeleteFileA(g_sah_temp_path);

    if (!g_sah_plain.empty())
    {
        SecureZeroMemory(g_sah_plain.data(), g_sah_plain.size());
        g_sah_plain.clear();
    }
    SecureZeroMemory(g_sah_key, 32);
    g_sah_key_ready = false;
    g_datasah_fd    = -1;
}
