[CmdletBinding()]
param(
    [Parameter()]
    [string]$EngineRoot = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { 'D:\UnrealEngine5.7.2' }),

    [Parameter()]
    [string]$ProjectPath,

    [Parameter()]
    [string]$Target = 'SensorSimulationHostEditor',

    [Parameter()]
    [ValidateSet('Debug', 'DebugGame', 'Development', 'Shipping', 'Test')]
    [string]$Configuration = 'Development',

    [Parameter()]
    [string]$ReportDirectory,

    [Parameter()]
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ProjectPath) {
    $ProjectPath = Join-Path $projectRoot 'SensorSimulationHost.uproject'
}
if (-not $ReportDirectory) {
    $ReportDirectory = Join-Path $projectRoot 'Saved\Acceptance\RendererPreflight\UE572'
}
$startedAt = Get-Date
$checkerPath = Join-Path $PSScriptRoot 'check_renderer_patch.ps1'
$patchReportPath = Join-Path $ReportDirectory 'RendererPatchCheck.json'
$summaryPath = Join-Path $ReportDirectory 'RendererPreflightSummary.json'
$buildScript = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'

function Write-Summary {
    param(
        [bool]$Passed,
        [bool]$PreflightPassed,
        [Nullable[int]]$BuildExitCode,
        [string]$FailureStage
    )

    $summary = [ordered]@{
        SchemaVersion = 1
        Passed = $Passed
        PreflightPassed = $PreflightPassed
        BuildRequested = -not $SkipBuild.IsPresent
        BuildExitCode = $BuildExitCode
        FailureStage = $FailureStage
        EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
        ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
        Target = $Target
        Platform = 'Win64'
        Configuration = $Configuration
        PatchReportPath = [IO.Path]::GetFullPath($patchReportPath)
        StartedAt = $startedAt.ToString('o')
        FinishedAt = (Get-Date).ToString('o')
    }

    $summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8
    $summary | ConvertTo-Json -Depth 4
}

New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null

if (-not (Test-Path -LiteralPath $checkerPath)) {
    Write-Summary -Passed $false -PreflightPassed $false -BuildExitCode $null -FailureStage 'PreflightSetup'
    throw "Renderer patch checker was not found: $checkerPath"
}

# Run the checker in a child PowerShell process so its intentional non-zero exit code
# can be captured without terminating this orchestration script.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $checkerPath `
    -EngineRoot $EngineRoot `
    -ReportPath $patchReportPath
$preflightExitCode = $LASTEXITCODE

if ($preflightExitCode -ne 0) {
    Write-Summary -Passed $false -PreflightPassed $false -BuildExitCode $null -FailureStage 'RendererPatchCheck'
    exit $preflightExitCode
}

if ($SkipBuild) {
    Write-Summary -Passed $true -PreflightPassed $true -BuildExitCode $null -FailureStage $null
    exit 0
}

foreach ($requiredPath in @($ProjectPath, $buildScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        Write-Summary -Passed $false -PreflightPassed $true -BuildExitCode $null -FailureStage 'BuildSetup'
        throw "Build input was not found: $requiredPath"
    }
}

& $buildScript `
    $Target `
    'Win64' `
    $Configuration `
    $ProjectPath `
    '-WaitMutex' `
    '-NoHotReloadFromIDE' `
    '-NoUBA' `
    '-MaxParallelActions=1'
$buildExitCode = $LASTEXITCODE

if ($buildExitCode -ne 0) {
    Write-Summary -Passed $false -PreflightPassed $true -BuildExitCode $buildExitCode -FailureStage 'Build'
    exit $buildExitCode
}

Write-Summary -Passed $true -PreflightPassed $true -BuildExitCode 0 -FailureStage $null
exit 0
