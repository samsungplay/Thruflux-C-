#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$AppExe    = "thru.exe"
$UrlX64    = "https://github.com/samsungplay/Thruflux-C-/releases/download/v18/thru_windows"
$InstallDir = Join-Path $env:LOCALAPPDATA "Thruflux\bin"
$Target     = Join-Path $InstallDir $AppExe

function Info([string]$m){ Write-Host ("[thruflux] " + $m) -ForegroundColor Cyan }
function Warn([string]$m){ Write-Host ("[thruflux] " + $m) -ForegroundColor Yellow }
function Fail([string]$m){ Write-Host ("[thruflux] " + $m) -ForegroundColor Red; exit 1 }

if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
  Fail "Windows x64 (AMD64) only."
}

try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

Info "Install dir: $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$tmp = Join-Path $env:TEMP ("thru-" + [Guid]::NewGuid().ToString() + ".exe")

Info "Downloading..."
Invoke-WebRequest -Uri $UrlX64 -OutFile $tmp -UseBasicParsing

if (-not (Test-Path $tmp) -or ((Get-Item $tmp).Length -lt 1024)) {
  Remove-Item -Force $tmp -ErrorAction SilentlyContinue
  Fail "Download failed or looks too small. Check: $UrlX64"
}

Info "Installing..."
Copy-Item -Force $tmp $Target
Remove-Item -Force $tmp -ErrorAction SilentlyContinue

function Add-ToUserPath([string]$dir) {
  $dirNorm = $dir.TrimEnd('\')
  $current = [Environment]::GetEnvironmentVariable("Path", "User")
  if ([string]::IsNullOrWhiteSpace($current)) { $current = "" }

  $parts = $current -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }

  $exists = $false
  foreach ($p in $parts) {
    if ($p.TrimEnd('\') -ieq $dirNorm) { $exists = $true; break }
  }

  if (-not $exists) {
    $newParts = @($parts + $dirNorm)
    $newPath = ($newParts -join ';')
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    Warn "Added to User PATH. Restart your terminal to pick it up."
  } else {
    Info "User PATH already contains install dir."
  }
}

Add-ToUserPath $InstallDir

Info "Installed: $Target"
Info "Run: thru --help"
Info "Re-run this installer any time to update."
