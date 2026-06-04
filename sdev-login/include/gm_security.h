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
}
