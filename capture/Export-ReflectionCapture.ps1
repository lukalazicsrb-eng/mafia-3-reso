[CmdletBinding()]
param(
    [string]$Capture,
    [string]$RenderDoc,
    [string]$Report
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Select-File([string]$Title, [string]$Filter) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = [Windows.Forms.OpenFileDialog]::new()
    try {
        $dialog.Title = $Title
        $dialog.Filter = $Filter
        if ($dialog.ShowDialog() -eq [Windows.Forms.DialogResult]::OK) {
            return $dialog.FileName
        }
    } finally {
        $dialog.Dispose()
    }
    throw 'Izbor fajla je otkazan.'
}

if (-not $Capture) {
    $Capture = Select-File 'Izaberi RenderDoc snimak Mafia III' 'RenderDoc snimak (*.rdc)|*.rdc'
}
$captureFile = Get-Item -LiteralPath $Capture
if ($captureFile.PSIsContainer -or $captureFile.Extension -ne '.rdc') {
    throw 'Izaberi postojeci .rdc fajl.'
}

if (-not $RenderDoc) {
    $portable = Join-Path $PSScriptRoot 'RenderDoc_1.47_64\qrenderdoc.exe'
    $installed = Join-Path $env:ProgramFiles 'RenderDoc\qrenderdoc.exe'
    $RenderDoc = @($portable, $installed) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $RenderDoc) {
        $RenderDoc = Select-File 'Izaberi qrenderdoc.exe (RenderDoc 1.47)' 'RenderDoc|qrenderdoc.exe'
    }
}
$renderDocFile = Get-Item -LiteralPath $RenderDoc
if ($renderDocFile.PSIsContainer -or $renderDocFile.Name -ne 'qrenderdoc.exe') {
    throw 'Izaberi qrenderdoc.exe.'
}

if (-not $Report) {
    $Report = Join-Path $captureFile.DirectoryName ($captureFile.BaseName + '.reflection-report.json')
}
$reportPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Report)
if ([IO.Path]::GetExtension($reportPath) -ne '.json' -or (Test-Path -LiteralPath $reportPath)) {
    throw 'Izaberi novi .json izlaz ili premesti prethodni izvestaj.'
}

$script = Join-Path $PSScriptRoot 'export_reflections.py'
$previousCapture = $env:MAFIA_REFLECTION_CAPTURE
$previousReport = $env:MAFIA_REFLECTION_REPORT
try {
    $env:MAFIA_REFLECTION_CAPTURE = $captureFile.FullName
    $env:MAFIA_REFLECTION_REPORT = $reportPath
    Write-Host 'RenderDoc analizira snimak. Ako se prvi put pokrece, potvrdi pocetni dijalog.'
    $process = Start-Process -FilePath $renderDocFile.FullName -ArgumentList "--python `"$script`"" -PassThru
    try {
        if (-not $process.WaitForExit(240000)) {
            if (-not $process.HasExited) { $process.Kill() }
            throw 'RenderDoc nije zavrsio analizu u roku od cetiri minuta.'
        }
    } finally {
        $process.Dispose()
    }
} finally {
    $env:MAFIA_REFLECTION_CAPTURE = $previousCapture
    $env:MAFIA_REFLECTION_REPORT = $previousReport
}

if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
    throw 'RenderDoc nije napravio izvestaj. Probaj sa verzijom 1.47 i zatvori druge prozore RenderDoc-a.'
}
$data = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($data.status -ne 'ok') {
    throw "RenderDoc analiza nije uspela. $($data.error) Izvestaj: $reportPath"
}
Write-Host "Izvestaj: $reportPath"
Write-Host "Zabelezeni prolazi: $(@($data.passes).Count)"
