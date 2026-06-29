// auto_login.cpp — auto-login IN-PROCESS (sostituisce l'injection cross-process
// dell'updater, eliminando la race col DoLogin per-frame).
//
// L'updater passa username/password via env (SDEV_LOGIN_USER / SDEV_LOGIN_PW),
// letti e azzerati subito (pattern one-shot come sec_token). Ad ogni frame, se la
// schermata di login e' attiva, scrive le credenziali nell'oggetto CLogin e applica
// i 3 patch del login-flow (gli stessi byte dell'updater, verificati con Ghidra su
// game.exe). Girando sul thread del gioco non c'e' race. I patch vengono ripristinati
// al momento del send 0xA101 (hook in packet.cpp -> auto_login_after_send).
//
// Offset/byte verificati (Ghidra, game.exe, 2026-06-26):
//   CLogin ptr      = *(0x007C48FC)        vtable atteso = 0x0074FFC0
//   userId field    = [obj + 4]   (32 byte)
//   password field  = [obj + 0x27](32 byte)
//   PatchLoginFlow1 @ 0x004D4EBF : E9 54 00 00 00 90*5  (skip check input UI)
//   PatchLoginFlow2 @ 0x004D1F5F : E9 64 00 00 00 90    (skip lettura textbox)
//   PatchLoginFlow3 @ 0x004D0DCD : E9 21 01 00 00 90    (forza flag stato login)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <util/util.h>
#include "include/main.h"

namespace
{
    constexpr uintptr_t kLoginPointer = 0x007C48FC;
    constexpr uintptr_t kLoginVTable  = 0x0074FFC0;
    constexpr uintptr_t kUserOffset   = 4;
    constexpr uintptr_t kPwOffset     = 0x27;
    // Global network/game state (DAT_022eed30). L'updater attende *(0x022EED30)!=0
    // ("Network state ready") PRIMA di iniettare: forzare il login (send 0xA101)
    // prima che la rete sia pronta fa crashare game.exe. Stesso gate qui.
    constexpr uintptr_t kNetStatePointer = 0x022EED30;

    struct PatchSite { uintptr_t addr; size_t len; const uint8_t* bytes; };

    const uint8_t kPatch1[10] = { 0xE9, 0x54, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90 };
    const uint8_t kPatch2[6]  = { 0xE9, 0x64, 0x00, 0x00, 0x00, 0x90 };

    // patch1 (force CALL DoLogin) + patch2 (skip textbox read). NIENTE patch3:
    // il vecchio PatchLoginFlow3 (JMP 0x4D0DCD->0x4D0EF3) saltava MOV EAX,EDI -> EAX garbage
    // -> crash OOB a 0x4D0EF9. Sostituito dal data-write obj+0x714=2 (vedi sotto), che fa
    // saltare il blocco display/overwrite-username coi salti nativi del gioco.
    const PatchSite kSites[2] = {
        { 0x004D4EBF, 10, kPatch1 },
        { 0x004D1F5F, 6,  kPatch2 },
    };
    constexpr int      kNumSites           = 2;
    constexpr uintptr_t kSkipDisplayOffset = 0x714;  // obj+0x714 = 2 -> skip display block

    // Settle dopo "network state ready" prima di forzare il login. L'updater originale
    // aspettava fino a ~1s (loop diagnostico socket 10x100ms): forzare il send 0xA101
    // troppo presto (RSA/handshake non pronti) crasha game.exe. Replichiamo: applica
    // quando il login socket e' attivo OPPURE dopo kSettleMs dal primo "ready".
    constexpr DWORD kSettleMs = 1000;

    char    g_user[33] = {};
    char    g_pw[33]   = {};
    bool    g_haveCreds = false;
    bool    g_initDone  = false;
    bool    g_applied   = false;   // patch applicati (login in corso)
    bool    g_finished  = false;   // login gia' avviato una volta: non ri-applicare
    DWORD   g_readyTick = 0;       // tick del primo frame "network state ready"

    uint8_t g_backup[kNumSites][16] = {};  // backup live dei byte originali (max 16/sito)

    void read_env_once(const char* name, char* out, size_t outCap)
    {
        char buf[64] = {};
        DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        SetEnvironmentVariableA(name, nullptr);  // cancella subito dall'env
        if (n > 0 && n < sizeof(buf))
        {
            size_t copy = (n < outCap - 1) ? n : (outCap - 1);
            std::memcpy(out, buf, copy);
        }
        SecureZeroMemory(buf, sizeof(buf));
    }
}

namespace auto_login
{
    // Letto+azzerato all'avvio (Main): minimizza la finestra di esposizione in env.
    void init()
    {
        if (g_initDone) return;
        g_initDone = true;
        read_env_once("SDEV_LOGIN_USER", g_user, sizeof(g_user));
        read_env_once("SDEV_LOGIN_PW",   g_pw,   sizeof(g_pw));
        g_haveCreds = (g_user[0] != '\0');
    }

    // Chiamato ogni frame da present_hook. No-op se: niente credenziali (login manuale),
    // gia' avviato, o schermata di login non attiva.
    void tick()
    {
        if (!g_haveCreds || g_finished || g_applied)
            return;

        uintptr_t obj = 0;
        if (util::read_memory((void*)kLoginPointer, &obj, sizeof(obj)) == 0 || obj == 0)
            return;

        uintptr_t vtbl = 0;
        if (util::read_memory((void*)obj, &vtbl, sizeof(vtbl)) == 0 || vtbl != kLoginVTable)
            return;  // la schermata attiva non e' CLogin

        // Attendi che lo stato di rete sia inizializzato (come fa l'updater): forzare il
        // login prima che la rete/socket siano pronti fa crashare game.exe (send 0xA101).
        uintptr_t netState = 0;
        if (util::read_memory((void*)kNetStatePointer, &netState, sizeof(netState)) == 0 || netState == 0)
        {
            g_readyTick = 0;  // rete non pronta: resetta il timer di settle
            return;
        }

        // Settle come l'updater: applica quando il login socket (*(netState+4)) e' attivo,
        // OPPURE dopo kSettleMs dal primo "ready" (il socket spesso resta 0 fino al DoLogin).
        if (g_readyTick == 0)
            g_readyTick = GetTickCount();
        uintptr_t loginSocket = 0;
        util::read_memory((void*)(netState + 4), &loginSocket, sizeof(loginSocket));
        if (loginSocket == 0 && (GetTickCount() - g_readyTick) < kSettleMs)
            return;  // non ancora pronto: aspetta

        // Schermata di login attiva + rete pronta + settle: inietta credenziali + patch.
        util::write_memory((void*)(obj + kUserOffset), g_user, 32);
        util::write_memory((void*)(obj + kPwOffset),   g_pw,   32);

        for (int i = 0; i < kNumSites; ++i)
        {
            util::read_memory((void*)kSites[i].addr, g_backup[i], kSites[i].len);
            util::write_memory((void*)kSites[i].addr, kSites[i].bytes, kSites[i].len);
        }
        // Al posto del buggato patch3: data-write obj+0x714=2 -> il gioco salta il blocco
        // display/overwrite-username coi suoi salti nativi (no registri stale, no crash).
        uint8_t skipFlag = 2;
        util::write_memory((void*)(obj + kSkipDisplayOffset), &skipFlag, 1);
        g_applied  = true;
        g_finished = true;  // un solo tentativo automatico; il resto e' manuale

        // Le credenziali sono ora nell'oggetto di gioco: azzera le nostre copie.
        SecureZeroMemory(g_user, sizeof(g_user));
        SecureZeroMemory(g_pw,   sizeof(g_pw));
        g_haveCreds = false;
    }

    // Ripristina i byte originali dopo il send 0xA101 (login partito).
    void on_login_sent()
    {
        if (!g_applied) return;
        for (int i = 0; i < kNumSites; ++i)
            util::write_memory((void*)kSites[i].addr, g_backup[i], kSites[i].len);
        // obj+0x714 NON ripristinato: e' lo stato "connesso" che il gioco setta da solo.
        g_applied = false;
    }
}

void hook::auto_login()
{
    auto_login::init();
}

// Chiamata dall'hook ASM del send 0xA101 (packet.cpp, naked_0x5EC5D5).
extern "C" void auto_login_after_send()
{
    auto_login::on_login_sent();
}
