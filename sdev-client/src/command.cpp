#include <ranges>
#include <atomic>
#include <cctype>
#include <charconv>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <util/util.h>
#include "include/main.h"
#include "include/custom_chat.h"
#include "include/shaiya/CCharacter.h"
#include "include/shaiya/CEffectData.h"
#include "include/shaiya/Static.h"
using namespace shaiya;

namespace
{
    constexpr const char* kFontIniSection = "FONT";
    constexpr const char* kFontHeightKey = "HEIGHT";
    constexpr const char* kFontWeightKey = "WEIGHT";
    constexpr const char* kFontItalicKey = "ITALIC";
    constexpr const char* kFontFaceNameKey = "FACENAME";
    constexpr int kCustomEftEffectDataId = 299;
    constexpr int kCustomEftEffectSubId = 1;
    constexpr DWORD kDefaultCustomEftLoopIntervalMs = 1200;
    constexpr DWORD kMinCustomEftLoopIntervalMs = 100;
    constexpr DWORD kMaxCustomEftLoopIntervalMs = 10000;
    constexpr const char* kEffectFolders[] = {
        "data/effect",
        "Data\\Effect",
        "effect",
        "Effect"
    };
    constexpr int kDefaultFontHeight = 13;
    constexpr const char* kDefaultFontFaceName = "Arial";

    inline std::atomic_bool g_chooseFontWorkerBusy = false;
    inline std::atomic_bool g_customEftLoopEnabled = false;
    inline std::atomic<DWORD> g_customEftLoopIntervalMs = kDefaultCustomEftLoopIntervalMs;
    inline std::atomic<DWORD> g_customEftLoopNextTick = 0;
    inline std::string g_customEftName;

    HRESULT create_d3dx_font(
        LPDIRECT3DDEVICE9 device,
        int height,
        int weight,
        BOOL italic,
        const char* faceName,
        LPD3DXFONT* outFont)
    {
        if (!device || !outFont)
            return E_FAIL;

        auto d3dx9 = GetModuleHandleA("d3dx9_43.dll");
        if (!d3dx9)
            d3dx9 = LoadLibraryA("d3dx9_43.dll");

        if (!d3dx9)
            return E_FAIL;

        typedef HRESULT(WINAPI* LPFN)(
            LPDIRECT3DDEVICE9,
            INT,
            UINT,
            UINT,
            UINT,
            BOOL,
            DWORD,
            DWORD,
            DWORD,
            DWORD,
            LPCSTR,
            LPD3DXFONT*);

        auto createFont = reinterpret_cast<LPFN>(GetProcAddress(d3dx9, "D3DXCreateFontA"));
        if (!createFont)
            return E_FAIL;

        return createFont(
            device,
            height,
            0,
            weight,
            1,
            italic,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            faceName,
            outFont);
    }

    bool apply_game_font(int height, int weight, BOOL italic, const char* faceName)
    {
        if (!g_var->camera.device || !faceName || !faceName[0])
            return false;

        auto newHFont = CreateFontA(
            height,
            0,
            0,
            0,
            weight,
            italic,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            faceName);

        if (!newHFont)
            return false;

        auto oldHFont = g_var->camera.hFont;
        g_var->camera.hFont = newHFont;
        if (oldHFont)
            DeleteObject(oldHFont);

        auto swapD3dxFont = [&](LPD3DXFONT& slot) {
            LPD3DXFONT newFont = nullptr;
            if (FAILED(create_d3dx_font(g_var->camera.device, height, weight, italic, faceName, &newFont)))
                return;

            auto oldFont = slot;
            slot = newFont;
            if (oldFont)
                oldFont->Release();
        };

        // The client renders text through several camera font slots. Updating
        // all of them makes /font affect UI labels, counters, chat-adjacent
        // overlays, and the native text helpers instead of only one subset.
        swapD3dxFont(g_var->camera.d3dxFont3);
        swapD3dxFont(g_var->camera.d3dxFont2);
        swapD3dxFont(g_var->camera.d3dxFont1);
        swapD3dxFont(g_var->camera.d3dxFont0);

        return true;
    }

    void save_game_font(int height, int weight, BOOL italic, const char* faceName)
    {
        WritePrivateProfileStringA(kFontIniSection, kFontHeightKey, std::to_string(height).c_str(), g_var->iniFileName.data());
        WritePrivateProfileStringA(kFontIniSection, kFontWeightKey, std::to_string(weight).c_str(), g_var->iniFileName.data());
        WritePrivateProfileStringA(kFontIniSection, kFontItalicKey, std::to_string(italic ? 1 : 0).c_str(), g_var->iniFileName.data());
        WritePrivateProfileStringA(kFontIniSection, kFontFaceNameKey, faceName, g_var->iniFileName.data());
    }

    void load_game_font_config()
    {
        auto height = GetPrivateProfileIntA(kFontIniSection, kFontHeightKey, kDefaultFontHeight, g_var->iniFileName.data());
        auto weight = GetPrivateProfileIntA(kFontIniSection, kFontWeightKey, FW_NORMAL, g_var->iniFileName.data());
        auto italic = GetPrivateProfileIntA(kFontIniSection, kFontItalicKey, FALSE, g_var->iniFileName.data()) ? TRUE : FALSE;

        std::string faceName(LF_FACESIZE, '\0');
        GetPrivateProfileStringA(kFontIniSection, kFontFaceNameKey, kDefaultFontFaceName, faceName.data(), static_cast<DWORD>(faceName.size()), g_var->iniFileName.data());
        faceName.resize(std::strlen(faceName.c_str()));

        apply_game_font(height, weight, italic, faceName.c_str());
    }

    void wait_and_load_game_font_config()
    {
        for (int attempt = 0; attempt < 200; ++attempt)
        {
            if (g_var->camera.device)
            {
                load_game_font_config();
                return;
            }

            Sleep(100);
        }
    }

    void choose_font_worker()
    {
        g_chooseFontWorkerBusy.store(true);

        LOGFONTA lf{};
        lf.lfHeight = GetPrivateProfileIntA(kFontIniSection, kFontHeightKey, kDefaultFontHeight, g_var->iniFileName.data());
        lf.lfWeight = GetPrivateProfileIntA(kFontIniSection, kFontWeightKey, FW_NORMAL, g_var->iniFileName.data());
        lf.lfItalic = GetPrivateProfileIntA(kFontIniSection, kFontItalicKey, FALSE, g_var->iniFileName.data()) ? TRUE : FALSE;
        lf.lfCharSet = DEFAULT_CHARSET;
        GetPrivateProfileStringA(kFontIniSection, kFontFaceNameKey, kDefaultFontFaceName, lf.lfFaceName, LF_FACESIZE, g_var->iniFileName.data());

        CHOOSEFONTA cf{};
        cf.lStructSize = sizeof(CHOOSEFONTA);
        cf.hwndOwner = g_var->hwnd;
        cf.lpLogFont = &lf;
        cf.Flags = CF_INITTOLOGFONTSTRUCT
            | CF_FORCEFONTEXIST
            | CF_LIMITSIZE
            | CF_SCREENFONTS
            | CF_NOSCRIPTSEL
            | CF_NOSIMULATIONS
            | CF_NOVERTFONTS;
        cf.nSizeMin = 8;
        cf.nSizeMax = 14;

        if (ChooseFontA(&cf))
        {
            auto italic = cf.lpLogFont->lfItalic ? TRUE : FALSE;
            if (apply_game_font(cf.lpLogFont->lfHeight, cf.lpLogFont->lfWeight, italic, cf.lpLogFont->lfFaceName))
                save_game_font(cf.lpLogFont->lfHeight, cf.lpLogFont->lfWeight, italic, cf.lpLogFont->lfFaceName);
        }

        g_chooseFontWorkerBusy.store(false);
    }

    void apply_effects_setting(bool enabled)
    {
        g_showEffects = enabled;
        g_showMobEffects = enabled;
        WritePrivateProfileStringA("ADVANCED", "EFFECTS", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
        if (!enabled)
            name_color::disable_enchant_eft();
    }

    void apply_pets_setting(bool enabled)
    {
        g_showMobEffects = enabled;
        g_showPets = enabled;
        WritePrivateProfileStringA("ADVANCED", "PETS", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
    }

    void apply_wings_setting(bool enabled)
    {
        g_showWings = enabled;
        WritePrivateProfileStringA("ADVANCED", "WINGS", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
    }

    void apply_fpsboost_setting(bool enabled)
    {
        g_fpsBoost = enabled ? 1 : 0;
        WritePrivateProfileStringA("ADVANCED", "FPS_BOOST", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
    }

    void apply_titles_setting(bool enabled)
    {
        g_showTitles = enabled;
        WritePrivateProfileStringA("ADVANCED", "TITLES", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
    }

    void apply_colour_setting(bool enabled)
    {
        g_showNameColors = enabled;
        WritePrivateProfileStringA("ADVANCED", "COLOUR", enabled ? "TRUE" : "FALSE", g_var->iniFileName.data());
    }

    void write_command_message(const std::string& text)
    {
        Static::DrawText_ChatBox(ChatType::Notice26, text.c_str(), 0);
    }

    bool contains_path_separator_or_parent_ref(const std::string& value)
    {
        return value.find('\\') != std::string::npos
            || value.find('/') != std::string::npos
            || value.find("..") != std::string::npos;
    }

    std::string normalize_eft_name(const std::string& rawName)
    {
        auto name = rawName;
        if (name.size() >= 4)
        {
            auto ext = name.substr(name.size() - 4);
            for (auto& ch : ext)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            if (ext == ".eft")
                name.resize(name.size() - 4);
        }

        return name;
    }

    bool try_parse_loop_interval(const std::string& text, DWORD& outIntervalMs)
    {
        unsigned value = 0;
        auto begin = text.data();
        auto end = text.data() + text.size();
        auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end)
            return false;

        if (value < kMinCustomEftLoopIntervalMs)
            value = kMinCustomEftLoopIntervalMs;
        else if (value > kMaxCustomEftLoopIntervalMs)
            value = kMaxCustomEftLoopIntervalMs;

        outIntervalMs = value;
        return true;
    }

    void remove_custom_eft()
    {
        auto user = g_pWorldMgr->user;
        if (!user)
            return;

        CCharacter::RemoveEffect(user, kCustomEftEffectDataId, kCustomEftEffectSubId);
    }

    bool render_custom_eft()
    {
        auto user = g_pWorldMgr->user;
        if (!user)
            return false;

        remove_custom_eft();
        CCharacter::RenderEffect(
            user,
            kCustomEftEffectDataId,
            kCustomEftEffectSubId,
            0.0f,
            &user->pos,
            &user->dir,
            &user->up,
            0);
        return true;
    }

    void stop_custom_eft_loop()
    {
        g_customEftLoopEnabled.store(false);
        g_customEftLoopNextTick.store(0);
    }

    void start_custom_eft_loop(DWORD intervalMs)
    {
        g_customEftLoopIntervalMs.store(intervalMs);
        g_customEftLoopEnabled.store(true);
        g_customEftLoopNextTick.store(GetTickCount() + intervalMs);
    }

    void update_custom_eft_loop()
    {
        if (!g_customEftLoopEnabled.load())
            return;

        auto nextTick = g_customEftLoopNextTick.load();
        auto now = GetTickCount();
        if (static_cast<int32_t>(now - nextTick) < 0)
            return;

        if (render_custom_eft())
            g_customEftLoopNextTick.store(now + g_customEftLoopIntervalMs.load());
    }

    bool play_custom_eft(const std::string& rawName, bool loop, DWORD loopIntervalMs)
    {
        auto user = g_pWorldMgr->user;
        if (!user)
        {
            write_command_message("[eft] Character is not ready yet.");
            return false;
        }

        auto name = normalize_eft_name(rawName);
        if (name.empty())
        {
            write_command_message("[eft] Usage: /eft <fileName> [loop [ms]] or /eft off");
            return false;
        }

        if (contains_path_separator_or_parent_ref(name))
        {
            write_command_message("[eft] Only plain file names are allowed.");
            return false;
        }

        auto fileName = name + ".eft";
        stop_custom_eft_loop();
        remove_custom_eft();

        const char* loadedFrom = nullptr;
        for (auto path : kEffectFolders)
        {
            if (CEffectData::CreateFromFile(&g_var->effectData299, path, fileName.c_str()))
            {
                loadedFrom = path;
                break;
            }
        }

        if (!loadedFrom)
        {
            g_customEftName.clear();
            write_command_message("[eft] Failed to load: " + fileName);
            return false;
        }

        g_customEftName = fileName;
        render_custom_eft();

        if (loop)
        {
            start_custom_eft_loop(loopIntervalMs);
            write_command_message("[eft] Loaded loop: " + fileName + " from " + loadedFrom + " (" + std::to_string(loopIntervalMs) + " ms)");
        }
        else
        {
            write_command_message("[eft] Loaded: " + fileName + " from " + loadedFrom);
        }
        return true;
    }
}

bool play_custom_eft_from_ui(const char* fileName, bool loop, unsigned loopIntervalMs)
{
    if (!fileName)
        return false;

    return play_custom_eft(fileName, loop, loopIntervalMs);
}

void stop_custom_eft_from_ui()
{
    stop_custom_eft_loop();
    remove_custom_eft();
    g_customEftName.clear();
}

bool is_custom_eft_loop_enabled_from_ui()
{
    return g_customEftLoopEnabled.load();
}

unsigned get_custom_eft_loop_interval_from_ui()
{
    return g_customEftLoopIntervalMs.load();
}

const char* get_custom_eft_name_from_ui()
{
    return g_customEftName.c_str();
}

void load_advanced_config()
{
    std::string str(MAX_PATH, 0);
    GetPrivateProfileStringA("ADVANCED", "COSTUMES", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showCostumes = str.compare(0, 4, "TRUE") == 0;

    GetPrivateProfileStringA("ADVANCED", "WINGS", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showWings = str.compare(0, 4, "TRUE") == 0;

    GetPrivateProfileStringA("ADVANCED", "EFFECTS", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showEffects = str.compare(0, 4, "TRUE") == 0;

    GetPrivateProfileStringA("ADVANCED", "PETS", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showPets = str.compare(0, 4, "TRUE") == 0;
    g_showMobEffects = g_showPets;

    GetPrivateProfileStringA("ADVANCED", "FPS_BOOST", "FALSE", str.data(), str.size(), g_var->iniFileName.data());
    g_fpsBoost = str.compare(0, 4, "TRUE") == 0;

    GetPrivateProfileStringA("ADVANCED", "TITLES", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showTitles = str.compare(0, 4, "TRUE") == 0;

    GetPrivateProfileStringA("ADVANCED", "COLOUR", "TRUE", str.data(), str.size(), g_var->iniFileName.data());
    g_showNameColors = str.compare(0, 4, "TRUE") == 0;
}

int command_handler(char* text)
{
    std::string input(text);
    if (!input.starts_with('/'))
        return 1;

    auto rng = std::views::split(input, ' ');
    auto argv = std::ranges::to<std::vector<std::string>>(rng);
    auto argc = argv.size();

    if (!argc)
    {
        Static::SysMsgToChatBox(ChatType::Acquire31, 253, 12);
        return 0;
    }

    if (input == "/effects on")
    {
        apply_effects_setting(true);
        return 0;
    }

    if (input == "/effects off")
    {
        apply_effects_setting(false);
        return 0;
    }

    if (input == "/pets on")
    {
        apply_pets_setting(true);
        return 0;
    }

    if (input == "/pets off")
    {
        apply_pets_setting(false);
        return 0;
    }

    if (argv[0] == "/eft")
    {
        if (argc < 2)
        {
            write_command_message("[eft] Usage: /eft <fileName> [loop [ms]] or /eft off");
            return 0;
        }

        if (argv[1] == "off")
        {
            stop_custom_eft_loop();
            remove_custom_eft();
            g_customEftName.clear();
            write_command_message("[eft] Removed custom effect.");
            return 0;
        }

        if (argv[1] == "on")
        {
            write_command_message("[eft] Usage: /eft <fileName> [loop [ms]] or /eft off");
            return 0;
        }

        auto loop = false;
        auto loopIntervalMs = kDefaultCustomEftLoopIntervalMs;

        if (argc >= 3)
        {
            if (argv[2] != "loop")
            {
                write_command_message("[eft] Usage: /eft <fileName> [loop [ms]] or /eft off");
                return 0;
            }

            loop = true;

            if (argc >= 4 && !try_parse_loop_interval(argv[3], loopIntervalMs))
            {
                write_command_message("[eft] Loop ms must be a number between 100 and 10000.");
                return 0;
            }
        }

        play_custom_eft(argv[1], loop, loopIntervalMs);
        return 0;
    }

    if (input == "/wings on")
    {
        apply_wings_setting(true);
        return 0;
    }

    if (input == "/wings off")
    {
        apply_wings_setting(false);
        return 0;
    }

    if (input == "/costumes on")
    {
        g_showCostumes = true;
        WritePrivateProfileStringA("ADVANCED", "COSTUMES", "TRUE", g_var->iniFileName.data());
        return 0;
    }

    if (input == "/costumes off")
    {
        g_showCostumes = false;
        WritePrivateProfileStringA("ADVANCED", "COSTUMES", "FALSE", g_var->iniFileName.data());
        return 0;
    }

    if (input == "/fpsboost on")
    {
        apply_fpsboost_setting(true);
        return 0;
    }

    if (input == "/fpsboost off")
    {
        apply_fpsboost_setting(false);
        return 0;
    }

    if (input == "/hpbars on")
    {
        g_showPlayerHpBar = true;
        return 0;
    }

    if (input == "/hpbars off")
    {
        g_showPlayerHpBar = false;
        return 0;
    }

    if (input == "/font")
    {
        if (g_chooseFontWorkerBusy.load())
            return 0;

        std::thread(choose_font_worker).detach();
        return 0;
    }

    if (input == "/titles on")
    {
        apply_titles_setting(true);
        return 0;
    }

    if (input == "/titles off")
    {
        apply_titles_setting(false);
        return 0;
    }

    if (input == "/colour on" || input == "/color on")
    {
        apply_colour_setting(true);
        return 0;
    }

    if (input == "/colour off" || input == "/color off")
    {
        apply_colour_setting(false);
        return 0;
    }

    // /mute PlayerName — permanently block all messages from that player
    if (argv[0] == "/mute" && argc >= 2)
    {
        custom_chat::mute_player(argv[1].c_str());
        return 0;
    }

    // /unmute PlayerName — remove player from the mute list
    if (argv[0] == "/unmute" && argc >= 2)
    {
        custom_chat::unmute_player(argv[1].c_str());
        return 0;
    }

    return 1;
}

unsigned u0x406110 = 0x406110;
unsigned u0x4094B2 = 0x4094B2;
void __declspec(naked) naked_0x4094AD()
{
    __asm 
    {
        pushad

        call load_advanced_config

        popad

        // original
        call u0x406110
        jmp u0x4094B2
    }
}

unsigned u0x4867A6 = 0x4867A6;
unsigned u0x487532 = 0x487532;
void __declspec(naked) naked_0x4867A1() 
{
    __asm 
    {
        pushad

        push edi
        call command_handler
        add esp,0x4
        test eax,eax

        popad

        je _0x487532

        // original
        push 0x13D4
        jmp u0x4867A6

        _0x487532:
        jmp u0x487532
    }
}

unsigned u0x41634D = 0x41634D;
void __declspec(naked) naked_0x416343()
{
    __asm 
    {
        cmp dword ptr[g_showCostumes],0x1
        jne label

        // original
        mov dword ptr[esi+0xAC],0x1
        jmp u0x41634D

        label:
        mov dword ptr[esi+0xAC],0x0
        jmp u0x41634D
    }
}

unsigned u0x59F092 = 0x59F092;
unsigned u0x59F4BA = 0x59F4BA;
void __declspec(naked) naked_0x59F08B()
{
    __asm 
    {
        movzx ecx,byte ptr[edi+0x7]
        cmp ecx,0x96
        je label

        original:
        mov eax,[edi+0x2]
        jmp u0x59F092

        label:
        cmp dword ptr[g_showCostumes],0x1
        jne _0x59F4BA
        jmp original

        _0x59F4BA:
        jmp u0x59F4BA
    }
}

unsigned u0x459127 = 0x459127;
void __declspec(naked) naked_0x459120() 
{
    __asm 
    {
        cmp dword ptr[g_showEffects],0x1
        je _original

        retn 0x18

        _original:
        mov eax,[esp+0x4]
        sub esp,0x10
        jmp u0x459127
    }
}

unsigned u0x43A307 = 0x43A307;
void __declspec(naked) naked_0x43A300() 
{
    __asm 
    {
        cmp dword ptr[g_showEffects],0x1
        je _original

        retn 0x18

        _original:
        mov eax,[esp+0x4]
        sub esp,0x10
        jmp u0x43A307
    }
}

unsigned u0x41A2C7 = 0x41A2C7;
void __declspec(naked) naked_0x41A2C0() 
{
    __asm 
    {
        cmp dword ptr[g_showEffects],0x1
        je _original

        retn 0x1C

        _original:
        mov eax,[esp+0x4]
        sub esp,0x10
        jmp u0x41A2C7
    }
}

unsigned u0x43A2FA = 0x43A2FA;
unsigned u0x43A147 = 0x43A147;
void __declspec(naked) naked_0x43A142()
{
    __asm 
    {
        cmp dword ptr[g_showMobEffects],0x0
        je _0x43A2FA
        cmp dword ptr[g_showEffects],0x1
        je _original

        _0x43A2FA:
        jmp u0x43A2FA

        _original:
        mov edi,ecx
        mov edx,[edi+0x30]
        jmp u0x43A147
    }
}

unsigned u0x418305 = 0x418305;
unsigned u0x4184CF = 0x4184CF;
void __declspec(naked) naked_0x4182FF() 
{
    __asm 
    {
        // original
        mov eax,[ebx+0x430]

        cmp dword ptr[g_showPets],0x1
        jne _0x4184CF
        jmp u0x418305

        _0x4184CF:
        jmp u0x4184CF
    }
}

unsigned u0x41F81D = 0x41F81D;
unsigned u0x41F9ED = 0x41F9ED;
void __declspec(naked) naked_0x41F816() 
{
    __asm 
    {
         // original
        mov eax,[esi+0x434]
        push edi

        cmp dword ptr[g_showWings],0x1
        jne _0x41F9ED
        jmp u0x41F81D

        _0x41F9ED:
        jmp u0x41F9ED
    }
}

unsigned u0x580D36 = 0x580D36;
unsigned u0x580DCE = 0x580DCE;
void __declspec(naked) naked_0x580D30()
{
    __asm
    {
        pushad
        call update_custom_eft_loop
        popad

        cmp dword ptr [g_fpsBoost], 1
        je enabled

        push ebx
        mov ebx, 0x007B19B0
        jmp u0x580D36

        enabled:
        jmp u0x580DCE
    }
}

void hook::command()
{
    std::thread(wait_and_load_game_font_config).detach();

    // get client config
    util::detour((void*)0x4094AD, naked_0x4094AD, 5);
    // commands
    util::detour((void*)0x4867A1, naked_0x4867A1, 5);
    // show or hide costumes
    util::detour((void*)0x59F08B, naked_0x59F08B, 7);
    util::detour((void*)0x416343, naked_0x416343, 10);
    // shen1l's effect on/off
    util::detour((void*)0x459120, naked_0x459120, 7);
    util::detour((void*)0x43A300, naked_0x43A300, 7);
    util::detour((void*)0x41A2C0, naked_0x41A2C0, 7);
    // show or hide pets
    util::detour((void*)0x4182FF, naked_0x4182FF, 6);
    // show or hide wings
    util::detour((void*)0x41F816, naked_0x41F816, 7);
    // show or hide mob effects
    util::detour((void*)0x43A142, naked_0x43A142, 5);
    // fps boost
    util::detour((void*)0x580D30, naked_0x580D30, 6);
}
