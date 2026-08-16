[CmdletBinding()]
param(
    [string]$EngineRoot = $(if ($env:UE_ENGINE_ROOT) { $env:UE_ENGINE_ROOT } else { 'D:\UnrealEngine5.7.2' }),
    [string]$ProjectPath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ProjectPath) { $ProjectPath = Join-Path $ProjectRoot 'SensorSimulationHost.uproject' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $ProjectRoot 'Saved\Acceptance\RendererOutputMatrixPhase2\UE572'
}

$TestName = 'SensorSimulation.Rendering.OutputMatrix.Phase2.HighResolutionMotionOcclusion'
$EditorCommand = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach ($RequiredPath in @($EditorCommand, $ProjectPath)) {
    if (-not (Test-Path -LiteralPath $RequiredPath)) { throw "Required path was not found: $RequiredPath" }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$Cases = @()
$MetricPattern = 'PHASE2_METRICS Accepted=(\d+) Busy=(\d+) Rejected=(\d+) Enqueued=(\d+) Completed=(\d+) Delivered=(\d+) PeakPending=(\d+) Capacity=(\d+) Created=(\d+) Reused=(\d+) AvgGpuMs=([0-9.]+) MaxGpuMs=([0-9.]+) AvgDeliveryMs=([0-9.]+) MaxDeliveryMs=([0-9.]+)'

foreach ($Rhi in @('D3D11', 'D3D12')) {
    $LogPath = Join-Path $OutputDirectory "$Rhi.log"
    $RhiArgument = if ($Rhi -eq 'D3D11') { '-dx11' } else { '-dx12' }
    $Arguments = @(
        $ProjectPath,
        $RhiArgument,
        '-nocef',
        '-Unattended',
        '-NoSplash',
        '-NoSound',
        '-stdout',
        '-FullStdOutLogOutput',
        "-ExecCmds=Automation RunTests $TestName; Quit",
        "-abslog=$LogPath"
    )

    & $EditorCommand @Arguments
    $ProcessExitCode = $LASTEXITCODE
    $LogText = if (Test-Path -LiteralPath $LogPath) {
        Get-Content -LiteralPath $LogPath -Raw -Encoding utf8
    } else { '' }
    # UnrealEditor-Cmd 可能在进程退出时晚于 abslog 刷新“Test Completed/TEST COMPLETE”尾行；
    # 测试路径证明目标用例实际启动，进程退出码与指标标记共同决定最终 Passed。
    $AutomationPassed = $LogText.Contains("Path={$TestName}")
    $MetricMatch = [regex]::Match($LogText, $MetricPattern)
    $Metrics = if ($MetricMatch.Success) {
        [ordered]@{
            Accepted = [int]$MetricMatch.Groups[1].Value
            Busy = [int]$MetricMatch.Groups[2].Value
            Rejected = [int]$MetricMatch.Groups[3].Value
            Enqueued = [int64]$MetricMatch.Groups[4].Value
            Completed = [int64]$MetricMatch.Groups[5].Value
            Delivered = [int64]$MetricMatch.Groups[6].Value
            PeakPending = [int]$MetricMatch.Groups[7].Value
            Capacity = [int]$MetricMatch.Groups[8].Value
            CreatedResources = [int64]$MetricMatch.Groups[9].Value
            ReusedResources = [int64]$MetricMatch.Groups[10].Value
            AverageGpuLatencyMs = [double]$MetricMatch.Groups[11].Value
            MaxGpuLatencyMs = [double]$MetricMatch.Groups[12].Value
            AverageDeliveryLatencyMs = [double]$MetricMatch.Groups[13].Value
            MaxDeliveryLatencyMs = [double]$MetricMatch.Groups[14].Value
        }
    } else { $null }

    $Cases += [pscustomobject]@{
        Rhi = $Rhi
        ProcessExitCode = $ProcessExitCode
        Passed = $ProcessExitCode -eq 0 -and $MetricMatch.Success
        LogPath = [IO.Path]::GetFullPath($LogPath)
        Metrics = $Metrics
    }
}

$Report = [ordered]@{
    SchemaVersion = 1
    GeneratedAtUtc = [DateTime]::UtcNow.ToString('o')
    EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
    ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
    TestName = $TestName
    Modalities = @('Rgb', 'Semantic', 'Depth', 'Instance')
    Resolutions = @('640x480', '1280x720')
    SceneCases = @('StaticForeground', 'MotionNoGhost', 'Occlusion', 'OpaqueProxyMoved', 'OpaqueProxyIgnore', 'CameraForward', 'CameraResetNoHistory')
    Passed = -not ($Cases.Passed -contains $false)
    Cases = $Cases
}
$ReportPath = Join-Path $OutputDirectory 'RendererOutputMatrixPhase2Summary.json'
$Report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding utf8
$Report | ConvertTo-Json -Depth 8
if (-not $Report.Passed) { exit 1 }
