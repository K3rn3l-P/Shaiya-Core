#include "include/main.h"
#include "src/include/GmCommandNative.h"

int dispatch_native_gm_command(void* chatWindow, char* text)
{
    return shaiya::gm::dispatch(chatWindow, text);
}

bool is_native_gm_command_catalogued(const char* text)
{
    return shaiya::gm::is_catalogued_command(text);
}
