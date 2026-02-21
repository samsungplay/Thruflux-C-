#Requires -Version 5.1
param(
  [switch]$SystemWide
)

$AppName = "thru.exe"
$UrlX64  = "https://github.com/samsungplay/Thruflux-C-/releases/download/v18/thru_windows"

function Out-Log($m){ Write-Host $m }
function Out-Warn($m){ Write-Host ("warning: " + $m) -ForegroundColor Yellow }
function Out-Err($m){ Write-Host ("error: " + $m) -ForegroundColor Red; exit 1 }

if ($env:PROCESSOR_ARCHITECTURE -ne "AMD64") {
  Out-Err "unsupported architecture: $env:PROCESSOR_ARCHITECTURE (AMD64 only)"
}

if ($SystemWide) {
  $InstallDir = Join-Path $env:ProgramFiles "Thruflux"
} else {
  $InstallDir = Join-Path $env:LOCALAPPDATA "Thruflux\bin"
}
$Target = Join-Path $InstallDir $AppName

try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

Out-Log "Thruflux installer (Windows)"
Out-Log "install dir: $InstallDir"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$tmp = Join-Path $env:TEMP ("thru-" + [Guid]::NewGuid().ToString() + ".exe")

Out-Log "downloading…"
Invoke-WebRequest -Uri $UrlX64 -OutFile $tmp -UseBasicParsing

if ((Get-Item $tmp).Length -lt 1024) {
  Remove-Item -Force $tmp -ErrorAction SilentlyContinue
  Out-Err "download failed or file too small"
}

Out-Log "installing…"
Copy-Item -Force $tmp $Target
Remove-Item -Force $tmp -ErrorAction SilentlyContinue

if ($SystemWide) {
  try {
    $old = [Environment]::GetEnvironmentVariable("Path", "Machine")
    if ($old -notlike "*$InstallDir*") {
      [Environment]::SetEnvironmentVariable("Path", ($old.TrimEnd(';') + ";" + $InstallDir), "Machine")
      Out-Warn "restart your terminal for PATH changes"
    }
  } catch {
    Out-Err "could not update MACHINE PATH (run PowerShell as Administrator or omit -SystemWide)"
  }
} else {
  $old = [Environment]::GetEnvironmentVariable("Path", "User")
  if ($old -notlike "*$InstallDir*") {
    [Environment]::SetEnvironmentVariable("Path", ($old.TrimEnd(';') + ";" + $InstallDir), "User")
    Out-Warn "restart your terminal for PATH changes"
  }
}

Out-Log "installed: $Target"
Out-Log "done"
Out-Log "try: thru --help"
