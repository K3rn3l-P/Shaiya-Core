#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdlib>
#include "CNetwork.h"
using namespace shaiya;

namespace {
    // Token effimero (4 byte hex) pre-computato dal server PHP e passato via env var SDEV_AUTH_KEY.
    // Il client NON ha il segreto: legge solo il token già pronto.
    // Token valido ~10 min (finestre ±1 da 5 min verificate in sdev-login.dll).
    // Env var cancellata subito dopo la lettura.
    static uint32_t compute_client_version_token()
    {
        static uint32_t token = 0;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            char hex[16]{};
            DWORD n = GetEnvironmentVariableA("SDEV_AUTH_KEY", hex, sizeof(hex));
            SetEnvironmentVariableA("SDEV_AUTH_KEY", nullptr);  // cancella dal processo
            if (n == 8)  // 8 hex chars = 4 byte little-endian uint32
                token = static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
        }
        return token;
    }
}

uint32_t CNetwork::ComputeToken()
{
    return compute_client_version_token();
}

void CNetwork::Send(void* buf, int len)
{
    // 0xA202 (select-server, 7 byte): XOR il server IP con il token.
    // Il server (hook a 0x4043BC) fa lo stesso XOR per ripristinare l'IP prima del lookup.
    if (len >= 7 && *reinterpret_cast<uint16_t*>(buf) == 0xA202)
    {
        uint32_t token = compute_client_version_token();
        *reinterpret_cast<uint32_t*>(static_cast<char*>(buf) + 3) ^= token;
    }
    typedef void(__cdecl* LPFN)(void*, int);
    (*(LPFN)0x5EA9A0)(buf, len);
}
