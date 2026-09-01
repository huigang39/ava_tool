param([string]$SdkProject = '')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $SdkProject) { $SdkProject = Join-Path (Split-Path -Parent $repo) 'ava_eth' }
$SdkProject = (Resolve-Path -LiteralPath $SdkProject).Path
# Update the public ABI snapshot and the runtime only. No driver deployment.
foreach ($file in 'sdk.h','controller.h','types.h') {
    $source = Join-Path $SdkProject "sdk/c/include/ava/$file"
    if (-not (Test-Path -LiteralPath $source)) { throw "SDK header missing: $source" }
}
foreach ($file in 'ava_eth_sdk.dll','libwinpthread-1.dll') {
    $source = Join-Path $SdkProject "build/win/$file"
    if (-not (Test-Path -LiteralPath $source)) { throw "Build ava_eth first; runtime missing: $source" }
}
foreach ($dir in 'sdk/ava_eth/include/ava','sdk/ava_eth/bin/win') {
    New-Item -ItemType Directory -Path (Join-Path $repo $dir) -Force | Out-Null
}
foreach ($file in 'sdk.h','controller.h','types.h') {
    Copy-Item -LiteralPath (Join-Path $SdkProject "sdk/c/include/ava/$file") -Destination (Join-Path $repo "sdk/ava_eth/include/ava/$file")
}
foreach ($file in 'ava_eth_sdk.dll','libwinpthread-1.dll') {
    Copy-Item -LiteralPath (Join-Path $SdkProject "build/win/$file") -Destination (Join-Path $repo "sdk/ava_eth/bin/win/$file")
}
Write-Output 'ava_eth SDK synchronized. Rebuild ava_tool to embed the updated runtime and headers.'
