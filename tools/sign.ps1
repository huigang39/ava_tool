<#
.SYNOPSIS
  Authenticode-sign one or more files with signtool (SHA-256 + timestamp).

.DESCRIPTION
  Removes the Windows "Unknown Publisher" warning. Requires a code-signing
  certificate. Configure it one of two ways:

    PFX file (OV certificate):
      $env:AVA_SIGN_PFX  = "C:\path\cert.pfx"
      $env:AVA_SIGN_PASS = "pfx-password"      # optional if the pfx has none

    Cert already in the Windows store (e.g. an EV token):
      $env:AVA_SIGN_SHA1 = "<cert thumbprint>"

  If no certificate is configured the script prints a note and exits 0, so an
  unsigned build still succeeds. signtool.exe is taken from PATH or the Windows SDK.

.EXAMPLE
  powershell -File tools\sign.ps1 -Files bin\win\ava_tool.exe,bin\win\updater.exe
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [string]$Pfx,
    [string]$Password,
    [string]$Sha1,
    [string]$Timestamp
)

$ErrorActionPreference = "Stop"

if (-not $Pfx)       { $Pfx = $env:AVA_SIGN_PFX }
if (-not $Password)  { $Password = $env:AVA_SIGN_PASS }
if (-not $Sha1)      { $Sha1 = $env:AVA_SIGN_SHA1 }
if (-not $Timestamp) { $Timestamp = if ($env:AVA_SIGN_TS) { $env:AVA_SIGN_TS } else { "http://timestamp.digicert.com" } }

if (-not $Pfx -and -not $Sha1) {
    Write-Host "[sign] No certificate configured (set AVA_SIGN_PFX/AVA_SIGN_PASS or AVA_SIGN_SHA1) - skipping signing."
    exit 0
}

# Locate signtool.exe (PATH, else newest in the Windows SDK).
$signtool = (Get-Command signtool.exe -ErrorAction SilentlyContinue).Source
if (-not $signtool) {
    $cand = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
    if ($cand) { $signtool = $cand.FullName }
}
if (-not $signtool) {
    Write-Error "signtool.exe not found. Install the Windows SDK (or add signtool to PATH)."
    exit 1
}

$args = @("sign", "/fd", "SHA256", "/tr", $Timestamp, "/td", "SHA256")
if ($Pfx) {
    $args += @("/f", $Pfx)
    if ($Password) { $args += @("/p", $Password) }
} else {
    $args += @("/sha1", $Sha1)
}

foreach ($pattern in $Files) {
    $items = @(Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue)
    if ($items.Count -eq 0) { Write-Host "[sign] skip (no match): $pattern"; continue }
    foreach ($item in $items) {
        Write-Host "[sign] $($item.FullName)"
        & $signtool @args $item.FullName
        if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed for $($item.FullName)"; exit $LASTEXITCODE }
    }
}
Write-Host "[sign] done."
