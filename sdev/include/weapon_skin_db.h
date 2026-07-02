#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// weapon_skin_db — persistenza della weapon skin per personaggio (charId).
//
// Canale ODBC PROPRIO di sdev verso PS_GameData.dbo.CharWeaponSkins (NON via
// ps_dbAgent). Stesso pattern di security_logger: worker thread dedicato che
// possiede la connessione ODBC; il gioco non fa I/O DB direttamente.
//
//   SAVE : asincrono fire-and-forget (upsert).
//   LOAD : sincrono ma BOUNDED — il chiamante accoda la richiesta e attende
//          il worker (che fa un SELECT su connessione già aperta, ~1ms). Timeout
//          di sicurezza → ritorna false (nessuna skin) senza bloccare a lungo.
//
// Credenziali SOLO da ENV di sistema SHAIYA_SKINDB_USER / SHAIYA_SKINDB_PWD.
// Se mancano → persistenza disabilitata (fail-safe): la feature skin continua
// a funzionare in-sessione, ma non salva/carica.
// ---------------------------------------------------------------------------
namespace weapon_skin_db
{
    // Avvia il worker thread ODBC. Chiamare da Main() (thread normale), dopo la
    // config e prima degli hook IOCP — come security_logger::init().
    void init();

    // Ferma il worker e chiude la connessione. Chiamare da DLL_PROCESS_DETACH.
    void shutdown();

    // Salva/aggiorna la skin del personaggio (type=0 → nessuna skin). Async.
    void save(uint32_t charId, uint8_t type, uint8_t typeId);

    // Carica la skin del personaggio. Ritorna true se esiste una riga (out* validi).
    // Bounded wait sul worker; false se non trovata, DB off o timeout.
    bool load(uint32_t charId, uint8_t& outType, uint8_t& outTypeId);
}
