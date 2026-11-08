# Packing regression check (PowerShell).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pack_regression.ps1
#
# Builds a throw-away payload, packs it in every UI mode, verifies the result
# and runs an administrative install (msiexec /a), which exercises the tables
# without touching the system. If Python is available it also drives the real
# installer wizard (install / repair / remove, en / zh-CN).

param(
    [string]$Exe = "build\msitool_x64.exe",
    [string]$Work = "build\regression"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not (Test-Path $Exe)) { throw "Build the tool first: $Exe" }

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$payload = [IO.Path]::GetFullPath((Join-Path $Work "payload"))
New-Item -ItemType Directory -Force -Path (Join-Path $payload "bin") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $payload "docs") | Out-Null
Set-Content -Path (Join-Path $payload "bin\app.exe") -Value "MZ placeholder" -Encoding Ascii
Set-Content -Path (Join-Path $payload "docs\readme.txt") -Value "hello regression" -Encoding Ascii

function Invoke-Pack($name, [string[]]$extra) {
    $msi = [IO.Path]::GetFullPath((Join-Path $Work "$name.msi"))
    $packArgs = @("pack", $payload, $msi, "--name", "Reg $name", "--force") + $extra
    Write-Host "== pack $name"
    & $Exe @packArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "pack failed: $name" }
    & $Exe verify $msi | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "verify failed: $name" }
    $admin = [IO.Path]::GetFullPath((Join-Path $Work "admin_$name"))
    Remove-Item -Recurse -Force $admin -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $admin | Out-Null
    $p = Start-Process msiexec.exe -ArgumentList @("/a", $msi, "/qn", "TARGETDIR=$admin") -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "msiexec /a failed ($($p.ExitCode)): $name" }
    Write-Host "   OK (verify + /a)"
    return $msi
}

$wizard = Invoke-Pack "wizard" @("--ui", "wizard")
Invoke-Pack "wizard_zh" @("--ui", "wizard", "--language", "2052") | Out-Null
Invoke-Pack "minimal" @("--ui", "minimal") | Out-Null
Invoke-Pack "basic" @("--ui", "basic") | Out-Null
Invoke-Pack "none" @("--ui", "none") | Out-Null
Invoke-Pack "custom" @("--config", "examples\ui-custom.xml") | Out-Null
Invoke-Pack "manifest" @("--config", "examples\pack.xml") | Out-Null
Invoke-Pack "shortcuts" @("--ui", "wizard", "--desktop-shortcut", "--start-menu-shortcut") | Out-Null
Invoke-Pack "peruser" @("--ui", "wizard", "--per-user", "--desktop-shortcut", "--start-menu-shortcut") | Out-Null
Invoke-Pack "peruser_zh" @("--ui", "wizard", "--per-user", "--language", "2052", "--name", "Reg peruser zh") | Out-Null
Invoke-Pack "customactions" @("--config", "examples\custom-actions.xml") | Out-Null

if (Get-Command python -ErrorAction SilentlyContinue) {
    Write-Host "== UI walkthrough (per-user, real dialogs)"
    $per = Join-Path $Work "peruser.msi"
    $perZh = Join-Path $Work "peruser_zh.msi"
    python tools\ui_walkthrough.py --msi $per
    if ($LASTEXITCODE -ne 0) { throw "UI walkthrough failed (install)" }
    python tools\ui_walkthrough.py --msi $per --mode repair
    if ($LASTEXITCODE -ne 0) { throw "UI walkthrough failed (repair)" }
    python tools\ui_walkthrough.py --msi $per --mode remove
    if ($LASTEXITCODE -ne 0) { throw "UI walkthrough failed (remove)" }
    python tools\ui_walkthrough.py --msi $perZh --language zh-CN "--install-dir" (Join-Path $env:LOCALAPPDATA "Reg peruser zh")
    if ($LASTEXITCODE -ne 0) { throw "UI walkthrough failed (zh-CN)" }
} else {
    Write-Host "== python not found, UI walkthrough skipped"
}

Write-Host "All pack regression checks passed."
