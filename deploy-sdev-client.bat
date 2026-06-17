@echo off
:: Deploy rapido sdev-client: build + upload server + aggiorna manifest
:: Doppio click per avvio. Oppure usare deploy-sdev-client.ps1 per opzioni avanzate.
powershell.exe -ExecutionPolicy Bypass -NonInteractive -File "%~dp0deploy-sdev-client.ps1"
pause
