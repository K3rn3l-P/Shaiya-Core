#pragma once

namespace security_logger
{
    // Avvia il worker thread ODBC in background.
    // Chiamare da un thread normale DOPO DllMain (non da IOCP, non da Loader Lock).
    void init();

    // Ferma il worker thread e scarica gli eventi residui nel DB.
    // Chiamare da DLL_PROCESS_DETACH (thread singolo, processa il drain).
    void shutdown();

    // Scrive ip/username/event_type/detail su file + accoda per INSERT DB.
    // Thread-safe. Non-blocking per il chiamante (file I/O sincrono, DB asincrono).
    void log(const char* ip, const char* username, const char* event_type, const char* detail);

    // Accoda un ban automatico via sp_BanUser (SecEventWriter ODBC, worker thread).
    // Non blocca: il worker esegue EXEC dbo.sp_BanUser in background.
    // uid = Users_Master.UserUID (= CUser::billingId in ps_game).
    void ban_user(int uid, const char* reason);

    // Invalida il secToken dell'utente al logout (DELETE GameSecuritySessions).
    // Non blocca: eseguito dal worker thread ODBC.
    void invalidate_session(const char* username);
}
