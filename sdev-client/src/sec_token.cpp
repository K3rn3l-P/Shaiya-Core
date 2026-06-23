#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include "../include/main.h"

namespace sec_token
{
    namespace
    {
        static char g_token[65] = {};  // 64 hex chars + NUL
        static bool g_loaded = false;

        static void load()
        {
            if (g_loaded) return;
            g_loaded = true;
            char buf[72]{};
            DWORD n = GetEnvironmentVariableA("SDEV_SEC_TOKEN", buf, sizeof(buf));
            SetEnvironmentVariableA("SDEV_SEC_TOKEN", nullptr);  // cancella subito
            if (n == 64)
                memcpy(g_token, buf, 64);
            SecureZeroMemory(buf, sizeof(buf));
        }
    }

    const char* get()
    {
        load();
        return g_token;
    }
}
