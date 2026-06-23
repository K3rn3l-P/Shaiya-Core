#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include "include/main.h"
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------------------
// Anti-Tamper — sdev-client
//
// FILOSOFIA: LOG FIRST, KICK LATER.
// Con 500 utenti su configurazioni diverse (AV, IME, codec GPU, overlay Steam
// ecc.) un kick immediato genera falsi positivi. Ogni check scrive un log
// locale e invia una notifica al server. L'enforcement si abilita dopo 4
// settimane di raccolta dati, modificando GMSecurity.ini lato server.
//
// CHECK ATTIVI:
//   3A — DLL injection: EnumProcessModules + whitelist
//   3B — Anti-debug: IsDebuggerPresent / CheckRemoteDebugger / NtQIP / heap
//   3C — Memory CRC .text: baseline al boot, controllo ogni 60s
//
// CHECK DISABILITATI (log-only per ora):
//   Tutti. Vedere il commento "ENFORCEMENT" nei punti rilevanti.
//   Per abilitare kick: impostare g_kick_on_tamper = true DOPO aver analizzato
//   i SecurityEvents raccolti nelle prime 4 settimane.
//
// LOG: C:\Users\<user>\AppData\Local\Shaiya_Duff_Updater\anti_tamper.log
//      (stesso dir del device.id, accessibile senza privilegi)
// ---------------------------------------------------------------------------

namespace anti_tamper
{
    std::atomic<uint8_t> g_tamper_flags{ 0 };

    // -----------------------------------------------------------------------
    // HTTP report — WinHTTP POST a /sec.php, OOB rispetto al socket di gioco.
    // Il socket del gioco NON è usabile quando CE è aperto: CE congela i thread
    // e corrompe il RC4 state → pacchetto garbage → crash del server.
    // -----------------------------------------------------------------------
    namespace
    {
        static constexpr uintptr_t kCharNameAddr = 0x09144CE;
        // URL dell'API updater — usa HTTPS, autenticazione via secToken DB-backed
        static constexpr LPCWSTR kReportHost = L"duff.pinto-lime.ts.net";
        static constexpr INTERNET_PORT kReportPort = INTERNET_DEFAULT_HTTPS_PORT;
        static constexpr LPCWSTR kReportPath = L"/Updater/api/security_report.php";

        static void http_security_report(uint8_t flags, std::string detail)
        {
            const char* token = sec_token::get();  // già garantito non vuoto da hook::anti_tamper

            char body[512];
            int blen = std::snprintf(body, sizeof(body),
                "{\"secToken\":\"%s\",\"flags\":\"%02X\",\"detail\":\"%s\"}",
                token, (unsigned)flags, detail.c_str());
            if (blen <= 0) return;

            HINTERNET hSess = WinHttpOpen(L"SC/1.0",
                WINHTTP_ACCESS_TYPE_NO_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!hSess) return;

            DWORD to = 3000;
            WinHttpSetOption(hSess, WINHTTP_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
            WinHttpSetOption(hSess, WINHTTP_OPTION_SEND_TIMEOUT,    &to, sizeof(to));
            WinHttpSetOption(hSess, WINHTTP_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));

            HINTERNET hConn = WinHttpConnect(hSess, kReportHost, kReportPort, 0);
            if (hConn)
            {
                HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", kReportPath,
                    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                    WINHTTP_FLAG_SECURE);
                if (hReq)
                {
                    static const wchar_t kCT[] =
                        L"Content-Type: application/json";
                    if (WinHttpSendRequest(hReq, kCT, (DWORD)-1L,
                            (LPVOID)body, (DWORD)blen, (DWORD)blen, 0))
                        WinHttpReceiveResponse(hReq, nullptr);
                    WinHttpCloseHandle(hReq);
                }
                WinHttpCloseHandle(hConn);
            }
            WinHttpCloseHandle(hSess);
        }

        static void send_tamper_report(uint8_t flags, std::string detail)
        {
            std::thread(http_security_report, flags, std::move(detail)).detach();
        }
    }

    // -----------------------------------------------------------------------
    // Log
    // -----------------------------------------------------------------------
    namespace
    {
        static char g_log_path[MAX_PATH] = {};
        static bool g_log_path_ready = false;

        static void init_log_path()
        {
            if (g_log_path_ready) return;
            g_log_path_ready = true;
            char base[MAX_PATH] = {};
            GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
            std::snprintf(g_log_path, sizeof(g_log_path),
                          "%s\\Shaiya_Duff_Updater\\anti_tamper.log", base);
        }

        static void log_event(const char* event_type, const char* detail)
        {
            init_log_path();
            if (!g_log_path[0]) return;

            SYSTEMTIME st;
            GetLocalTime(&st);
            char line[512];
            int len = std::snprintf(line, sizeof(line),
                "[%04d-%02d-%02d %02d:%02d:%02d] %-20s | %s\r\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond,
                event_type, detail ? detail : "");
            if (len <= 0) return;

            HANDLE hFile = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return;
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    // -----------------------------------------------------------------------
    // 3A — DLL Whitelist
    //
    // PROCEDURA WHITELIST:
    //   1. Avvia anti_tamper in log-only per 4 settimane
    //   2. Raccogli tutti i moduli loggati su macchine pulite
    //   3. Aggiungi i moduli legittimi nella lista g_whitelist_prefixes
    //   4. Solo dopo enable enforcement
    //
    // I prefissi coprono le varianti versione (es. d3d9on12.dll e d3d9.dll).
    // -----------------------------------------------------------------------
    namespace
    {
        static const char* const g_whitelist_prefixes[] = {
            // Eseguibile principale
            "game",
            // Moduli di sistema Windows fondamentali
            "ntdll", "kernel32", "kernelbase", "user32", "gdi32", "ole32",
            "oleaut32", "shell32", "advapi32", "crypt32", "cryptsp", "cryptbase",
            "ws2_32", "wsock32", "mswsock", "dnsapi", "iphlpapi", "sechost",
            "rpcrt4", "combase", "comdlg32", "shlwapi", "msvcrt",
            "vcruntime140", "msvcp140", "ucrtbase", "api-ms-win",
            // Windows Shell / UI
            "apphelp", "shcore", "win32u", "comctl32", "version",
            "uxtheme", "dwmapi", "clbcatq", "wintrust", "cfgmgr32", "devobj",
            "wintypes", "coreuicomponents",
            "textinputframework", "inputhost", "coremessaging", "msctf",
            "windows.ui", "windows.storage", "windows.staterepositorycore",
            "kernel.appcore", "resourcepolicyclient",
            // DirectX / grafica
            "d3d9", "d3d9on12", "d3d11", "d3d12", "dxgi", "dxgidebug",
            "d3dcompiler", "d3dx9", "d3dx10", "d3dx11", "d3dxof",
            "opengl32", "glu32", "nvoglv32", "nvd3dum", "atioglxx",
            "igdumdim32", "ig4icd32", "vulkan-1",
            "ddraw", "dciman32", "dxcore", "directxdatabasehelper",
            "gdiplus", "mscms",
            // GPU AMD
            "aticfx32", "atiu9pag", "atiumdag", "atiumdva", "amdihk32",
            // GPU NVIDIA
            "nvcontainer", "nvshell", "nvspcap",
            // Audio
            "dsound", "winmm", "xaudio2", "audioses",
            "mmdevapi", "avrt", "msdmo", "resampledmo", "mfplat", "rtworkq",
            // Input / HID
            "dinput8", "xinput1_4", "xinput1_3", "hid", "setupapi",
            // Runtime / CRT
            "msvcp", "concrt", "vcomp",
            // Rete / TLS / Crypto
            "wininet", "winhttp", "urlmon", "iertutil", "ondemandconnroutehelper",
            "nsi", "winnsi", "rasadhlp", "fwpuclnt",
            "schannel", "ncrypt", "ntasn1", "ncryptsslp", "cryptnet",
            "rsaenh", "dpapi", "srvcli", "netutils", "sspicli",
            "bcrypt", "bcryptprimitives", "msasn1", "usp10", "textshaping",
            "wbemprox", "wbemcomn", "wbemsvc", "fastprox",
            // Gestione risorse / profilo
            "userenv", "profapi", "powrprof", "umpdc",
            // Windows Defender / AMSI
            "amsi", "mpoav",
            // DirectShow legacy (ijl15 = Intel JPEG Library)
            "ijl15",
            // Debug helpers (caricati da driver GPU AMD e altri)
            "dbghelp", "dbgcore",
            // Overlay legittimi (Steam, Discord)
            "gameoverlayrenderer", "discordhook", "reshade",
            // Windows Imaging Component (WIC) — caricato da DirectX/grafica
            "windowscodecs",
            // IME (input method editor — molto comune in Asia)
            "imm32", "msimtf", "imjp", "mspy",
            // Nostra DLL (sia nome build che nome deploy)
            "sdev-client", "sdev",
            // Moduli comuni antivirus/EDR
            "avcuf", "avghooka", "mbam", "mbae", "mbamc",
        };

        static bool is_whitelisted(const char* modname_lower)
        {
            for (auto* prefix : g_whitelist_prefixes)
            {
                if (std::strncmp(modname_lower, prefix, std::strlen(prefix)) == 0)
                    return true;
            }
            return false;
        }

        static void check_injected_dlls()
        {
            HMODULE mods[1024] = {};
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
                return;

            DWORD count = needed / sizeof(HMODULE);
            for (DWORD i = 0; i < count; ++i)
            {
                char name[MAX_PATH] = {};
                if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name)))
                    continue;

                // Normalizza lowercase per confronto
                char lower[MAX_PATH] = {};
                for (int j = 0; name[j] && j < MAX_PATH - 1; ++j)
                    lower[j] = static_cast<char>(tolower(static_cast<unsigned char>(name[j])));

                // Rimuovi estensione .dll per il confronto prefisso
                char* dot = std::strrchr(lower, '.');
                size_t namelen = dot ? static_cast<size_t>(dot - lower) : std::strlen(lower);
                char noext[MAX_PATH] = {};
                std::memcpy(noext, lower, namelen);

                if (!is_whitelisted(noext))
                {
                    g_tamper_flags.fetch_or(0x01);
                    log_event("DLL_INJECT", name);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // 3B — Anti-Debug
    //
    // Rileva: IsDebuggerPresent, CheckRemoteDebuggerPresent,
    //         NtQueryInformationProcess(ProcessDebugPort=7),
    //         NtGlobalFlag heap debug flags.
    //
    // [LOG ONLY] — mese 1. Per enforcement: impostare g_kick_on_debug = true
    //   DOPO aver verificato che nessun utente legittimo usa dev tools correlati.
    // -----------------------------------------------------------------------
    namespace
    {
        typedef LONG(NTAPI* NtQueryInformationProcess_t)(
            HANDLE, UINT, PVOID, ULONG, PULONG);

        // MSVC non permette __asm e __try/__except nella stessa funzione.
        // CloseHandle trick separato per questo motivo.
        static bool check_close_handle_trick()
        {
            // Con debugger (OllyDbg/x64dbg): CloseHandle su handle non valido
            // genera EXCEPTION_INVALID_HANDLE — il debugger la intercetta prima di noi.
            // Il nostro __except non viene mai chiamato → debugger presente.
            // Senza debugger: CloseHandle ritorna FALSE, nessuna eccezione.
            //
            // CRITICO: usare EXCEPTION_EXECUTE_HANDLER (non CONTINUE_SEARCH).
            // CONTINUE_SEARCH per codici != STATUS_INVALID_HANDLE propagava l'eccezione
            // al receive thread di game.exe che non ha handler → crash processo.
            bool caught = false;
            __try
            {
                CloseHandle(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0xDEADBEEFu)));
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                caught = (GetExceptionCode() == 0xC0000008L);  // STATUS_INVALID_HANDLE
            }
            return caught;
        }

        // Ritorna stringa del check che ha triggerato, o nullptr se pulito.
        static const char* which_debugger_check()
        {
            // Check 1
            if (IsDebuggerPresent()) return "IS_DEBUGGER_PRESENT";

            // Check 2
            BOOL remote = FALSE;
            CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
            if (remote) return "REMOTE_DEBUGGER";

            // Check 3: NtQueryInformationProcess ProcessDebugPort (7)
            auto NtQIP = reinterpret_cast<NtQueryInformationProcess_t>(
                GetProcAddress(GetModuleHandleA("ntdll.dll"),
                               "NtQueryInformationProcess"));
            if (NtQIP)
            {
                DWORD_PTR port = 0;
                NtQIP(GetCurrentProcess(), 7, &port, sizeof(port), nullptr);
                if (port) return "NTQIP_DEBUG_PORT";
            }

            // Check 4: NtGlobalFlag PEB+0x68 (flags 0x70 = heap debug)
            // ScyllaHide patcha questo, ma non sempre su versioni vecchie.
            {
                auto* peb  = reinterpret_cast<BYTE*>(__readfsdword(0x30));
                DWORD ntGlobal = *reinterpret_cast<DWORD*>(peb + 0x68);
                if (ntGlobal & 0x70) return "NT_GLOBAL_FLAG";
            }

            // Check 5 RIMOSSO: HeapForceFlags (PEB+0x18 → heap+0x10) dava
            // falsi positivi su Windows 10/11 con MSVC runtime /MT.
            // Il runtime MSVC con /MT puo' impostare heap flags legittimi.

            // Check 6: CloseHandle trick (in funzione separata per SEH)
            if (check_close_handle_trick()) return "CLOSE_HANDLE_TRICK";

            return nullptr;
        }

        static bool is_debugged()
        {
            return which_debugger_check() != nullptr;
        }

        static void check_debugger()
        {
            const char* which = which_debugger_check();
            if (which)
            {
                g_tamper_flags.fetch_or(0x02);
                log_event("DEBUGGER", which);
                send_tamper_report(g_tamper_flags.load(), std::string(which));
            }
        }
    }

    // -----------------------------------------------------------------------
    // 3C — Memory CRC sezione .text
    //
    // Calcola CRC32 della sezione .text al boot (baseline) e poi ogni 60s.
    // Se il CRC cambia → Cheat Engine / patcher ha modificato il codice.
    //
    // [LOG ONLY] — nessun kick nel mese 1. Falsi positivi rari su .text stabile.
    // -----------------------------------------------------------------------
    namespace
    {
        static uint32_t g_text_crc_baseline = 0;
        static bool     g_baseline_computed  = false;

        static uint32_t crc32_impl(const uint8_t* data, size_t len)
        {
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < len; ++i)
            {
                crc ^= data[i];
                for (int j = 0; j < 8; ++j)
                    crc = (crc >> 1) ^ (0xEDB88320u & ~((crc & 1u) - 1u));
            }
            return crc ^ 0xFFFFFFFFu;
        }

        static uint32_t compute_text_crc()
        {
            auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
            if (!base) return 0;

            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
            {
                if (std::memcmp(sec->Name, ".text", 5) == 0)
                {
                    auto* text = base + sec->VirtualAddress;
                    DWORD  size = sec->Misc.VirtualSize;
                    return crc32_impl(text, size);
                }
            }
            return 0;
        }

        static void check_text_crc()
        {
            uint32_t crc = compute_text_crc();
            if (crc == 0) return;

            if (!g_baseline_computed)
            {
                g_text_crc_baseline = crc;
                g_baseline_computed = true;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "baseline_crc=%08X", crc);
                log_event("TEXT_CRC_INIT", buf);
                return;
            }

            if (crc != g_text_crc_baseline)
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                    "expected=%08X got=%08X", g_text_crc_baseline, crc);
                g_tamper_flags.fetch_or(0x04);
                log_event("MEM_PATCH", buf);
            }
        }
    }

    // -----------------------------------------------------------------------
    // In-game state detection
    // -----------------------------------------------------------------------
    static bool is_in_game()
    {
        return *reinterpret_cast<const int*>(0x7C48F8) == 6;
    }

    // -----------------------------------------------------------------------
    // 3D — Process scan: rileva tool cheat noti in esecuzione
    //
    // Eseguito solo in-game, ogni 60s nel monitor_thread.
    // Log-only: setta bit 0x08 nel tamper_flags (inviato al server al prossimo login).
    // -----------------------------------------------------------------------
    namespace
    {
        static const char* const kBannedProcPrefixes[] = {
            "cheatengine", "x64dbg", "x32dbg", "ollydbg",
            "processhacker", "artmoney", "tsearch",
            nullptr
        };

        static void check_cheat_processes()
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return;

            PROCESSENTRY32 pe{};
            pe.dwSize = sizeof(pe);

            if (Process32First(snap, &pe))
            {
                do
                {
                    // Convert TCHAR (WCHAR in Unicode build) to narrow char for comparison.
                    char lower[MAX_PATH] = {};
                    char namebuf[MAX_PATH] = {};
                    for (int i = 0; pe.szExeFile[i] && i < MAX_PATH - 1; ++i)
                    {
                        char c = static_cast<char>(pe.szExeFile[i] & 0xFF);
                        namebuf[i] = c;
                        lower[i]   = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    }

                    for (int i = 0; kBannedProcPrefixes[i]; ++i)
                    {
                        if (std::strstr(lower, kBannedProcPrefixes[i]))
                        {
                            g_tamper_flags.fetch_or(0x08);
                            log_event("CHEAT_PROCESS", namebuf);
                            send_tamper_report(g_tamper_flags.load(), std::string(namebuf));
                            break;
                        }
                    }
                } while (Process32Next(snap, &pe));
            }

            CloseHandle(snap);
        }
    }

    // Timer loop — eseguito in thread separato ogni 60s
    static std::atomic<bool> g_running{ false };
    static std::atomic<bool> g_session_logged{ false };

    static DWORD WINAPI monitor_thread(LPVOID)
    {
        // t=0: check immediati — cattura CE aperto PRIMA del login.
        // check_debugger: CE attached al processo → IsDebuggerPresent/NtQIP
        // check_cheat_processes: CE aperto ma non attached → scan per nome processo
        check_debugger();
        check_cheat_processes();

        // t=10s: attendi che il gioco carichi tutti i moduli legittimi,
        // poi baseline CRC + DLL scan + secondo process scan.
        std::this_thread::sleep_for(std::chrono::seconds(10));

        check_text_crc();
        check_injected_dlls();
        check_cheat_processes();

        // Loop ogni 30s — process scan SEMPRE, indipendentemente dallo screen state.
        // CE può essere aperto in qualsiasi momento: login, selezione personaggio, in-game.
        while (g_running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;

            check_debugger();
            check_injected_dlls();
            check_text_crc();
            check_cheat_processes();

            if (is_in_game())
            {
                if (!g_session_logged.exchange(true))
                    log_event("GAME_SESSION_ACTIVE", "screen_state=6");
            }
            else
            {
                g_session_logged.store(false);
            }
        }
        return 0;
    }
}

// ---------------------------------------------------------------------------
// hook::anti_tamper() — installato da Main()
// ---------------------------------------------------------------------------
void hook::anti_tamper()
{
    // Il gioco può essere avviato SOLO tramite l'updater.
    // sec_token::get() legge e cancella SDEV_SEC_TOKEN (env var one-shot).
    // Token assente = bypass updater → chiude il processo.
    if (sec_token::get()[0] == '\0')
    {
        MessageBoxA(nullptr,
            "Shaiya must be launched through the official Updater.\n\n"
            "Run Updater.exe to play.",
            "Shaiya Unauthorized Launch",
            MB_ICONERROR | MB_OK | MB_TOPMOST);
        ExitProcess(0xDEAD);
    }

    anti_tamper::g_running.store(true);
    HANDLE hThread = CreateThread(nullptr, 0,
        anti_tamper::monitor_thread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

uint8_t anti_tamper::get_tamper_flags()
{
    return anti_tamper::g_tamper_flags.load();
}

// Eseguito SINCRONO al momento del login (0xA101 send) — cattura debugger
// attaccato DOPO il check iniziale del thread (t=0) ma PRIMA del login.
void anti_tamper::check_now()
{
    check_debugger();   // veloce, nessun side-effect, nessun FP
}


