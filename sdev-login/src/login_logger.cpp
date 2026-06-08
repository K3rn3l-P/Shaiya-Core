#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <mutex>
#include "include/login_logger.h"

namespace login_logger
{
    namespace
    {
        static std::mutex g_mtx;
        static constexpr const char* kLogDir = "C:\\ShaiyaServer\\Logs";

        static void write_line(const char* level,
                                const char* ip,
                                const char* user,
                                const char* reason)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);

            CreateDirectoryA(kLogDir, nullptr);  // no-op se esiste già

            char path[MAX_PATH];
            std::snprintf(path, sizeof(path), "%s\\login_%04d-%02d-%02d.log",
                          kLogDir, st.wYear, st.wMonth, st.wDay);

            char line[512];
            int len = std::snprintf(line, sizeof(line),
                "[%02d:%02d:%02d] %-4s | ip=%-15s | user=%-20s | %s\r\n",
                st.wHour, st.wMinute, st.wSecond,
                level,
                ip     ? ip     : "?",
                user   ? user   : "-",
                reason ? reason : "");
            if (len <= 0) return;

            std::lock_guard<std::mutex> lk(g_mtx);
            HANDLE hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return;
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(hFile, line, static_cast<DWORD>(len), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    void log_fail(const char* ip, const char* user, const char* reason)
    {
        write_line("FAIL", ip, user, reason);
    }

    void log_ok(const char* ip, const char* user)
    {
        write_line("OK  ", ip, user, "LOGIN_SUCCESS");
    }
}
