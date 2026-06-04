# Deploy sdev-login.dll al server via SSH/SCP.
# Uso: .\deploy-sdev-login.ps1 [-SkipBuild] [-SkipRestart]

param(
    [switch]$SkipBuild,
    [switch]$SkipRestart
)

$ErrorActionPreference = 'Stop'

$msbuild    = "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
$solution   = "$PSScriptRoot\Shaiya-Core.sln"
$dllSrc     = "$PSScriptRoot\Release\sdev-login.dll"
$sshKey     = "C:\Users\lol\.ssh\id_ed25519"
$sshHost    = "administrator@10.10.5.91"
$sshOpts    = "-i `"$sshKey`" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no"
$dllDst     = "C:/ShaiyaServer/PSM_Client/Bin/sdev-login.dll"
$service    = "ps_login"

function Invoke-Ssh($cmd) {
    $out = & ssh -i $sshKey -o IdentitiesOnly=yes -o StrictHostKeyChecking=no $sshHost $cmd 2>&1
    return $out
}

# 1. Build
if (-not $SkipBuild) {
    Write-Host "`n[BUILD] sdev-login Release x86..." -ForegroundColor Cyan
    & $msbuild $solution /p:Configuration=Release /p:Platform=x86 /t:"sdev-login:Rebuild" /m /nologo
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] Build failed." -ForegroundColor Red; exit 1 }
    Write-Host "[OK] Build succeeded." -ForegroundColor Green
} else {
    Write-Host "[SKIP] Build skipped." -ForegroundColor Yellow
}

if (-not (Test-Path $dllSrc)) {
    Write-Host "[FAIL] DLL not found: $dllSrc" -ForegroundColor Red; exit 1
}
$info = Get-Item $dllSrc
Write-Host "      DLL: $($info.Length) bytes  $($info.LastWriteTime)"

# 2. Stop service
if (-not $SkipRestart) {
    Write-Host "`n[STOP] Stopping $service on server..." -ForegroundColor Cyan
    Invoke-Ssh "sc stop $service" | Out-Null
    # Wait until stopped (max 15s)
    $deadline = (Get-Date).AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 500
        $state = Invoke-Ssh "sc query $service"
        $stopped = ($state | Select-String "STATE.*STOPPED").Count -gt 0
    } while (-not $stopped -and (Get-Date) -lt $deadline)
    if (-not $stopped) { Write-Host "[WARN] Service did not stop in time — proceeding anyway." -ForegroundColor Yellow }
    else { Write-Host "[OK] Service stopped." -ForegroundColor Green }
}

# 3. Copy DLL
Write-Host "`n[COPY] Uploading sdev-login.dll..." -ForegroundColor Cyan
& scp -i $sshKey -o IdentitiesOnly=yes -o StrictHostKeyChecking=no $dllSrc "${sshHost}:$dllDst" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] SCP failed." -ForegroundColor Red
    if (-not $SkipRestart) {
        Write-Host "[RESTART] Attempting to restart service anyway..." -ForegroundColor Yellow
        Invoke-Ssh "sc start $service" | Out-Null
    }
    exit 1
}
Write-Host "[OK] DLL uploaded." -ForegroundColor Green

# 4. Start service
if (-not $SkipRestart) {
    Write-Host "`n[START] Starting $service..." -ForegroundColor Cyan
    Invoke-Ssh "sc start $service" | Out-Null
    Start-Sleep -Milliseconds 1500
    $state = Invoke-Ssh "sc query $service"
    $running = ($state | Select-String "STATE.*RUNNING").Count -gt 0
    if ($running) { Write-Host "[OK] Service running." -ForegroundColor Green }
    else { Write-Host "[WARN] Service may not be running — check manually." -ForegroundColor Yellow; $state }
}

Write-Host "`n[DONE] sdev-login deploy complete." -ForegroundColor Green
