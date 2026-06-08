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
    // Layer 2: verifica SHA256(data.sah)[0:8] inviato nel packet 0xA101.
    // Fail-open se SahTokenHex non è configurato in GMSecurity.ini.
    bool verify_sah_token(const uint8_t* client_token);
}
