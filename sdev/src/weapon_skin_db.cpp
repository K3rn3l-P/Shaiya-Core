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
#include <future>
#include <utility>
#include "../include/weapon_skin_db.h"

#pragma comment(lib, "odbc32.lib")

// ---------------------------------------------------------------------------
// Persistenza weapon skin — ODBC verso PS_GameData.dbo.CharWeaponSkins.
// Vedi weapon_skin_db.h e la guida install NAS (WeaponSkin-Persistence).
// ---------------------------------------------------------------------------
namespace weapon_skin_db
{
    namespace
    {
        struct SaveEntry
        {
            uint32_t charId;
            uint8_t  type;
            uint8_t  typeId;
        };

        struct LoadResult
        {
            bool    found;
            uint8_t type;
            uint8_t typeId;
        };

        struct LoadEntry
        {
            uint32_t                   charId;
            std::promise<LoadResult>   result;
        };

        std::queue<SaveEntry> g_save_queue;
        std::queue<LoadEntry> g_load_queue;
        std::mutex              g_mtx;
        std::condition_variable g_cv;
        std::atomic<bool>       g_running{ false };
        std::thread             g_worker;

        // ODBC — usato SOLO dal worker thread
        SQLHENV g_hEnv  = SQL_NULL_HENV;
        SQLHDBC g_hDbc  = SQL_NULL_HDBC;
        bool    g_db_ok = false;

        void odbc_connect()
        {
            if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_hEnv) != SQL_SUCCESS)
                return;
            SQLSetEnvAttr(g_hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
            if (SQLAllocHandle(SQL_HANDLE_DBC, g_hEnv, &g_hDbc) != SQL_SUCCESS)
                return;
            SQLSetConnectAttr(g_hDbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

            char user[128]{}, pwd[256]{};
            DWORD un = GetEnvironmentVariableA("SHAIYA_SKINDB_USER", user, sizeof(user));
            DWORD pn = GetEnvironmentVariableA("SHAIYA_SKINDB_PWD",  pwd,  sizeof(pwd));
            if (un == 0 || un >= sizeof(user) || pn == 0 || pn >= sizeof(pwd))
                return;  // ENV assenti → persistenza disabilitata (fail-safe)

            char connIn[512]{};
            std::snprintf(connIn, sizeof(connIn),
                "Driver={ODBC Driver 17 for SQL Server};"
                "Server=127.0.0.1;Database=PS_GameData;Uid=%s;Pwd=%s;",
                user, pwd);
            SQLSMALLINT outLen = 0;
            RETCODE rc = SQLDriverConnectA(g_hDbc, nullptr, (SQLCHAR*)connIn, SQL_NTS,
                                           nullptr, 0, &outLen, SQL_DRIVER_NOPROMPT);
            g_db_ok = (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO);
            SecureZeroMemory(connIn, sizeof(connIn));
            SecureZeroMemory(pwd,    sizeof(pwd));
        }

        void odbc_save(const SaveEntry& e)
        {
            if (!g_db_ok) return;
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
                return;
            // Upsert. WeaponSkinRW ha solo SELECT/INSERT/UPDATE → MERGE senza DELETE.
            static const SQLCHAR kSql[] =
                "MERGE dbo.CharWeaponSkins AS t "
                "USING (SELECT ? AS CharID, ? AS SkinType, ? AS SkinTypeId) AS s "
                "ON t.CharID = s.CharID "
                "WHEN MATCHED THEN UPDATE SET SkinType=s.SkinType, SkinTypeId=s.SkinTypeId, UpdatedAt=GETDATE() "
                "WHEN NOT MATCHED THEN INSERT (CharID,SkinType,SkinTypeId) VALUES (s.CharID,s.SkinType,s.SkinTypeId);";
            if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
            {
                SQLINTEGER cid = static_cast<SQLINTEGER>(e.charId);
                SQLLEN c0 = 0, c1 = 0, c2 = 0;
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG,    SQL_INTEGER,
                                 0, 0, &cid, 0, &c0);
                SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,
                                 0, 0, const_cast<uint8_t*>(&e.type),   0, &c1);
                SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_UTINYINT, SQL_TINYINT,
                                 0, 0, const_cast<uint8_t*>(&e.typeId), 0, &c2);
                RETCODE rc = SQLExecute(hStmt);
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

        LoadResult odbc_load(uint32_t charId)
        {
            LoadResult r{ false, 0, 0 };
            if (!g_db_ok) return r;
            SQLHSTMT hStmt = SQL_NULL_HSTMT;
            if (SQLAllocHandle(SQL_HANDLE_STMT, g_hDbc, &hStmt) != SQL_SUCCESS)
                return r;
            static const SQLCHAR kSql[] =
                "SELECT SkinType, SkinTypeId FROM dbo.CharWeaponSkins WHERE CharID = ?;";
            if (SQLPrepareA(hStmt, (SQLCHAR*)kSql, SQL_NTS) == SQL_SUCCESS)
            {
                SQLINTEGER cid = static_cast<SQLINTEGER>(charId);
                SQLLEN c0 = 0;
                SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
                                 0, 0, &cid, 0, &c0);
                RETCODE rc = SQLExecute(hStmt);
                if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
                {
                    if (SQLFetch(hStmt) == SQL_SUCCESS)
                    {
                        SQLCHAR t = 0, tid = 0;
                        SQLLEN lt = 0, ltid = 0;
                        SQLGetData(hStmt, 1, SQL_C_UTINYINT, &t,   0, &lt);
                        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &tid, 0, &ltid);
                        r.found  = true;
                        r.type   = static_cast<uint8_t>(t);
                        r.typeId = static_cast<uint8_t>(tid);
                    }
                }
                else if (rc == SQL_ERROR)
                {
                    g_db_ok = false;
                    SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
                    odbc_connect();
                    return r;
                }
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
            return r;
        }

        void worker_thread_fn()
        {
            odbc_connect();  // fino a 5s — OK, thread dedicato

            while (g_running.load())
            {
                std::unique_lock<std::mutex> lk(g_mtx);
                g_cv.wait(lk, [] {
                    return !g_save_queue.empty() || !g_load_queue.empty() || !g_running.load();
                });

                // Load ha priorità (un giocatore sta aspettando in login).
                while (!g_load_queue.empty())
                {
                    LoadEntry entry = std::move(g_load_queue.front());
                    g_load_queue.pop();
                    lk.unlock();
                    entry.result.set_value(odbc_load(entry.charId));
                    lk.lock();
                }

                while (!g_save_queue.empty())
                {
                    SaveEntry entry = g_save_queue.front();
                    g_save_queue.pop();
                    lk.unlock();
                    odbc_save(entry);
                    lk.lock();
                }
            }

            // Drain dei save residui alla chiusura (i load pendenti ricevono "not found").
            {
                std::unique_lock<std::mutex> lk(g_mtx);
                while (!g_load_queue.empty())
                {
                    LoadEntry entry = std::move(g_load_queue.front());
                    g_load_queue.pop();
                    entry.result.set_value(LoadResult{ false, 0, 0 });
                }
                while (!g_save_queue.empty())
                {
                    SaveEntry entry = g_save_queue.front();
                    g_save_queue.pop();
                    lk.unlock();
                    odbc_save(entry);
                    lk.lock();
                }
            }

            if (g_hDbc != SQL_NULL_HDBC) {
                SQLDisconnect(g_hDbc);
                SQLFreeHandle(SQL_HANDLE_DBC, g_hDbc);
            }
            if (g_hEnv != SQL_NULL_HENV)
                SQLFreeHandle(SQL_HANDLE_ENV, g_hEnv);
        }
    }

    void init()
    {
        if (g_running.load()) return;
        g_running.store(true);
        g_worker = std::thread(worker_thread_fn);
    }

    void shutdown()
    {
        if (!g_running.load()) return;
        g_running.store(false);
        g_cv.notify_all();
        if (g_worker.joinable())
            g_worker.join();
    }

    void save(uint32_t charId, uint8_t type, uint8_t typeId)
    {
        if (!g_running.load() || charId == 0) return;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_save_queue.push(SaveEntry{ charId, type, typeId });
        }
        g_cv.notify_one();
    }

    bool load(uint32_t charId, uint8_t& outType, uint8_t& outTypeId)
    {
        if (!g_running.load() || charId == 0)
            return false;

        std::future<LoadResult> fut;
        {
            LoadEntry entry;
            entry.charId = charId;
            fut = entry.result.get_future();
            std::lock_guard<std::mutex> lk(g_mtx);
            g_load_queue.push(std::move(entry));
        }
        g_cv.notify_one();

        // Bounded wait: non bloccare il thread di login più di 2s.
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            return false;

        LoadResult r = fut.get();
        if (!r.found)
            return false;
        outType   = r.type;
        outTypeId = r.typeId;
        return true;
    }
}
