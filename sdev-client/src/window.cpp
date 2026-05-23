#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <util/util.h>
#include <imm.h>
#include <array>
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <shaiya/include/network/game/incoming/0800.h>
#include "include/main.h"
#include "include/shaiya/CNetwork.h"
#include "include/shaiya/CQuickSlot.h"
#include "include/shaiya/Static.h" // g_var client/window state
#include "include/shaiya/Unknown.h"

using namespace shaiya;
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace window
{
    namespace config {
        const char* ping_ip = "100.95.179.88"; // IP local do servidor do jogo
        const int server_port = 30800;
        const wchar_t* title_base = L"Shaiya Duff";
        char* char_name_ptr = reinterpret_cast<char*>(0x09144CE);

        const std::vector<std::wstring> custom_messages = {
            L"Enjoy the game!",
            L"Follow us on social media!",
            L"Check out our website!",
            L"Join our community!",
            L"Participate in events!",
            L"Share your achievements!",
            L"Invite your friends!",
            L"Look Roulette!",
            L"Show Rewards!"
        };
    }

    inline HWND g_hookedGameHwnd = nullptr;
    inline WNDPROC g_originalGameWndProc = nullptr;
    inline bool g_hwndIsUnicode = false;
    inline DWORD g_nextTitleRefreshTick = 0;

    inline std::atomic<int> g_message_index{ 0 };
    wchar_t g_dynamicTitleBuffer[256] = L"Shaiya";
    std::atomic<bool> g_winsock_initialized{ false };

    bool InitWinsockOnce() {
        bool expected = false;
        if (g_winsock_initialized.compare_exchange_strong(expected, true)) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                g_winsock_initialized.store(false);
                return false;
            }
        }
        return true;
    }

    std::pair<int, int> GetNetworkStats(const char* ip, int port) {
        if (!InitWinsockOnce()) return { -1, 100 };
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return { -1, 100 };

        u_long mode = 1; ioctlsocket(sock, FIONBIO, &mode);
        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, ip, &server.sin_addr);

        auto start = std::chrono::steady_clock::now();
        connect(sock, (SOCKADDR*)&server, sizeof(server));

        fd_set writefds; FD_ZERO(&writefds); FD_SET(sock, &writefds);
        timeval timeout{ 1, 0 }; // 1s timeout
        if (select(0, nullptr, &writefds, nullptr, &timeout) <= 0) {
            closesocket(sock); return { -1, 100 };
        }

        auto end = std::chrono::steady_clock::now();
        int ping = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        closesocket(sock);
        return { ping, 0 };
    }

    const char* SafeReadCharName(char* ptr) {
        if (!ptr || IsBadReadPtr(ptr, 1) || ptr[0] == '\0') return nullptr;
        return ptr;
    }

    void UpdateWindowTitleLoop() {
        while (true) {
            auto [ping, loss] = GetNetworkStats(config::ping_ip, config::server_port);
            std::wstring ping_text = (ping >= 0) ? std::to_wstring(ping) : L"--";
            int msg_idx = (g_message_index.fetch_add(1)) % config::custom_messages.size();
            std::wstring dyn_text = config::custom_messages[msg_idx];
            const char* safe_name = SafeReadCharName(config::char_name_ptr);
            wchar_t w_char_name[32]{};

            if (safe_name) {
                if (MultiByteToWideChar(CP_UTF8, 0, safe_name, -1, w_char_name, 32) <= 0) {
                    MultiByteToWideChar(CP_ACP, 0, safe_name, -1, w_char_name, 32);
                }
            }
            else {
                wcscpy_s(w_char_name, L"No character selected");
            }

            swprintf(g_dynamicTitleBuffer, 256, L"%s - Playing as %s | ping/ms: %s | %s",
                config::title_base, w_char_name, ping_text.c_str(), dyn_text.c_str());

            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }

    void refresh_game_window_title(HWND hwnd, bool force)
    {
        if (!hwnd || !g_hwndIsUnicode)
            return;

        auto now = GetTickCount();
        if (!force && now < g_nextTitleRefreshTick)
            return;

        g_nextTitleRefreshTick = now + 1000;
        DefWindowProcW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(g_dynamicTitleBuffer));
    }

    void copy_composition_string(HIMC context, DWORD index, wchar_t(&output)[64])
    {
        output[0] = L'\0';
        if (!context)
            return;

        auto bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
        if (bytes <= 0)
            return;

        auto bytesToCopy = std::min<LONG>(bytes, static_cast<LONG>(sizeof(output) - sizeof(wchar_t)));
        auto copied = ImmGetCompositionStringW(context, index, output, static_cast<DWORD>(bytesToCopy));
        if (copied <= 0)
        {
            output[0] = L'\0';
            return;
        }

        auto chars = copied / static_cast<LONG>(sizeof(wchar_t));
        output[std::min<std::size_t>(chars, std::size(output) - 1)] = L'\0';
    }

    bool inject_unicode_textbox_input(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (IsWindowUnicode(hwnd) && msg == WM_CHAR && wParam == VK_BACK)
        {
            auto textBox = &g_var->input.textBox;
            if (backspace_utf8_textbox_char(textBox))
                return true;
        }

        if (msg == WM_CHAR && wParam > 0x7F)
        {
            auto textBox = &g_var->input.textBox;
            return append_utf8_textbox_wchar(textBox, static_cast<wchar_t>(wParam));
        }

        if (msg != WM_IME_COMPOSITION || (lParam & GCS_RESULTSTR) == 0)
            return false;

        auto context = ImmGetContext(hwnd);
        if (!context)
            return false;

        wchar_t result[64]{};
        copy_composition_string(context, GCS_RESULTSTR, result);
        ImmReleaseContext(hwnd, context);

        if (result[0] == L'\0')
            return false;

        auto textBox = &g_var->input.textBox;

        for (auto* current = result; *current; ++current)
        {
            append_utf8_textbox_wchar(textBox, *current);
        }

        sync_textbox_utf8_display(textBox);
        return true;
    }

    void assign_windows1(Unknown* unknown)
    {
        g_uiRoot1 = unknown;
        auto quickSlot3 = g_pQuickSlot3;
        unknown->windows1.quickSlot3 = quickSlot3;
    }

    void assign_windows2(Unknown* unknown)
    {
        g_uiRoot2 = unknown;
        auto quickSlot3 = g_pQuickSlot3;
        unknown->windows2.quickSlot3 = quickSlot3;
    }

    LRESULT CALLBACK game_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        LRESULT imguiResult = 0;
        if (handle_imgui_layer_wnd_proc(hwnd, msg, wParam, lParam, imguiResult))
            return imguiResult;

        if (msg != WM_SETTEXT)
            refresh_game_window_title(hwnd, false);

        if (inject_unicode_textbox_input(hwnd, msg, wParam, lParam))
            return 0;

        if (msg == kClientRouletteListWindowMessage)
        {
            GameRouletteListIncoming outgoing{};
            CNetwork::Send(&outgoing, sizeof(outgoing));
            return 0;
        }

        if (msg == kClientRouletteRollWindowMessage)
        {
            GameRouletteSpinIncoming outgoing{};
            CNetwork::Send(&outgoing, sizeof(outgoing));
            return 0;
        }

        if (msg == kClientEmojiTokenWindowMessage)
        {
            auto token = reinterpret_cast<const char*>(lParam);
            append_utf8_textbox_text(&g_var->input.textBox, token);
            return 0;
        }

        if (g_hwndIsUnicode)
            return CallWindowProcW(g_originalGameWndProc, hwnd, msg, wParam, lParam);

        return CallWindowProcA(g_originalGameWndProc, hwnd, msg, wParam, lParam);
    }

    void ensure_client_sysmsg_dispatch_ready()
    {
        auto hwnd = g_var->hwnd;
        if (!hwnd || !IsWindow(hwnd))
            return;

        if (g_hookedGameHwnd == hwnd && g_originalGameWndProc)
            return;

        auto previousProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(game_wnd_proc)));
        if (!previousProc)
            return;

        g_originalGameWndProc = previousProc;
        g_hookedGameHwnd = hwnd;
        g_hwndIsUnicode = IsWindowUnicode(hwnd) != FALSE;

        static std::once_flag titleThreadFlag;
        std::call_once(titleThreadFlag, []() {
            std::thread(UpdateWindowTitleLoop).detach();
            });

        refresh_game_window_title(hwnd, true);
    }

    bool is_client_sysmsg_dispatch_ready()
    {
        auto hwnd = g_var->hwnd;
        return hwnd
            && IsWindow(hwnd)
            && g_hookedGameHwnd == hwnd
            && g_originalGameWndProc != nullptr;
    }
}

unsigned u0x42B826 = 0x42B826;
void __declspec(naked) naked_0x42B820()
{
    __asm
    {
        mov[esi + 0x2A0], ecx
        pushad
        push esi
        call window::assign_windows1
        add esp, 0x4
        popad
        jmp u0x42B826
    }
}

unsigned u0x42B9D7 = 0x42B9D7;
void __declspec(naked) naked_0x42B9D1()
{
    __asm
    {
        mov[esi + 0x370], ecx
        pushad
        push esi
        call window::assign_windows2
        add esp, 0x4
        popad
        jmp u0x42B9D7
    }
}

void hook::window()
{
    util::detour((void*)0x42B820, naked_0x42B820, 6);
    util::detour((void*)0x42B9D1, naked_0x42B9D1, 6);
}

void ensure_client_sysmsg_dispatch_ready()
{
    window::ensure_client_sysmsg_dispatch_ready();
}

bool is_client_sysmsg_dispatch_ready()
{
    return window::is_client_sysmsg_dispatch_ready();
}