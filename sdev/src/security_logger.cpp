#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <thread>
#include "../include/security_logger.h"

#pragma comment(lib, "odbc32.lib")

// ---------------------------------------------------------------------------
// security_logger — file + DB (SecurityEvents in PS_UserData)
//
// THREADING: log() è chiamato dal thread IOCP (receive) e da thread worker.
//   - log() = push in queue + write file (non-blocking, < 2μs)
//   - Worker thread separato = pop queue + INSERT ODBC (blocking OK qui)
//
// ODBC: connessione lazy nel worker thread — NON in DllMain né in IOCP thread.
//   SQLDriverConnect da IOCP → timeout TCP 3s → tutti i pacchetti bloccati.
//   Nel worker thread questo è accettabile (non impatta il gioco).
// ---------------------------------------------------------------------------

namespace security_logger
{
    namespace
    {
        static constexpr const char* kLogDir = "C:\\ShaiyaServer\\Logs";

        // -----------------------------------------------------------------------
        // Queue producer-consumer
        // -----------------------------------------------------------------------
        struct LogEntry
        {
            char ip[46];
            char username[34];
            char event_type[34];
            char detail[257];
        };

        struct BanEntry
        {
            int  uid;
            char reason[257];
        };

        struct SessionInvalidateEntry
        {
            char username[34];
        };

        static std::queue<LogEntry>               g_queue;
        static std::queue<BanEntry>               g_ban_queue;
        static std::queue<SessionInvalidateEntry> g_session_queue;
        static std::mutex              g_queue_mtx;
        static std::condition_variable g_queue_cv;
        static std::atomic<bool>       g_worker_running{ false };
        static std::thread             g_worker;

        // -----------------------------------------------------------------------
        // ODBC — usato SOLO dal worker thread
        // -----------------------------------------------------------------------
        static SQLHENV g_hEnv  = SQL_NULL_HENV;
        static SQLHDBC g_hDbc  = SQL_NULL_HDBC;
        static bool    g_db_ok = false;

        static void odbc_connect()
        {
            if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_hEnv) != SQL_SUCCESS)
                return;
            SQLSetEnvAttr(g_hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
            if (SQLAllocHandle(SQL_HANDLE_DBC, g_hEnv, &g_hDbc) != SQL_SUCCESS)
                return;
            SQLSetConnectAttr(g_hDbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

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

        static void odbc_insert(const LogEntry& e)
        {
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
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                                 33,  0, (SQLPOINTER)e.username,   0, &cbU);
                SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                 45,  0, (SQLPOINTER)e.ip,         0, &cbI);
                SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                                 33,  0, (SQLPOINTER)e.event_type, 0, &cbE);
                SQLBindParameter(hStmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                                 256, 0, (SQLPOINTER)e.detail,     0, &cbD);
                RETCODE rc = SQLExecute(hStmt);
                // Se la connessione è caduta, prova a riconnettersi una volta
                if (rc == SQL_ERROR)
                {
                    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
                    g_db_ok = false;
                    odbc_connect();
                    return;
                }
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        }

        // -----------------------------------------------------------------------
        // -----------------------------------------------------------------------
        // DELETE GameSecuritySessions — chiamata dal worker thread al logout
        // -----------------------------------------------------------------------
        static void odbc_invalidate_session(const SessionInvalidateEntry& s)
        {
            if (!g_db_ok) return;
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
                return;
            static const SQLCHAR kSql[] =
                "DELETE FROM dbo.GameSecuritySessions WHERE UserID = ?";
            if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
            {
                SQLLEN cb = SQL_NTS;
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                                 33, 0, (SQLPOINTER)s.username, 0, &cb);
                RETCODE rc = SQLExecute(hStmt);
                if (rc == SQL_ERROR) { g_db_ok = false; odbc_connect(); }
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        }

        // -----------------------------------------------------------------------
        // sp_BanUser — chiamata dal worker thread
        // -----------------------------------------------------------------------
        static void odbc_ban_user(const BanEntry& b)
        {
            if (!g_db_ok) return;
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
                return;
            static const SQLCHAR kSql[] =
                "EXEC dbo.sp_BanUser @UserUID=?, @DaysBann=3650, @Reason=?, @GMID='sdev-auto'";
            if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
            {
                SQLLEN cbUid = 0, cbR = SQL_NTS;
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, const_cast<int*>(&b.uid), 0, &cbUid);
                SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                                 256, 0, (SQLPOINTER)b.reason, 0, &cbR);
                RETCODE rc = SQLExecute(hStmt);
                if (rc == SQL_ERROR) { g_db_ok = false; odbc_connect(); }
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        }

        // -----------------------------------------------------------------------
        // Worker thread — gira finché g_worker_running = true
        // -----------------------------------------------------------------------
        static void worker_thread_fn()
        {
            odbc_connect();  // blocca qui per max 5s — OK, non è IOCP

            while (g_worker_running.load())
            {
                std::unique_lock<std::mutex> lk(g_queue_mtx);
                g_queue_cv.wait(lk, [] {
                    return !g_queue.empty() || !g_ban_queue.empty() ||
                           !g_session_queue.empty() || !g_worker_running.load();
                });

                while (!g_queue.empty())
                {
                    LogEntry entry = g_queue.front();
                    g_queue.pop();
                    lk.unlock();
                    odbc_insert(entry);
                    lk.lock();
                }

                while (!g_ban_queue.empty())
                {
                    BanEntry entry = g_ban_queue.front();
                    g_ban_queue.pop();
                    lk.unlock();
                    odbc_ban_user(entry);
                    lk.lock();
                }

                while (!g_session_queue.empty())
                {
                    SessionInvalidateEntry entry = g_session_queue.front();
                    g_session_queue.pop();
                    lk.unlock();
                    odbc_invalidate_session(entry);
                    lk.lock();
                }
            }

            // Drain residuo alla chiusura
            while (!g_queue.empty())
            {
                odbc_insert(g_queue.front());
                g_queue.pop();
            }
            while (!g_ban_queue.empty())
            {
                odbc_ban_user(g_ban_queue.front());
                g_ban_queue.pop();
            }
            while (!g_session_queue.empty())
            {
                odbc_invalidate_session(g_session_queue.front());
                g_session_queue.pop();
            }

            if (g_hDbc != SQL_NULL_HDBC) {
                SQLDisconnect(g_hDbc);
                SQLFreeHandle(SQL_HANDLE_DBC, g_hDbc);
            }
            if (g_hEnv != SQL_NULL_HENV)
                SQLFreeHandle(SQL_HANDLE_ENV, g_hEnv);
        }

        // -----------------------------------------------------------------------
        // File log — chiamato con g_queue_mtx acquisito (da push_entry)
        // -----------------------------------------------------------------------
        static void write_file(const LogEntry& e)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            CreateDirectoryA(kLogDir, nullptr);
            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\security_%04d-%02d-%02d.log",
                          kLogDir, st.wYear, st.wMonth, st.wDay);
            char line[600];
            int len = std::snprintf(line, sizeof(line),
                "[%02d:%02d:%02d] %-28s | ip=%-15s | user=%-20s | %s\r\n",
                st.wHour, st.wMinute, st.wSecond,
                e.event_type[0] ? e.event_type : "",
                e.ip[0] ? e.ip : "?",
                e.username[0] ? e.username : "-",
                e.detail[0] ? e.detail : "");
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

    // -----------------------------------------------------------------------
    // API pubblica
    // -----------------------------------------------------------------------

    void init()
    {
        // Chiamare DOPO DllMain, da un thread normale (non IOCP, non Loader Lock).
        // Avvia il worker thread che gestisce la connessione ODBC in background.
        if (g_worker_running.load()) return;
        g_worker_running.store(true);
        g_worker = std::thread(worker_thread_fn);
    }

    void shutdown()
    {
        if (!g_worker_running.load()) return;
        g_worker_running.store(false);
        g_queue_cv.notify_all();
        if (g_worker.joinable())
            g_worker.join();
    }

    void log(const char* ip, const char* username,
             const char* event_type, const char* detail)
    {
        LogEntry e{};
        if (ip)         strncpy_s(e.ip,         sizeof(e.ip),         ip,         _TRUNCATE);
        if (username)   strncpy_s(e.username,   sizeof(e.username),   username,   _TRUNCATE);
        if (event_type) strncpy_s(e.event_type, sizeof(e.event_type), event_type, _TRUNCATE);
        if (detail)     strncpy_s(e.detail,     sizeof(e.detail),     detail,     _TRUNCATE);

        {
            std::lock_guard<std::mutex> lk(g_queue_mtx);
            write_file(e);
            if (g_worker_running.load())
            {
                g_queue.push(e);
                g_queue_cv.notify_one();
            }
        }
    }

    void ban_user(int uid, const char* reason)
    {
        if (!g_worker_running.load()) return;
        BanEntry b{};
        b.uid = uid;
        if (reason) strncpy_s(b.reason, sizeof(b.reason), reason, _TRUNCATE);
        {
            std::lock_guard<std::mutex> lk(g_queue_mtx);
            g_ban_queue.push(b);
            g_queue_cv.notify_one();
        }
    }

    void invalidate_session(const char* username)
    {
        if (!g_worker_running.load() || !username) return;
        SessionInvalidateEntry e{};
        strncpy_s(e.username, sizeof(e.username), username, _TRUNCATE);
        {
            std::lock_guard<std::mutex> lk(g_queue_mtx);
            g_session_queue.push(e);
            g_queue_cv.notify_one();
        }
    }
}
