#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// hwid::get() — HWID stabile dall'Updater (%LOCALAPPDATA%\Shaiya_Duff_Updater\device.id)
// hwid::get_sig() — firma grezza da device.id.sig (64 hex chars, non verificata client-side)
// hwid::is_signed() — true se device.id.sig era presente
//
// Sicurezza:
//   - La chiave HMAC NON è nel client. Verifica avviene SOLO lato server (sdev-login).
//   - sdev-login legge HwidSigKey da GMSecurity.ini e verifica HMAC-SHA256(hwid, key).
//   - Se sig ASSENTE → g_hwid = "" → HWID_MISSING → sdev-login disconnette (EnforceKick=1)
//   - Se sig PRESENTE ma ERRATA → sdev-login logga HWID_SIG_INVALID → disconnect
// ---------------------------------------------------------------------------
namespace hwid
{
    static char g_hwid[33]     = {};
    static char g_hwid_sig[65] = {};  // sig grezza da device.id.sig (64 hex chars)
    static bool g_loaded       = false;
    static bool g_signed       = false;      // true = sig file presente
    static bool g_sig_tampered = false;      // true = sig file assente (HWID sarà vuoto)

    static char* read_file_trim(const char* path, char* buf, DWORD bufsz)
    {
        buf[0] = '\0';
        HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return nullptr;
        DWORD nRead = 0;
        ReadFile(hFile, buf, bufsz - 1, &nRead, nullptr);
        CloseHandle(hFile);
        buf[nRead] = '\0';

        char* p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        char* end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\r' || end[-1] == '\n'))
            --end;
        *end = '\0';
        return p;
    }

    const char* get()
    {
        if (g_loaded) return g_hwid;
        g_loaded = true;

        char localAppData[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH))
            return g_hwid;

        char pathId[MAX_PATH], pathSig[MAX_PATH];
        std::snprintf(pathId,  sizeof(pathId),  "%s\\Shaiya_Duff_Updater\\device.id",     localAppData);
        std::snprintf(pathSig, sizeof(pathSig), "%s\\Shaiya_Duff_Updater\\device.id.sig", localAppData);

        char rawId[64]  = {};
        char rawSig[72] = {};

        char* hwid_str = read_file_trim(pathId, rawId, sizeof(rawId));
        if (!hwid_str || hwid_str[0] == '\0')
            return g_hwid;  // device.id assente → HWID_MISSING

        size_t len = strlen(hwid_str);
        if (len > 32) len = 32;

        char* sig_str = read_file_trim(pathSig, rawSig, sizeof(rawSig));
        if (!sig_str || sig_str[0] == '\0')
        {
            // Sig assente → HWID non inviato, sdev-login farà HWID_MISSING + disconnect
            g_sig_tampered = true;
            return g_hwid;  // vuoto
        }

        // Sig presente → inviamo entrambi al server che verifica l'HMAC
        size_t sigLen = strlen(sig_str);
        if (sigLen > 64) sigLen = 64;
        memcpy(g_hwid_sig, sig_str, sigLen);
        g_hwid_sig[sigLen] = '\0';
        g_signed = true;

        memcpy(g_hwid, hwid_str, len);
        g_hwid[len] = '\0';
        return g_hwid;
    }

    const char* get_sig()  { return g_hwid_sig; }
    bool is_signed()       { return g_signed; }
    bool is_tampered()     { return g_sig_tampered; }
}
