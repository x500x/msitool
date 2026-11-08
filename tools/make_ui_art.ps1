# Generates the built-in wizard artwork and the sample EULA RTF files.
# Requires Windows PowerShell / .NET (System.Drawing) - no external tools.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_ui_art.ps1

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'resources\ui'
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

function New-GradientBitmap {
    param(
        [string]$Path,
        [int]$Width,
        [int]$Height,
        [System.Drawing.Color]$Top,
        [System.Drawing.Color]$Bottom,
        [System.Drawing.Color]$Accent
    )
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    $rect = New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $Top, $Bottom, 90.0)
    $g.FillRectangle($brush, $rect)
    $brush.Dispose()

    # A soft accent band along the bottom/right keeps the image from looking flat.
    $accentRect = New-Object System.Drawing.Rectangle(0, [int]($Height * 0.72), $Width, [int]($Height * 0.28))
    $accentBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $accentRect,
        [System.Drawing.Color]::FromArgb(0, $Accent),
        [System.Drawing.Color]::FromArgb(90, $Accent),
        90.0)
    $g.FillRectangle($accentBrush, $accentRect)
    $accentBrush.Dispose()

    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Dispose()
}

# banner.bmp  : 493x58  - inner wizard pages (scaled to 370x44)
New-GradientBitmap -Path (Join-Path $dir 'banner.bmp') -Width 493 -Height 58 `
    -Top ([System.Drawing.Color]::FromArgb(13, 71, 161)) `
    -Bottom ([System.Drawing.Color]::FromArgb(30, 136, 229)) `
    -Accent ([System.Drawing.Color]::FromArgb(255, 255, 255))

# background.bmp : 493x312 - welcome / exit pages (scaled to 370x234)
New-GradientBitmap -Path (Join-Path $dir 'background.bmp') -Width 493 -Height 312 `
    -Top ([System.Drawing.Color]::FromArgb(255, 255, 255)) `
    -Bottom ([System.Drawing.Color]::FromArgb(222, 235, 250)) `
    -Accent ([System.Drawing.Color]::FromArgb(30, 136, 229))

function ConvertTo-Rtf {
    param([string]$Text, [string]$FontName)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('{\rtf1\ansi\deff0{\fonttbl{\f0\fnil ' + $FontName + ';}}\viewkind4\uc1\pard\f0\fs18 ')
    foreach ($ch in $Text.ToCharArray()) {
        $code = [int][char]$ch
        if ($ch -eq "`r") { continue }
        if ($ch -eq "`n") { [void]$sb.Append('\par '); continue }
        if ($ch -eq '\') { [void]$sb.Append('\\'); continue }
        if ($ch -eq '{') { [void]$sb.Append('\{'); continue }
        if ($ch -eq '}') { [void]$sb.Append('\}'); continue }
        if ($code -lt 128) { [void]$sb.Append($ch); continue }
        if ($code -gt 32767) { $code = $code - 65536 }
        [void]$sb.Append("\u$code?")
    }
    [void]$sb.Append('}')
    return $sb.ToString()
}

$en = [IO.File]::ReadAllText((Join-Path $dir 'license.en.txt'), [Text.Encoding]::UTF8)
$zh = [IO.File]::ReadAllText((Join-Path $dir 'license.zh-CN.txt'), [Text.Encoding]::UTF8)

[IO.File]::WriteAllText((Join-Path $dir 'license.en.rtf'),
    (ConvertTo-Rtf -Text $en -FontName 'Segoe UI'), [Text.Encoding]::ASCII)
[IO.File]::WriteAllText((Join-Path $dir 'license.zh-CN.rtf'),
    (ConvertTo-Rtf -Text $zh -FontName 'Microsoft YaHei'), [Text.Encoding]::ASCII)

Write-Host "Wrote banner.bmp, background.bmp, license.en.rtf, license.zh-CN.rtf to $dir"
