#requires -Version 5.1
param([string]$GameDir)
$ErrorActionPreference='Stop'
$originalHash='529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900'
$knownPreviousProxyHash='9e3875d13e85052ee5ba79030ba8ac02dd75732b01af4c307f99ce9ea534c84a'
Write-Host 'Mafia III Reflections — bounded DX11 capture patch (NOT an Ultra renderer fix)' -ForegroundColor Yellow
if (-not $GameDir) { $GameDir=Read-Host 'Path to folder containing mafia3definitiveedition.exe' }
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { throw 'Game folder missing' }
$GameDir=(Resolve-Path -LiteralPath $GameDir).Path
if (-not(Test-Path -LiteralPath (Join-Path $GameDir 'mafia3definitiveedition.exe') -PathType Leaf)) {
  throw 'This is not a Mafia III Definitive Edition installation folder.'
}
if (Get-Process -Name mafia3definitiveedition -ErrorAction SilentlyContinue) {
  throw 'Close Mafia III before updating the DLL.'
}
$active=Join-Path $GameDir 'dinput8.dll'
$original=Join-Path $GameDir 'ModernTweaks_reflections_original.dll'
$backup=Join-Path $GameDir 'MafiaReflections_Audit_Previous_dinput8.bak'
$manifest=Join-Path $GameDir 'MafiaReflections_Audit_Manifest.json'
$incoming=Join-Path $PSScriptRoot 'dinput8.dll'
foreach($path in @($active,$incoming)){
  if (-not(Test-Path -LiteralPath $path -PathType Leaf)) {throw "Required DLL missing: $path"}
}
foreach($path in @($backup,$manifest)){
  if(Test-Path -LiteralPath $path) {throw "Existing audit backup or manifest: $path. Restore previous install first."}
}
$old=(Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash.ToLowerInvariant()
if($old -ne $originalHash -and $old -ne $knownPreviousProxyHash){
  throw "Active dinput8.dll is not the known original or previous diagnostic DLL: $old"
}
if(Test-Path -LiteralPath $original){
  $saved=(Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant()
  if($saved -ne $originalHash){throw 'Preserved original Modern Tweaks does not match expected version.'}
} elseif($old -ne $originalHash){
  throw 'Existing diagnostic proxy is active but saved original Modern Tweaks is missing.'
}
$new=(Get-FileHash -LiteralPath $incoming -Algorithm SHA256).Hash.ToLowerInvariant()
$confirm=Read-Host 'Install non-rendering capture improvement, preserving High settings? Type CAPTURE'
if($confirm -cne 'CAPTURE'){Write-Host 'Cancelled without changes.';exit 0}
$hasOriginal=Test-Path -LiteralPath $original
try {
  Copy-Item -LiteralPath $active -Destination $backup -ErrorAction Stop
  if(-not $hasOriginal){Copy-Item -LiteralPath $active -Destination $original -ErrorAction Stop}
  Copy-Item -LiteralPath $incoming -Destination $active -Force -ErrorAction Stop
  if((Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash.ToLowerInvariant() -ne $new){
    throw 'Installed DLL hash check failed.'
  }
  @{schema=1;priorHash=$old;installedHash=$new;originalCreated=(!$hasOriginal)} |
    ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8 -ErrorAction Stop
  Write-Host 'Diagnostic capture patch installed. The user-chosen High reflection setting is unchanged.' -ForegroundColor Green
  Write-Host 'This does NOT activate Ultra filtering or MSAA in the game.'
} catch {
  if(Test-Path -LiteralPath $backup){
    Copy-Item -LiteralPath $backup -Destination $active -Force -ErrorAction Stop
  }
  throw
}
