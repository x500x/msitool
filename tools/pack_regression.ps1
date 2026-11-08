# Packing regression check (PowerShell).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pack_regression.ps1
#
# Builds a throw-away payload, packs it in every UI mode, verifies the result
# and runs an administrative install (msiexec /a), which exercises the tables
# without touching the system. If WiX 3.x is installed it also decompiles the
# wizard package with dark.exe and fails on table-structure warnings.

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

$wix = Join-Path ${env:ProgramFiles(x86)} "WiX Toolset v3.14\bin\dark.exe"
if (Test-Path $wix) {
    Write-Host "== dark.exe decompile"
    $out = Join-Path $Work "wizard.wxs"
    & $wix -nologo $wizard -o $out 2>&1 | ForEach-Object { Write-Host "   $_" }
    $warn = (& $wix -nologo $wizard -o $out 2>&1 | Select-String -Pattern "warning" | Measure-Object).Count
    if ($warn -gt 0) { throw "dark.exe reported $warn table warning(s)" }
    Write-Host "   OK (no dark.exe table warnings)"
} else {
    Write-Host "== dark.exe not installed, skipped"
}

Write-Host "All pack regression checks passed."
