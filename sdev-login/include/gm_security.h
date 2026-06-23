#pragma once
#include <cstdint>

namespace shaiya
{
    struct SDatabase;
}

namespace gm_security
{
    bool is_login_allowed(shaiya::SDatabase* db, const char* username, const char* ipv4);
    bool verify_password(shaiya::SDatabase* db, const char* username, const char* password, int* argonResult);
    uint32_t get_client_auth_token();
    bool verify_rsa_client_token(uint32_t token);
    // Layer 2: verifica SHA256(.text memoria)[0:8] inviato nel packet 0xA101.
    // Fail-open se GameHashHex non è configurato in GMSecurity.ini.
    // Logga sempre il valore ricevuto (utile per aggiornare GameHashHex dopo rebuild).
    bool verify_sah_token(const uint8_t* client_token, const char* ip);
    // Layer 3: verifica HWID + firma HMAC del client contro il DB.
    // Prima connessione: salva nel DB (fail-open). Mismatch: logga.
    // sig = firma HMAC-SHA256(hwid, HwidSigKey) inviata dal client (64 hex chars).
    // Ritorna false → caller deve disconnettere il client in questi casi:
    //   - HWID in dbo.BlockedHWID (ban hardware attivo)
    //   - sig invalida (HwidSigKey configurato e HMAC errato → HWID_SIG_INVALID)
    //   - HWID mancante con [HWID] EnforceKick=1 in GMSecurity.ini
    bool verify_hwid(shaiya::SDatabase* db, const char* username,
                     const char* hwid, const char* sig, const char* ip);
}

namespace security_logger
{
    // Scrive un evento di sicurezza in C:\ShaiyaServer\Logs\security_YYYY-MM-DD.log
    void log(const char* ip, const char* username, const char* event_type, const char* detail);

    // Controlla se un HWID è in dbo.BlockedHWID (connessione PS_UserData del logger,
    // sotto g_mtx). Usato al handshake 0xA101 per disconnettere prima del login.
    // Fail-open: se il DB non è disponibile ritorna false (il check a login resta).
    bool is_hwid_blocked(const char* hwid);
}
