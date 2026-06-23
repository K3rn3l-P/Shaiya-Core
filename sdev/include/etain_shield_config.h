#pragma once
#include <cstdint>
#include <vector>

// ===========================================================================
//  EtainShield — Runtime configuration
// ===========================================================================
//
//  Loaded from Data/EtainShield.ini at server startup.
//  If the file is missing, all defaults below apply (everything enabled).
//
//  INI layout:
//
//    [General]
//    Enabled=1                   ; Master switch (0 disables ALL protections)
//
//    [AntiSpeedHack]
//    Enabled=1                   ; Toggle for speed-hack protections
//    MaxSpeedOnFoot=12.5         ; Max speed coefficient on foot  (server-measured ~12 + 0.5 margin)
//    MaxSpeedMounted=13.5        ; Max speed coefficient mounted  (server-measured ~13 + 0.5 margin)
//    ViolationLimit=5            ; Consecutive violations before correction (first 4 forgiven)
//
//    [AntiRangeHack]
//    Enabled=1                   ; Toggle for range-hack protections
//    Margin=4                    ; Tolleranza FISSA, sommata SEMPRE (vedi sotto)
//    MovingGrace=5               ; Tolleranza EXTRA, solo se il bersaglio si muove
//    KickOnViolation=1           ; (se LogOnly=0) kicka su violazione
//    BanOnViolation=0            ; (se LogOnly=0) banna nel DB + kicka
//    LogOnly=1                   ; 1 = logga e basta (osserva); 0 = enforce
//
//    --- Come funziona il check range ---
//    Per ogni attacco/skill il server calcola la distanza reale (server-side)
//    tra attaccante e bersaglio e la confronta con:
//
//      allowed = max(attackRange, skillRange) + targetSize + Margin + grace
//      grace   = MovingGrace  se il bersaglio si sta muovendo, altrimenti 0
//
//      attackRange = range base dell'arma/abilità del giocatore
//      skillRange  = range della skill usata (cap a 80); si usa il maggiore
//      targetSize  = dimensione del bersaglio (mob: size reale; player: 1)
//
//    Se distanza <= allowed → OK. Altrimenti = violazione (log; con LogOnly=0
//    scatta kick/ban).
//
//    --- Margin vs MovingGrace ---
//    Margin       = cuscinetto FISSO aggiunto a OGNI check. Assorbe il
//                   disallineamento costante di posizione (lag di rete, tick
//                   server, arrotondamenti). Alzalo se vedi falsi positivi
//                   diffusi anche su bersagli fermi.
//    MovingGrace  = cuscinetto EXTRA aggiunto SOLO quando il bersaglio è in
//                   movimento. Un bersaglio che corre desincronizza molto di
//                   più (la sua posizione client è "avanti" rispetto a quella
//                   server). Alzalo se i falsi positivi capitano soprattutto
//                   in inseguimento / PvP in corsa, senza allargare il margine
//                   sui bersagli fermi.
//
//    --- Come tararli (EtainShield.ini, runtime al riavvio) ---
//    * Troppi falsi positivi (giocatori legittimi loggati come RANGE_HACK):
//        - se su qualsiasi bersaglio  → alza Margin       (es. 4 → 6 → 8)
//        - se solo su bersagli mobili → alza MovingGrace  (es. 5 → 8 → 10)
//    * Troppo permissivo (hacker non rilevati): abbassa i due valori.
//    * Esempio reale: log "dist=29.26 allowed=28.00" = 1.26 oltre soglia =
//      lag su bersaglio lontano. Bastava Margin +2 (o MovingGrace +2) per
//      assorbirlo senza falsare il rilevamento dei range hack veri (che
//      superano la soglia di parecchie unità).
//    * Unità = coordinate di gioco; valori interi. Tieni LogOnly=1 mentre
//      tari, leggi rangehack-{data}.log, poi metti LogOnly=0 quando i falsi
//      positivi sono spariti.
//

struct EtainShieldConfig
{
    // [General]
    bool enabled = true;

    // [AntiSpeedHack]
    bool         speedHackEnabled        = true;
    float        speedMaxOnFoot          = 12.5f;  // server-measured ~12 + 0.5 margin
    float        speedMaxMounted         = 13.5f;  // server-measured ~13 + 0.5 margin
    uint8_t      speedViolationThreshold = 5;      // first 4 forgiven, 5th triggers correction

    // [AntiRangeHack]
    bool         rangeHackEnabled        = true;
    int          rangeMargin             = 4;
    int          rangeMovingGrace        = 5;   // Tolleranza range extra per bersagli in movimento (riduce FP)
    bool         rangeKickOnViolation    = false;
    bool         rangeBanOnViolation     = false;
    bool         rangeLogOnly            = true; // true = logga e basta (ignora kick+ban); false = enforce

    // [AntiSpeedHack] enforcement
    bool         speedKickOnViolation    = false;
    bool         speedBanOnViolation     = false;
    bool         speedLogOnly            = true; // true = logga e basta (ignora kick+ban); false = enforce

    // [AntiCutting] — anti attack-move-cancel. Default LOG-ONLY (osserva, non blocca)
    // per tarare LockMs sui tempi reali skill/attacco prima di abilitare l'enforce.
    bool              cuttingEnabled   = true;
    bool              cuttingLogOnly   = true;   // true = solo log; false = enforce (droppa move + kick/ban se attivi)
    uint32_t          cuttingLockMs    = 500;    // finestra post-attacco in ms
    std::vector<int>  cuttingSkipSkills;         // skill esenti (applicato in fase enforce)
    bool              cuttingKickOnViolation = false; // enforce: kicka su violazione (solo se cuttingLogOnly=false)
    bool              cuttingBanOnViolation  = false; // enforce: banna su violazione (solo se cuttingLogOnly=false)

    // [AntiDupe] — blocca item bound (ItemInfo.reqOg>=1) da drop/trade/battle/market/guild-WH.
    // Player WH (magazzino personale) ESCLUSO di proposito. Anti bypass-binding.
    bool              blockBoundItems  = true;
};

/// Global config instance — loaded once at startup, read at runtime.
extern EtainShieldConfig g_etainConfig;
