#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "include/main.h"
#include "include/security_logger.h"
#include "include/weapon_skin_db.h"

void DllExport()
{
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_DETACH:
        weapon_skin_db::shutdown();
        security_logger::shutdown();
        break;
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        Main();
        break;
    case DLL_THREAD_DETACH:
    case DLL_THREAD_ATTACH:
        break;
    }

    return TRUE;
}
