param(
    [string]$EngineRoot = 'D:\UnrealEngine5.7.2',
    [string]$ProjectPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'SensorSimulationHost.uproject'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'Saved\Acceptance\RendererOutputMatrix\UE572')
)

$ErrorActionPreference = 'Stop'
$TestName = 'SensorSimulation.Rendering.OutputMatrix.AllModalities'
$EditorCommand = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'

if (-not (Test-Path -LiteralPath $EditorCommand))
{
    throw "UnrealEditor-Cmd.exe was not found: $EditorCommand"
}
if (-not (Test-Path -LiteralPath $ProjectPath))
{
    throw "Project was not found: $ProjectPath"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$Cases = @()

# D3D11 与 D3D12 使用完全相同的 UE Automation 测试，防止两套验收逻辑长期漂移。
foreach ($Rhi in @('D3D11', 'D3D12'))
{
    $LogPath = Join-Path $OutputDirectory "$Rhi.log"
    $RhiArgument = if ($Rhi -eq 'D3D11') { '-dx11' } else { '-dx12' }
    $Arguments = @(
        $ProjectPath,
        $RhiArgument,
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
    $LogText = if (Test-Path -LiteralPath $LogPath)
    {
        Get-Content -LiteralPath $LogPath -Raw -Encoding utf8
    }
    else
    {
        ''
    }

    # 同时要求进程成功、Automation 成功和最终退出码为 0，避免只检查其中一个产生假通过。
    $AutomationPassed =
        $LogText.Contains("Path={$TestName}") -and
        $LogText.Contains('LogAutomationController: Display: Test Completed.') -and
        $LogText.Contains('**** TEST COMPLETE. EXIT CODE: 0 ****')
    $InstanceEvidence = @(
        [regex]::Matches($LogText, 'ECameraChannelType::Instance[^\r\n]+') |
            ForEach-Object { $_.Value }
    )

    $Cases += [pscustomobject]@{
        Rhi = $Rhi
        ProcessExitCode = $ProcessExitCode
        Passed = ($ProcessExitCode -eq 0 -and $AutomationPassed)
        LogPath = [System.IO.Path]::GetFullPath($LogPath)
        InstancePixelEvidence = $InstanceEvidence
    }
}

# 报告结构固定，后续 CI 或发布脚本可直接读取 Passed 和 Cases，而不需要重新解析完整 UE 日志。
$Report = [pscustomobject]@{
    SchemaVersion = 1
    GeneratedAtUtc = [DateTime]::UtcNow.ToString('o')
    EngineRoot = [System.IO.Path]::GetFullPath($EngineRoot)
    ProjectPath = [System.IO.Path]::GetFullPath($ProjectPath)
    TestName = $TestName
    Modalities = @('Rgb', 'Semantic', 'Depth', 'Instance')
    Resolutions = @('32x24', '17x11')
    Passed = -not ($Cases.Passed -contains $false)
    Cases = $Cases
}
$ReportPath = Join-Path $OutputDirectory 'RendererOutputMatrixSummary.json'
$Report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ReportPath -Encoding utf8
$Report | ConvertTo-Json -Depth 6

if (-not $Report.Passed)
{
    exit 1
}
