#requires -Version 5.1
param([string]$GameDir)
$ErrorActionPreference='Stop'
if(-not $GameDir){$GameDir=Read-Host 'Path to Mafia III game folder'}
if(-not(Test-Path -LiteralPath $GameDir -PathType Container)){throw 'Game folder missing'}
$GameDir=(Resolve-Path -LiteralPath $GameDir).Path
if(Get-Process -Name mafia3definitiveedition -ErrorAction SilentlyContinue){
  throw 'Close Mafia III before restoring the DLL.'
}
$active=Join-Path $GameDir 'dinput8.dll'
$backup=Join-Path $GameDir 'MafiaReflections_Audit_Previous_dinput8.bak'
$manifest=Join-Path $GameDir 'MafiaReflections_Audit_Manifest.json'
foreach($path in @($active,$backup,$manifest)){
  if(-not(Test-Path -LiteralPath $path -PathType Leaf)){throw "Required installed file missing: $path"}
}
$m=Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if($m.schema -ne 1 -or $m.priorHash -notmatch '^[a-f0-9]{64}$' -or
   $m.installedHash -notmatch '^[a-f0-9]{64}$'){throw 'Invalid audit manifest.'}
if((Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash.ToLowerInvariant() -ne $m.installedHash){
  throw 'Active dinput8.dll has changed since installation. Refusing to overwrite another mod.'
}
if((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant() -ne $m.priorHash){
  throw 'Previous DLL backup changed or damaged.'
}
$confirm=Read-Host 'Restore the previous dinput8.dll? Type RESTORE'
if($confirm -cne 'RESTORE'){Write-Host 'Cancelled.'; exit 0}
Copy-Item -LiteralPath $backup -Destination $active -Force -ErrorAction Stop
if((Get-FileHash -LiteralPath $active -Algorithm SHA256).Hash.ToLowerInvariant() -ne $m.priorHash){
  throw 'Previous DLL could not be restored correctly; backup preserved.'
}
Remove-Item -LiteralPath $manifest -Force
# Archive the verified previous DLL instead of leaving a stale working
# backup that would block a future install or risk accidental overwrite.
$archive=$backup+'.restored_'+[Guid]::NewGuid().ToString('N')
Move-Item -LiteralPath $backup -Destination $archive -ErrorAction Stop
Write-Host 'Previous dinput8.dll restored. Reflection settings untouched.' -ForegroundColor Green
Write-Host "Verified previous DLL archived at $archive"
