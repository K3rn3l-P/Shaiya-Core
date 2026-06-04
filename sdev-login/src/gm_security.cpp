#include "include/gm_security.h"

#include <array>
#include <cstring>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <sql.h>
#include <sqlext.h>
#pragma comment(lib, "bcrypt.lib")
#include "include/shaiya/SDatabase.h"

namespace gm_security
{
    namespace
    {
        constexpr const char* kIniFileName = "Data\\GMSecurity.ini";
        constexpr const char* kIniSection = "IP_WHITELIST";
        constexpr const char* kSettingsSection = "SETTINGS";
        constexpr int kMaxUsernameLen = 32;

        // Constant-time helpers — prevengono timing attack su confronti segreti.
        static bool secure_u32_eq(uint32_t a, uint32_t b)
        {
            volatile uint32_t diff = a ^ b;
            return diff == 0;
        }

        static bool secure_mem_eq(const void* a, const void* b, size_t n)
        {
            const volatile uint8_t* pa = reinterpret_cast<const volatile uint8_t*>(a);
            const volatile uint8_t* pb = reinterpret_cast<const volatile uint8_t*>(b);
            volatile uint8_t acc = 0;
            for (size_t i = 0; i < n; ++i) acc |= pa[i] ^ pb[i];
            return acc == 0;
        }

        const char* get_ini_path()
        {
            static char path[MAX_PATH]{};
            static bool resolved = false;

            if (!resolved)
            {
                resolved = true;

                DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
                if (len == 0 || len >= MAX_PATH)
                {
                    path[0] = '\0';
                    return path;
                }

                char* lastSlash = std::strrchr(path, '\\');
                if (!lastSlash)
                {
                    path[0] = '\0';
                    return path;
                }

                *(lastSlash + 1) = '\0';
                strcat_s(path, MAX_PATH, kIniFileName);
            }

            return path;
        }

        bool is_feature_enabled()
        {
            const char* iniPath = get_ini_path();
            if (!iniPath[0])
                return false;

            return GetPrivateProfileIntA(kSettingsSection, "Enabled", 0, iniPath) != 0;
        }

        int check_whitelist_ip(const char* username, const char* ipv4)
        {
            if (!username || !username[0] || !ipv4 || !ipv4[0])
                return 0;

            const char* iniPath = get_ini_path();
            if (!iniPath[0])
                return 0;

            char safeUser[kMaxUsernameLen + 1]{};
            strncpy_s(safeUser, username, kMaxUsernameLen);

            constexpr const char* kNotFound = "\x01";

            char allowedIps[512]{};
            GetPrivateProfileStringA(
                kIniSection, safeUser, kNotFound,
                allowedIps, sizeof(allowedIps), iniPath);

            if (std::strcmp(allowedIps, kNotFound) == 0)
                return 0;

            if (allowedIps[0] == '\0')
                return -1;

            const char* p = allowedIps;
            auto ipLen = std::strlen(ipv4);

            while (*p)
            {
                while (*p == ',' || *p == ' ' || *p == '\t')
                    ++p;
                if (*p == '\0')
                    break;

                const char* tokenStart = p;
                while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t')
                    ++p;
                auto tokenLen = static_cast<std::size_t>(p - tokenStart);

                if (tokenLen == ipLen && std::strncmp(tokenStart, ipv4, tokenLen) == 0)
                    return +1;
            }

            return -1;
        }

        bool is_db_admin_account(shaiya::SDatabase* db, const char* username)
        {
            if (!db || !username || !username[0])
                return false;

            const char* query =
                "SELECT TOP 1 [Admin], [AdminLevel], [Status] "
                "FROM [PS_UserData].[dbo].[Users_Master] "
                "WHERE [UserID] = ?";

            if (shaiya::SDatabase::PrepareSql(db, query))
                return false;

            char user[kMaxUsernameLen + 1]{};
            strncpy_s(user, username, kMaxUsernameLen);

            if (shaiya::SDatabase::BindParameter(db, 1, kMaxUsernameLen,
                    SQL_C_CHAR, SQL_VARCHAR, user, nullptr, SQL_PARAM_INPUT))
            {
                SQLFreeStmt(db->stmt, SQL_CLOSE);
                SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);
                return false;
            }

            if (shaiya::SDatabase::ExecuteSql(db))
            {
                SQLFreeStmt(db->stmt, SQL_CLOSE);
                SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);
                return false;
            }

            bool isAdmin = false;
            auto fetchRc = SQLFetch(db->stmt);

            if (fetchRc == SQL_SUCCESS)
            {
                unsigned char adminFlag = 0;
                SQLLEN ind1 = 0;
                SQLGetData(db->stmt, 1, SQL_C_UTINYINT, &adminFlag, sizeof(adminFlag), &ind1);

                unsigned char adminLevel = 0;
                SQLLEN ind2 = 0;
                SQLGetData(db->stmt, 2, SQL_C_UTINYINT, &adminLevel, sizeof(adminLevel), &ind2);

                short status = 0;
                SQLLEN ind3 = 0;
                SQLGetData(db->stmt, 3, SQL_C_SSHORT, &status, sizeof(status), &ind3);

                isAdmin = (adminFlag != 0) || (adminLevel > 0) || (status > 0);
            }

            SQLFreeStmt(db->stmt, SQL_CLOSE);
            SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);

            return isAdmin;
        }

        // --- Client Secret ---

        const char* get_client_secret()
        {
            static char secret[128]{};
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                GetPrivateProfileStringA(kSettingsSection, "ClientSecret", "",
                                         secret, sizeof(secret), get_ini_path());
            }
            return secret;
        }

        // --- Token Secret ---

        const char* get_token_secret()
        {
            static char secret[128]{};
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                GetPrivateProfileStringA(kSettingsSection, "TokenSecret", "",
                                         secret, sizeof(secret), get_ini_path());
            }
            return secret;
        }

        // --- PBKDF2 helpers ---

        bool hex_decode(const char* hex, size_t hex_len, unsigned char* out, size_t out_len)
        {
            if (hex_len != out_len * 2)
                return false;

            auto from_hex = [](unsigned char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };

            for (size_t i = 0; i < out_len; i++)
            {
                int hi = from_hex(static_cast<unsigned char>(hex[i * 2]));
                int lo = from_hex(static_cast<unsigned char>(hex[i * 2 + 1]));
                if (hi < 0 || lo < 0)
                    return false;
                out[i] = static_cast<unsigned char>((hi << 4) | lo);
            }
            return true;
        }

        bool get_enpassword(shaiya::SDatabase* db, const char* username, char* out, int out_size)
        {
            const char* query =
                "SELECT TOP 1 Enpassword "
                "FROM [PS_UserData].[dbo].[Users_Master] "
                "WHERE UserID = ?";

            if (shaiya::SDatabase::PrepareSql(db, query))
                return false;

            char user[kMaxUsernameLen + 1]{};
            strncpy_s(user, username, kMaxUsernameLen);

            if (shaiya::SDatabase::BindParameter(db, 1, kMaxUsernameLen,
                    SQL_C_CHAR, SQL_VARCHAR, user, nullptr, SQL_PARAM_INPUT))
            {
                SQLFreeStmt(db->stmt, SQL_CLOSE);
                SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);
                return false;
            }

            if (shaiya::SDatabase::ExecuteSql(db))
            {
                SQLFreeStmt(db->stmt, SQL_CLOSE);
                SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);
                return false;
            }

            out[0] = '\0';
            SQLRETURN rc = SQLFetch(db->stmt);
            if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
            {
                SQLLEN indicator = 0;
                SQLGetData(db->stmt, 1, SQL_C_CHAR, out, out_size, &indicator);
                if (indicator == SQL_NULL_DATA)
                    out[0] = '\0';
            }

            SQLFreeStmt(db->stmt, SQL_CLOSE);
            SQLFreeStmt(db->stmt, SQL_RESET_PARAMS);
            return true;
        }

        bool verify_pbkdf2(const char* stored, const char* password)
        {
            constexpr const char* kPrefix    = "pbkdf2sha256:";
            constexpr size_t      kPrefixLen = 13;
            constexpr size_t      kSaltBytes = 16;
            constexpr size_t      kHashBytes = 32;

            if (!stored || std::strncmp(stored, kPrefix, kPrefixLen) != 0)
                return false;

            const char* p = stored + kPrefixLen;

            char* end = nullptr;
            unsigned long iterations = std::strtoul(p, &end, 10);
            if (!end || *end != ':' || iterations == 0 || iterations > 2000000UL)
                return false;
            p = end + 1;

            const char* colon = std::strchr(p, ':');
            if (!colon || static_cast<size_t>(colon - p) != kSaltBytes * 2)
                return false;

            unsigned char salt[kSaltBytes]{};
            if (!hex_decode(p, kSaltBytes * 2, salt, kSaltBytes))
                return false;
            p = colon + 1;

            if (std::strlen(p) != kHashBytes * 2)
                return false;

            unsigned char expected[kHashBytes]{};
            if (!hex_decode(p, kHashBytes * 2, expected, kHashBytes))
                return false;

            BCRYPT_ALG_HANDLE hAlg = nullptr;
            NTSTATUS status = BCryptOpenAlgorithmProvider(
                &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
            if (!BCRYPT_SUCCESS(status))
                return false;

            unsigned char computed[kHashBytes]{};
            status = BCryptDeriveKeyPBKDF2(
                hAlg,
                reinterpret_cast<PUCHAR>(const_cast<char*>(password)),
                static_cast<ULONG>(std::strlen(password)),
                salt, static_cast<ULONG>(kSaltBytes),
                static_cast<ULONGLONG>(iterations),
                computed, static_cast<ULONG>(kHashBytes),
                0);

            BCryptCloseAlgorithmProvider(hAlg, 0);

            if (!BCRYPT_SUCCESS(status))
                return false;

            volatile int diff = 0;
            for (size_t i = 0; i < kHashBytes; i++)
                diff |= computed[i] ^ expected[i];

            return diff == 0;
        }

        bool verify_hmac_token(const char* username, const char* token)
        {
            if (!token || !username) return false;

            const char* secret = get_token_secret();

            if (!secret || !secret[0]) return false;

            for (int i = 0; i < 15; i++)
            {
                char c = token[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;
            }

            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER uli;
            uli.LowPart  = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            ULONGLONG unixSecs = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
            unsigned int window = (unsigned int)(unixSecs / 300);

            for (int w = 0; w <= 1; w++)
            {
                char data[96]{};
                std::snprintf(data, sizeof(data), "%s:%u",
                              username, window - (unsigned int)w);

                BCRYPT_ALG_HANDLE  hAlg  = nullptr;
                BCRYPT_HASH_HANDLE hHash = nullptr;
                BYTE hash[32]{};
                bool computed = false;

                if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                        BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0)
                {
                    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(secret)),
                            static_cast<ULONG>(std::strlen(secret)), 0) == 0)
                    {
                        if (BCryptHashData(hHash,
                                reinterpret_cast<PUCHAR>(data),
                                static_cast<ULONG>(std::strlen(data)), 0) == 0)
                            computed = (BCryptFinishHash(hHash, hash, 32, 0) == 0);
                    }
                }

                if (hHash) BCryptDestroyHash(hHash);
                if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);

                if (!computed) { SecureZeroMemory(hash, sizeof(hash)); continue; }

                char expected[17]{};
                std::snprintf(expected, sizeof(expected),
                    "%02x%02x%02x%02x%02x%02x%02x%02x",
                    hash[0], hash[1], hash[2], hash[3],
                    hash[4], hash[5], hash[6], hash[7]);

                bool match = secure_mem_eq(token, expected, 15);
                SecureZeroMemory(hash, sizeof(hash));
                SecureZeroMemory(expected, sizeof(expected));
                if (match) return true;
            }

            return false;
        }
    }

    bool is_login_allowed(shaiya::SDatabase* db, const char* username, const char* ipv4)
    {
        if (!is_feature_enabled())
            return true;

        int whitelistResult = check_whitelist_ip(username, ipv4);
        if (whitelistResult == -1)
            return false;

        if (whitelistResult == 0 && is_db_admin_account(db, username))
            return false;

        return true;
    }

    bool verify_password(shaiya::SDatabase* db, const char* username, const char* password, int* argonResult)
    {
        if (!argonResult)
            return false;

        *argonResult = 0;

        if (verify_hmac_token(username, password))
        {
            *argonResult = 1;
            return true;
        }

        char enpassword[256]{};
        if (!get_enpassword(db, username, enpassword, sizeof(enpassword)))
            return false;

        if (enpassword[0] == '\0')
            return false;

        *argonResult = verify_pbkdf2(enpassword, password) ? 1 : 0;
        return true;
    }

    bool verify_rsa_client_token(uint32_t token)
    {
        const char* secret = get_client_secret();
        if (!secret || !secret[0])
            return false;  // segreto non configurato → nega sempre

        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        ULONGLONG unixSecs = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
        unsigned int window = static_cast<unsigned int>(unixSecs / 300);

        // Controlla finestra corrente, precedente e successiva (±5 min di clock drift)
        for (int w = -1; w <= 1; w++)
        {
            unsigned int wnd = window + static_cast<unsigned int>(w);
            char data[32]{};
            std::snprintf(data, sizeof(data), "client:%u", wnd);

            BCRYPT_ALG_HANDLE  hAlg  = nullptr;
            BCRYPT_HASH_HANDLE hHash = nullptr;
            BYTE hash[32]{};
            bool computed = false;

            if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                    BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0)
            {
                if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                        reinterpret_cast<PUCHAR>(const_cast<char*>(secret)),
                        static_cast<ULONG>(std::strlen(secret)), 0) == 0)
                {
                    if (BCryptHashData(hHash,
                            reinterpret_cast<PUCHAR>(data),
                            static_cast<ULONG>(std::strlen(data)), 0) == 0)
                        computed = (BCryptFinishHash(hHash, hash, 32, 0) == 0);
                }
            }
            if (hHash) BCryptDestroyHash(hHash);
            if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);

            if (!computed) { SecureZeroMemory(hash, sizeof(hash)); continue; }

            uint32_t expected = static_cast<uint32_t>(hash[0])
                              | (static_cast<uint32_t>(hash[1]) << 8)
                              | (static_cast<uint32_t>(hash[2]) << 16)
                              | (static_cast<uint32_t>(hash[3]) << 24);

            bool match = secure_u32_eq(token, expected);
            SecureZeroMemory(hash, sizeof(hash));
            if (match) return true;
        }
        return false;
    }

    uint32_t get_client_auth_token()
    {
        const char* secret = get_client_secret();
        if (!secret || !secret[0])
            return 0;  // nessun segreto = XOR con 0 = passthrough (fail-open)

        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        ULONGLONG unixSecs = (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;
        unsigned int window = static_cast<unsigned int>(unixSecs / 300);

        char data[32]{};
        std::snprintf(data, sizeof(data), "client:%u", window);

        BCRYPT_ALG_HANDLE  hAlg  = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        BYTE hash[32]{};
        bool ok = false;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0)
        {
            if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                    reinterpret_cast<PUCHAR>(const_cast<char*>(secret)),
                    static_cast<ULONG>(std::strlen(secret)), 0) == 0)
            {
                if (BCryptHashData(hHash,
                        reinterpret_cast<PUCHAR>(data),
                        static_cast<ULONG>(std::strlen(data)), 0) == 0)
                    ok = (BCryptFinishHash(hHash, hash, 32, 0) == 0);
            }
        }
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);

        if (!ok) return 0;
        return static_cast<uint32_t>(hash[0])
             | (static_cast<uint32_t>(hash[1]) << 8)
             | (static_cast<uint32_t>(hash[2]) << 16)
             | (static_cast<uint32_t>(hash[3]) << 24);
    }
}
