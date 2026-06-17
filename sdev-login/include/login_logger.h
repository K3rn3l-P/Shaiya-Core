#pragma once

namespace login_logger
{
    // Thread-safe append to C:\ShaiyaServer\Logs\login_YYYY-MM-DD.log.
    void log_fail(const char* ip, const char* user, const char* reason);
    void log_ok  (const char* ip, const char* user);
}
