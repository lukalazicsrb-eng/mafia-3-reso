#requires -Version 5.1
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Restore')]
    [string]$Action,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$originalSha256 = '529b16daec59e2ce5c735e5c8e7974662b647591da2847c53a16f165a479e900'
$payloadNames = @('dinput8.dll', 'mafia_reflections_observer.dll', 'MafiaReflections.ini')
$receiptName = 'MafiaReflections_install.json'
$originalName = 'ModernTweaks_reflections_original.dll'
$fsrNames = @('MafiaF8_install.json', 'dinput8.ModernTweaks.2.5.1.bak', 'ModernTweaks_original.dll',
    'mafia_f8_experimental.dll', 'mafia_taa_trace.dll', 'mafia_fsr_gamepass.dll',
    'mafia_live_fsr_backend.dll', 'MafiaF8_test.ini')

function Get-RegularFileHash([string]$Path) {
    $file = Get-Item -LiteralPath $Path -Force
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Expected a regular file: $Path"
    }
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-FileHash([string]$Path, [string]$Expected) {
    $actual = Get-RegularFileHash $Path
    if ($actual -cne $Expected) { throw "SHA-256 mismatch: $Path (found $actual)" }
}

function Get-PayloadHashes($Record) {
    if ($Record.schemaVersion -ne 1 -or $null -eq $Record.files -or
        @($Record.files.PSObject.Properties).Count -ne $payloadNames.Count) {
        throw 'Unrecognized reflections manifest format.'
    }
    $hashes = [ordered]@{}
    foreach ($name in $payloadNames) {
        $property = $Record.files.PSObject.Properties[$name]
        if ($null -eq $property -or $property.Value -isnot [string] -or $property.Value -cnotmatch '^[a-f0-9]{64}$') {
            throw "Invalid SHA-256 in reflections manifest: $name"
        }
        $hashes[$name] = $property.Value
    }
    $hashes
}

function Read-Manifest([string]$Path) {
    $null = Get-RegularFileHash $Path
    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Assert-GameClosed {
    if (Get-Process -Name 'mafia3definitiveedition' -ErrorAction SilentlyContinue) {
        throw 'Close Mafia III completely before changing the installed DLLs.'
    }
}

function New-TransactionFolder([string]$Prefix) {
    $folder = Join-Path $GameDir ($Prefix + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $folder
    $folder
}

function Move-VerifiedFile([string]$Source, [string]$Destination, [string]$Expected) {
    Assert-FileHash $Source $Expected
    [IO.File]::Move($Source, $Destination)
}

function Install-Diagnostic {
    Assert-FileHash $active $originalSha256
    foreach ($name in $fsrNames) {
        if (Test-Path -LiteralPath (Join-Path $GameDir $name)) {
            throw "Previous FSR installation files were found ($name). Restore or archive that installation separately."
        }
    }
    foreach ($name in @($originalName, $receiptName) + $payloadNames[1..2]) {
        if (Test-Path -LiteralPath (Join-Path $GameDir $name)) {
            throw "Destination already exists: $name"
        }
    }
    foreach ($name in $payloadNames) {
        Assert-FileHash (Join-Path $PSScriptRoot $name) $packageHashes[$name]
    }
    if (-not $PSCmdlet.ShouldProcess($GameDir, 'Install the read-only reflections diagnostic')) { return }

    $stage = New-TransactionFolder '.MafiaReflections_install_'
    try {
        foreach ($name in $payloadNames) {
            $stagedFile = Join-Path $stage $name
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stagedFile
            Assert-FileHash $stagedFile $packageHashes[$name]
        }
        $stagedOriginal = Join-Path $stage $originalName
        Copy-Item -LiteralPath $active -Destination $stagedOriginal
        Assert-FileHash $stagedOriginal $originalSha256

        $stagedReceipt = Join-Path $stage $receiptName
        @{ schemaVersion = 1; originalSha256 = $originalSha256; files = $packageHashes } |
            ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $stagedReceipt -Encoding UTF8

        Assert-GameClosed
        Assert-FileHash $active $originalSha256
        [IO.File]::Move($stagedReceipt, $receipt)
        Move-VerifiedFile $stagedOriginal $original $originalSha256
        foreach ($name in $payloadNames[1..2]) {
            Move-VerifiedFile (Join-Path $stage $name) (Join-Path $GameDir $name) $packageHashes[$name]
        }

        Assert-GameClosed
        Assert-FileHash $active $originalSha256
        $displaced = Join-Path $stage 'previous-dinput8.dll'
        [IO.File]::Replace((Join-Path $stage 'dinput8.dll'), $active, $displaced)
        Assert-FileHash $active $packageHashes['dinput8.dll']
        Assert-FileHash $original $originalSha256
        Assert-FileHash $displaced $originalSha256
        Remove-Item -LiteralPath $displaced
        [IO.Directory]::Delete($stage)
        Write-Host 'Read-only reflections diagnostic installed. Launch the game through Steam.' -ForegroundColor Green
    } catch {
        Write-Warning "Installation interrupted. Files staged at: $stage"
        Write-Warning 'Use the bundled Restore command to recover a recorded installation.'
        throw
    }
}

function Restore-Original {
    $activeHash = Get-RegularFileHash $active
    if ($activeHash -cne $originalSha256 -and $activeHash -cne $packageHashes['dinput8.dll']) {
        throw "Unrecognized active dinput8.dll SHA-256: $activeHash"
    }
    if ($activeHash -ceq $originalSha256 -and -not (Test-Path -LiteralPath $receipt)) {
        Write-Host 'Original Modern Tweaks 2.5.1 is already active.'
        return
    }

    $receiptHash = Get-RegularFileHash $receipt
    $record = Read-Manifest $receipt
    $installedHashes = Get-PayloadHashes $record
    if ($record.originalSha256 -cne $originalSha256) { throw 'Unexpected original DLL hash in the install record.' }
    foreach ($name in $payloadNames) {
        if ($installedHashes[$name] -cne $packageHashes[$name]) {
            throw 'Use Restore from the same reflections ZIP that installed this diagnostic.'
        }
    }
    if ($activeHash -cne $originalSha256 -or (Test-Path -LiteralPath $original)) {
        Assert-FileHash $original $originalSha256
    }
    if (-not $PSCmdlet.ShouldProcess($GameDir, 'Restore verified Modern Tweaks 2.5.1')) { return }

    $archive = New-TransactionFolder '.MafiaReflections_restore_'
    try {
        Assert-GameClosed
        Assert-FileHash $receipt $receiptHash
        Assert-FileHash $active $activeHash
        if ($activeHash -cne $originalSha256) {
            $stagedOriginal = Join-Path $archive 'original-staged.dll'
            Copy-Item -LiteralPath $original -Destination $stagedOriginal
            Assert-FileHash $stagedOriginal $originalSha256
            $savedProxy = Join-Path $archive 'dinput8-reflections-proxy.dll'
            [IO.File]::Replace($stagedOriginal, $active, $savedProxy)
            Assert-FileHash $savedProxy $packageHashes['dinput8.dll']
        }
        Assert-FileHash $active $originalSha256
        if (Test-Path -LiteralPath $original) {
            Move-VerifiedFile $original (Join-Path $archive $originalName) $originalSha256
        }
        foreach ($name in $payloadNames[1..2]) {
            $path = Join-Path $GameDir $name
            if (Test-Path -LiteralPath $path) {
                $item = Get-Item -LiteralPath $path -Force
                if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                    (Get-RegularFileHash $path) -cne $installedHashes[$name]) {
                    Write-Warning "Preserved modified file: $path"
                } else {
                    Move-VerifiedFile $path (Join-Path $archive $name) $installedHashes[$name]
                }
            }
        }
        Move-VerifiedFile $receipt (Join-Path $archive $receiptName) $receiptHash
        Write-Host 'Original Modern Tweaks 2.5.1 restored and verified.' -ForegroundColor Green
        Write-Host "Archived diagnostic files: $archive"
    } catch {
        Write-Warning "Restore interrupted. Recovery files retained at: $archive"
        throw
    }
}

if (-not $GameDir) { $GameDir = Read-Host 'Enter the Mafia III game folder' }
$gameFolder = Get-Item -LiteralPath $GameDir
if (-not $gameFolder.PSIsContainer -or $gameFolder.PSProvider.Name -ne 'FileSystem') {
    throw 'The game folder must be a filesystem directory.'
}
$GameDir = $gameFolder.FullName
if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'mafia3definitiveedition.exe') -PathType Leaf)) {
    throw 'mafia3definitiveedition.exe was not found in the selected folder.'
}
Assert-GameClosed

$package = Read-Manifest (Join-Path $PSScriptRoot 'package.json')
$packageHashes = Get-PayloadHashes $package
if ($packageHashes['dinput8.dll'] -ceq $originalSha256) { throw 'The package must contain the diagnostic proxy.' }
$active = Join-Path $GameDir 'dinput8.dll'
$original = Join-Path $GameDir $originalName
$receipt = Join-Path $GameDir $receiptName

# Serialize this installer and its restore command for a given game folder.
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $folderKey = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($GameDir.ToUpperInvariant()))) -replace '-', ''
} finally { $sha.Dispose() }
$mutex = New-Object System.Threading.Mutex($false, "MafiaReflections-$folderKey")
$ownsMutex = $false
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another reflections install or restore is already running for this game folder.' }
    if ($Action -eq 'Install') { Install-Diagnostic } else { Restore-Original }
} finally {
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
