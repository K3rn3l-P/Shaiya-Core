# Deploy sdev-client.dll → server Files/ + aggiorna manifest.json
#
# Uso manuale:   .\deploy-sdev-client.ps1
# Post-build VS: .\deploy-sdev-client.ps1 -DllPath "$(TargetPath)" -SkipBuild
#
# In Visual Studio 2022 → sdev-client Properties → Build Events → Post-Build Event:
#   powershell.exe -ExecutionPolicy Bypass -NonInteractive -File "$(SolutionDir)deploy-sdev-client.ps1" -DllPath "$(TargetPath)" -SkipBuild

param(
    [string]$DllPath   = "",      # percorso DLL già compilata (usato da VS post-build)
    [switch]$SkipBuild            # non fare MSBuild (usato da VS post-build)
)

$ErrorActionPreference = 'Continue'

$sshKey          = "C:\Users\lol\.ssh\id_ed25519"
$sshHost         = "administrator@10.10.5.91"
$serverFilesPath = "C:/xampp/htdocs/Updater/Files/sdev.dll"
$localClientDir  = "C:\Users\lol\Desktop\Shaiya Core Beta v0.11\Shaiya Core Client"
$sshOpts         = @("-i", $sshKey, "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no")

# ---------------------------------------------------------------------------
# 1. Build
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    $msbuild  = "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
    $solution = "$PSScriptRoot\Shaiya-Core.sln"
    Write-Host "`n[BUILD] sdev-client Release x86..." -ForegroundColor Cyan
    & $msbuild $solution /p:Configuration=Release /p:Platform=x86 /t:"sdev-client:Rebuild" /m /nologo
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] Build fallita." -ForegroundColor Red; exit 1 }
    $DllPath = "$PSScriptRoot\Release\sdev-client.dll"
}

# ---------------------------------------------------------------------------
# 2. Calcola hash e size
# ---------------------------------------------------------------------------
if (-not (Test-Path $DllPath)) {
    Write-Host "[FAIL] DLL non trovata: $DllPath" -ForegroundColor Red; exit 1
}
$sha256 = (Get-FileHash $DllPath -Algorithm SHA256).Hash.ToLower()
$size   = (Get-Item $DllPath).Length
Write-Host "[INFO] DLL: $([System.IO.Path]::GetFileName($DllPath))  SHA256=$sha256  SIZE=$size bytes"

# ---------------------------------------------------------------------------
# 3. Upload DLL sul server (Files/sdev.dll)
# ---------------------------------------------------------------------------
Write-Host "`n[UPLOAD] Invio sdev.dll → ${sshHost}:$serverFilesPath ..." -ForegroundColor Cyan

# Assicura che la cartella Files esista sul server
& ssh @sshOpts $sshHost "cmd /c if not exist C:\xampp\htdocs\Updater\Files mkdir C:\xampp\htdocs\Updater\Files" 2>$null

& scp @sshOpts $DllPath "${sshHost}:$serverFilesPath" 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] Upload SCP fallito." -ForegroundColor Red; exit 1 }
Write-Host "[OK] Upload completato." -ForegroundColor Green

# ---------------------------------------------------------------------------
# 4. Copia in cartella client locale
# ---------------------------------------------------------------------------
if (Test-Path $localClientDir) {
    Copy-Item $DllPath "$localClientDir\sdev.dll" -Force
    Write-Host "[OK] sdev.dll copiato in client locale: $localClientDir" -ForegroundColor Green
}

Write-Host "`n[DONE] sdev-client deploy completato.`n" -ForegroundColor Green
