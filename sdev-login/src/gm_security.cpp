#include "include/gm_security.h"
#include "include/login_logger.h"

#include <array>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
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

        // --- SAH Token (Layer 2) ---

        const uint8_t* get_expected_sah_token()
        {
            static uint8_t token[8] = {};
            static bool loaded = false;
            static bool configured = false;
            if (!loaded)
            {
                loaded = true;
                char hex[17]{};
                GetPrivateProfileStringA(kSettingsSection, "GameHashHex", "",
                                         hex, sizeof(hex), get_ini_path());
                configured = (std::strlen(hex) == 16 && hex_decode(hex, 16, token, 8));
            }
            return configured ? token : nullptr;
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

    bool verify_sah_token(const uint8_t* client_token, const char* ip)
    {
        if (!client_token)
            return false;

        // Logga sempre il valore ricevuto — utile dopo ogni rebuild per aggiornare GameHashHex.
        char receivedHex[17] = {};
        for (int i = 0; i < 8; ++i)
            std::snprintf(receivedHex + i * 2, 3, "%02x", client_token[i]);
        char hashLogMsg[40] = {};
        std::snprintf(hashLogMsg, sizeof(hashLogMsg), "GAME_HASH_RECEIVED=%s", receivedHex);
        login_logger::log_fail(ip ? ip : "?", "-", hashLogMsg);

        const uint8_t* expected = get_expected_sah_token();
        if (!expected)
        {
            security_logger::log(ip ? ip : "?", "-", "SAH_CONFIG_MISSING",
                "GameHashHex non configurato in GMSecurity.ini — nega accesso");
            return false;  // fail-closed: nessun hash configurato → nega sempre
        }

        if (!secure_mem_eq(client_token, expected, 8))
        {
            char expectedHex[17] = {};
            for (int i = 0; i < 8; ++i)
                std::snprintf(expectedHex + i * 2, 3, "%02x", expected[i]);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "expected=%s got=%s", expectedHex, receivedHex);
            security_logger::log(ip ? ip : "?", "-", "GAME_HASH_MISMATCH", msg);
            return false;
        }
        return true;
    }

    uint32_t get_client_auth_token()
    {
        const char* secret = get_client_secret();
        if (!secret || !secret[0])
            return 0;  // Nessun segreto = XOR con 0 (IP server inviato in chiaro).
                       // NON è un buco di auth: questo token offusca solo l'IP nella
                       // select-server (post-login). Il gate vero è verify_rsa_client_token
                       // (0xA101), già fail-closed: senza ClientSecret nessun client
                       // autentica → questo ramo è di fatto irraggiungibile.

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

// ===========================================================================
// security_logger — log file + DB (SecurityEvents in PS_UserData)
// ===========================================================================
namespace security_logger
{
    namespace
    {
        static std::mutex g_mtx;
        static constexpr const char* kLogDir = "C:\\ShaiyaServer\\Logs";

        // ODBC lazy connection a PS_UserData via Windows Auth locale
        static SQLHENV g_hEnv     = SQL_NULL_HENV;
        static SQLHDBC g_hDbc     = SQL_NULL_HDBC;
        static bool    g_db_ok    = false;
        static bool    g_db_tried = false;

        static void db_init_unlocked()
        {
            if (g_db_tried) return;
            g_db_tried = true;

            if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_hEnv) != SQL_SUCCESS)
                return;
            SQLSetEnvAttr(g_hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
            if (SQLAllocHandle(SQL_HANDLE_DBC, g_hEnv, &g_hDbc) != SQL_SUCCESS)
                return;
            SQLSetConnectAttr(g_hDbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)3, 0);

            // Credenziali da variabili d'ambiente di sistema — mai hardcoded né in INI.
            // Fail-safe: se mancano, DB logging disabilitato (il file log resta attivo).
            char user[128]{}, pwd[256]{};
            DWORD un = GetEnvironmentVariableA("SHAIYA_LOGDB_USER", user, sizeof(user));
            DWORD pn = GetEnvironmentVariableA("SHAIYA_LOGDB_PWD",  pwd,  sizeof(pwd));
            if (un == 0 || un >= sizeof(user) || pn == 0 || pn >= sizeof(pwd))
                return;

            char connIn[512]{};
            std::snprintf(connIn, sizeof(connIn),
                "Driver={ODBC Driver 17 for SQL Server};"
                "Server=127.0.0.1;Database=PS_UserData;Uid=%s;Pwd=%s;",
                user, pwd);
            SQLSMALLINT outLen = 0;
            RETCODE rc = SQLDriverConnectA(g_hDbc, nullptr, (SQLCHAR*)connIn, SQL_NTS,
                                           nullptr, 0, &outLen, SQL_DRIVER_NOPROMPT);
            g_db_ok = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
            SecureZeroMemory(connIn, sizeof(connIn));
            SecureZeroMemory(pwd,    sizeof(pwd));
        }

        static void write_db_unlocked(const char* ip, const char* username,
                                       const char* event_type, const char* detail)
        {
            db_init_unlocked();
            if (!g_db_ok) return;

            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
                return;

            static const SQLCHAR kSql[] =
                "INSERT INTO [dbo].[SecurityEvents] (Username,IP,EventType,Detail)"
                " VALUES (?,?,?,?)";
            if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
            {
                SQLLEN cbU = SQL_NTS, cbI = SQL_NTS, cbE = SQL_NTS, cbD = SQL_NTS;
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 64,  0, (SQLPOINTER)username,   0, &cbU);
                SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 45,  0, (SQLPOINTER)ip,         0, &cbI);
                SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 32,  0, (SQLPOINTER)event_type, 0, &cbE);
                SQLBindParameter(hStmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 256, 0, (SQLPOINTER)detail,     0, &cbD);
                SQLExecute(hStmt);
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        }

        static void write_file_unlocked(const char* ip, const char* username,
                                         const char* event_type, const char* detail)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            CreateDirectoryA(kLogDir, nullptr);
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\security_%04d-%02d-%02d.log",
                          kLogDir, st.wYear, st.wMonth, st.wDay);
            char line[512];
            int len = std::snprintf(line, sizeof(line),
                "[%02d:%02d:%02d] %-24s | ip=%-15s | user=%-20s | %s\r\n",
                st.wHour, st.wMinute, st.wSecond,
                event_type ? event_type : "",
                ip ? ip : "?",
                username ? username : "-",
                detail ? detail : "");
            if (len <= 0) return;
            HANDLE hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return;
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    bool is_hwid_blocked(const char* hwid)
    {
        if (!hwid || !hwid[0]) return false;
        char safeHwid[33] = {};
        strncpy_s(safeHwid, sizeof(safeHwid), hwid, 32);

        std::lock_guard<std::mutex> lk(g_mtx);
        db_init_unlocked();
        if (!g_db_ok) return false;  // fail-open: il check autoritativo resta a login (verify_hwid)

        SQLHSTMT hStmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
            return false;

        bool blocked = false;
        static const SQLCHAR kSql[] =
            "SELECT TOP 1 1 FROM [PS_UserData].[dbo].[BlockedHWID]"
            " WHERE Hwid=? AND (ExpiresAt IS NULL OR ExpiresAt > GETDATE())";
        if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
        {
            SQLLEN cb = SQL_NTS;
            SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                             32, 0, safeHwid, sizeof(safeHwid), &cb);
            if (SQLExecute(hStmt) == SQL_SUCCESS)
            {
                SQLRETURN rc = SQLFetch(hStmt);
                blocked = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
            }
        }
        SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        return blocked;
    }

    void log(const char* ip, const char* username,
             const char* event_type, const char* detail)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        write_file_unlocked(ip, username, event_type, detail);
        write_db_unlocked(ip, username, event_type, detail);
    }
}

// ===========================================================================
// gm_security::verify_hwid_sig / verify_hwid
//
// verify_hwid_sig: verifica HMAC-SHA256(hwid, HwidSigKey) contro la sig ricevuta.
//   La chiave NON è nel client — è letta da GMSecurity.ini [HWID] HwidSigKey (hex).
//   Ritorna false se chiave non configurata, sig vuota, lunghezza errata, o digest diverso.
//
// verify_hwid: verifica HWID + sig + BlockedHWID contro il DB.
//   Prima connessione (LastHWID NULL): salva e passa (fail-open).
//   HWID cambiato: logga HWID_MISMATCH.
// ===========================================================================
namespace gm_security
{
    // Verifica HMAC-SHA256(hwid, key) dove key è letta da GMSecurity.ini [HWID] HwidSigKey.
    static bool verify_hwid_sig(const char* hwid, const char* sig)
    {
        if (!hwid || !hwid[0] || !sig || !sig[0]) return false;

        char hexKey[65] = {};
        GetPrivateProfileStringA("HWID", "HwidSigKey", "", hexKey, sizeof(hexKey), get_ini_path());
        if (!hexKey[0] || strlen(hexKey) != 64) return false;

        BYTE key[32] = {};
        for (int i = 0; i < 32; ++i)
        {
            char byteStr[3] = { hexKey[i * 2], hexKey[i * 2 + 1], '\0' };
            key[i] = static_cast<BYTE>(std::strtoul(byteStr, nullptr, 16));
        }

        BCRYPT_ALG_HANDLE  hAlg  = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        BYTE hash[32] = {};
        bool ok = false;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0)
        {
            if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, key, sizeof(key), 0) == 0)
            {
                ULONG hlen = static_cast<ULONG>(std::strlen(hwid));
                BCryptHashData(hHash, reinterpret_cast<PBYTE>(const_cast<char*>(hwid)), hlen, 0);
                ok = (BCryptFinishHash(hHash, hash, 32, 0) == 0);
            }
            if (hHash) BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
        }
        SecureZeroMemory(key, sizeof(key));
        if (!ok) return false;

        char expected[65] = {};
        for (int i = 0; i < 32; ++i)
            std::snprintf(expected + i * 2, 3, "%02x", static_cast<unsigned>(hash[i]));

        bool match = (std::strlen(sig) == 64) && secure_mem_eq(sig, expected, 64);
        SecureZeroMemory(hash, sizeof(hash));
        SecureZeroMemory(expected, sizeof(expected));
        return match;
    }

    bool verify_hwid(shaiya::SDatabase* db, const char* username,
                     const char* hwid, const char* sig, const char* ip)
    {
        if (!db || !username || !username[0]) return true;

        // HWID vuoto = client ha bypassato l'updater (device.id assente o sig tamperata)
        bool hwid_present = (hwid && hwid[0]);
        if (!hwid_present)
        {
            security_logger::log(ip, username, "HWID_MISSING",
                "client non ha inviato HWID (bypass updater?)");

            // Bypass fix: cancellare device.id.sig → sdev-client invia HWID vuoto →
            // BlockedHWID saltato. Leggiamo LastHWID dal DB per l'account e verifichiamo.
            char safeUser2[kMaxUsernameLen + 1] = {};
            strncpy_s(safeUser2, username, kMaxUsernameLen);
            char storedHwid2[33] = {};
            bool hasStored2 = false;
            {
                SQLHSTMT hStmt = SQL_NULL_HSTMT;
                if (SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &hStmt) == SQL_SUCCESS)
                {
                    const char* sel =
                        "SELECT TOP 1 CAST(LastHWID AS VARCHAR(33))"
                        " FROM [PS_UserData].[dbo].[Members] WHERE UserID=?";
                    if (SQLPrepareA(hStmt, (SQLCHAR*)sel, SQL_NTS) == SQL_SUCCESS)
                    {
                        SQLLEN cbU = SQL_NTS;
                        SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         kMaxUsernameLen, 0, safeUser2, kMaxUsernameLen + 1, &cbU);
                        if (SQLExecute(hStmt) == SQL_SUCCESS)
                        {
                            SQLRETURN rc = SQLFetch(hStmt);
                            if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
                            {
                                SQLLEN ind = 0;
                                SQLGetData(hStmt, 1, SQL_C_CHAR, storedHwid2, sizeof(storedHwid2), &ind);
                                hasStored2 = (ind != SQL_NULL_DATA && storedHwid2[0] != '\0');
                            }
                        }
                    }
                    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
                }
            }
            if (hasStored2)
            {
                SQLHSTMT hStmt = SQL_NULL_HSTMT;
                bool blocked2 = false;
                if (SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &hStmt) == SQL_SUCCESS)
                {
                    const char* sql =
                        "SELECT TOP 1 1 FROM [PS_UserData].[dbo].[BlockedHWID]"
                        " WHERE Hwid=? AND (ExpiresAt IS NULL OR ExpiresAt > GETDATE())";
                    if (SQLPrepareA(hStmt, (SQLCHAR*)sql, SQL_NTS) == SQL_SUCCESS)
                    {
                        SQLLEN cbH = SQL_NTS;
                        SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         32, 0, storedHwid2, 33, &cbH);
                        if (SQLExecute(hStmt) == SQL_SUCCESS)
                        {
                            SQLRETURN rc = SQLFetch(hStmt);
                            blocked2 = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
                        }
                    }
                    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
                }
                if (blocked2)
                {
                    security_logger::log(ip, username, "HWID_BLOCKED", storedHwid2);
                    return false;
                }
            }

            // EnforceKick=1 → HWID mancante = bypass updater → disconnect
            // EnforceKick=0 → fail-open (modalità transizione, vecchi client senza sig)
            if (GetPrivateProfileIntA("HWID", "EnforceKick", 0, get_ini_path()))
                return false;

            return true;
        }

        // Sanitizza HWID — max 32 char, stessa lunghezza device.id
        char safeHwid[33] = {};
        strncpy_s(safeHwid, sizeof(safeHwid), hwid, 32);
        safeHwid[32] = '\0';

        char safeUser[kMaxUsernameLen + 1] = {};
        strncpy_s(safeUser, username, kMaxUsernameLen);

        // -----------------------------------------------------------------
        // Check BlockedHWID — blocca anche account fasullo con stesso hardware.
        // Priorità massima: check prima di qualsiasi altra logica HWID.
        // -----------------------------------------------------------------
        {
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            bool hwid_blocked = false;
            if (SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &hStmt) == SQL_SUCCESS)
            {
                const char* sql =
                    "SELECT TOP 1 1 FROM [PS_UserData].[dbo].[BlockedHWID]"
                    " WHERE Hwid=? AND (ExpiresAt IS NULL OR ExpiresAt > GETDATE())";
                if (SQLPrepareA(hStmt, (SQLCHAR*)sql, SQL_NTS) == SQL_SUCCESS)
                {
                    SQLLEN cbHwid = SQL_NTS;
                    SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                     32, 0, safeHwid, 33, &cbHwid);
                    if (SQLExecute(hStmt) == SQL_SUCCESS)
                    {
                        SQLRETURN rc = SQLFetch(hStmt);
                        hwid_blocked = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
                    }
                }
                SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
            }
            if (hwid_blocked)
            {
                security_logger::log(ip, username, "HWID_BLOCKED", safeHwid);
                return false; // caller disconnette
            }
        }

        // -----------------------------------------------------------------
        // Verifica firma HMAC-SHA256(hwid, HwidSigKey) se chiave configurata.
        // Chiave in GMSecurity.ini [HWID] HwidSigKey — mai nel client.
        // -----------------------------------------------------------------
        {
            char sigKeyCheck[2] = {};
            GetPrivateProfileStringA("HWID", "HwidSigKey", "", sigKeyCheck,
                                     sizeof(sigKeyCheck), get_ini_path());
            if (sigKeyCheck[0] != '\0')
            {
                bool sig_present = (sig && sig[0]);
                if (!sig_present)
                {
                    // HWID presente ma sig assente → sdev-client modificato
                    security_logger::log(ip, username, "HWID_SIG_ABSENT", safeHwid);
                    return false;
                }
                if (!verify_hwid_sig(safeHwid, sig))
                {
                    security_logger::log(ip, username, "HWID_SIG_INVALID", safeHwid);
                    return false;
                }
            }
        }

        // Leggi HWID precedente con ODBC diretto (no SDatabase per evitare il bug cursor)
        char storedHwid[33] = {};
        bool isFirstTime = true;
        {
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &hStmt) == SQL_SUCCESS)
            {
                // CAST→VARCHAR evita troncamento a 31 char del driver ODBC su NVARCHAR(32)
                const char* sel = "SELECT TOP 1 CAST(LastHWID AS VARCHAR(33)) FROM [PS_UserData].[dbo].[Members] WHERE UserID=?";
                if (SQLPrepareA(hStmt, (SQLCHAR*)sel, SQL_NTS) == SQL_SUCCESS)
                {
                    SQLLEN cbUser = SQL_NTS;
                    SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                     kMaxUsernameLen, 0, safeUser, kMaxUsernameLen+1, &cbUser);
                    if (SQLExecute(hStmt) == SQL_SUCCESS)
                    {
                        SQLLEN ind = 0;
                        SQLRETURN rc = SQLFetch(hStmt);
                        if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
                        {
                            SQLGetData(hStmt, 1, SQL_C_CHAR, storedHwid, sizeof(storedHwid), &ind);
                            isFirstTime = (ind == SQL_NULL_DATA || storedHwid[0] == '\0');
                        }
                    }
                }
                SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
            }
        }

        // Upsert Members: INSERT prima volta, UPDATE altrimenti
        {
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, db->dbc, &hStmt) == SQL_SUCCESS)
            {
                const char* upsert = isFirstTime
                    ? "INSERT INTO [PS_UserData].[dbo].[Members] (UserID,LastHWID,LastHWIDTime) VALUES (?,?,GETDATE())"
                    : "UPDATE [PS_UserData].[dbo].[Members] SET LastHWID=?,LastHWIDTime=GETDATE() WHERE UserID=?";

                if (SQLPrepareA(hStmt, (SQLCHAR*)upsert, SQL_NTS) == SQL_SUCCESS)
                {
                    SQLLEN cbUser = SQL_NTS, cbHwid = SQL_NTS;
                    if (isFirstTime)
                    {
                        SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         kMaxUsernameLen, 0, safeUser, kMaxUsernameLen+1, &cbUser);
                        SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         32, 0, safeHwid, 33, &cbHwid);
                    }
                    else
                    {
                        SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         32, 0, safeHwid, 33, &cbHwid);
                        SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                         kMaxUsernameLen, 0, safeUser, kMaxUsernameLen+1, &cbUser);
                    }
                    SQLExecute(hStmt);
                }
                SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
            }
        }

        if (isFirstTime)
        {
            security_logger::log(ip, username, "HWID_REGISTERED", safeHwid);
            return true;
        }

        // HWID già registrato: confronto
        if (std::strcmp(storedHwid, safeHwid) != 0)
        {
            char detail[128];
            std::snprintf(detail, sizeof(detail),
                "stored=%.32s new=%.32s", storedHwid, safeHwid);
            security_logger::log(ip, username, "HWID_MISMATCH", detail);

            // EnforceKick=1 in GMSecurity.ini → disconnette il client
            if (GetPrivateProfileIntA("HWID", "EnforceKick", 0, get_ini_path()))
                return false;
        }

        return true;
    }
}
