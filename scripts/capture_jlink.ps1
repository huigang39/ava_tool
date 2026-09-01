# Address verified from the current STM32H745 firmware's timing structure.
# Override -TimingAddress when testing a firmware with a different RAM layout.
param([string]$TimingAddress = '0x20004E6C', [string]$HeartbeatAddress = '0x20004C20')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$program = Join-Path $projectRoot 'bin\win\ava_tool.exe'
if (Get-Process -Name ava_tool -ErrorAction SilentlyContinue) {
    throw 'Please exit ava_tool before starting diagnostic capture.'
}
if (-not (Test-Path -LiteralPath $program)) { throw 'Build bin/win/ava_tool.exe first.' }
$captureDirectory = Join-Path $projectRoot ('build\jlink-diagnostics\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $captureDirectory -Force | Out-Null
$previousLogDirectory = $env:AVA_JLINK_LOG_DIR
$previousTimingAddress = $env:AVA_JLINK_TIMING_ADDRESS
$previousHeartbeatAddress = $env:AVA_JLINK_HEARTBEAT_ADDRESS
try {
    $env:AVA_JLINK_LOG_DIR = $captureDirectory
    $env:AVA_JLINK_TIMING_ADDRESS = $TimingAddress
    $env:AVA_JLINK_HEARTBEAT_ADDRESS = $HeartbeatAddress
    Write-Host "J-Link logs: $captureDirectory"
    Write-Host "Timing RAM snapshot address: $TimingAddress"
    Write-Host 'Connect, reset once, then reproduce disconnect/reconnect and exit the app.'
    # Diagnostics do not connect or reset the target automatically.
    Start-Process -FilePath $program -WorkingDirectory (Split-Path -Parent $program) -Wait -WindowStyle Normal
} finally {
    $env:AVA_JLINK_LOG_DIR = $previousLogDirectory
    $env:AVA_JLINK_TIMING_ADDRESS = $previousTimingAddress
    $env:AVA_JLINK_HEARTBEAT_ADDRESS = $previousHeartbeatAddress
    Write-Host "Capture saved: $captureDirectory"
}

