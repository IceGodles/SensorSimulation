[CmdletBinding()]
param(
    [Parameter()]
    [string]$EngineRoot = "D:\UnrealEngine5.7.2",

    [Parameter()]
    [string]$ReportPath
)

$ErrorActionPreference = "Stop"
$expected = [ordered]@{ MajorVersion = 5; MinorVersion = 7; PatchVersion = 2 }
$patchPath = Join-Path $PSScriptRoot "EnginePatches\UE5.7.2\SensorSimulationRendererContext.patch"
$versionPath = Join-Path $EngineRoot "Engine\Build\Build.version"
$headerPath = Join-Path $EngineRoot "Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessing.h"
$sourcePath = Join-Path $EngineRoot "Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessing.cpp"

foreach ($requiredPath in @($patchPath, $versionPath, $headerPath, $sourcePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "缺少必需文件：$requiredPath"
    }
}

$version = Get-Content -Raw -LiteralPath $versionPath | ConvertFrom-Json
$versionMatches =
    $version.MajorVersion -eq $expected.MajorVersion -and
    $version.MinorVersion -eq $expected.MinorVersion -and
    $version.PatchVersion -eq $expected.PatchVersion

$header = Get-Content -Raw -LiteralPath $headerPath
$source = Get-Content -Raw -LiteralPath $sourcePath
$features = [ordered]@{
    ContextType = $header.Contains("struct FExtensionContext")
    InstanceCullingMember = $header.Contains("FInstanceCullingManager* InstanceCullingManager")
    NaniteMember = $header.Contains("const Nanite::FRasterResults* NaniteRasterResults")
    GetterDeclaration = $header.Contains("GetExtensionContext_RenderThread")
    GetterDefinition = $source.Contains("UE::Renderer::PostProcess::GetExtensionContext_RenderThread()")
    ScopedGuard = $source.Contains("TGuardValue<const FExtensionContext*> ExtensionContextGuard")
}

$missingFeatures = @($features.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object Key)
$patchState = if ($missingFeatures.Count -eq 0) { "Applied" } else { "MissingOrModified" }
$passed = $versionMatches -and $missingFeatures.Count -eq 0

$result = [ordered]@{
    Passed = $passed
    EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
    ExpectedVersion = "5.7.2"
    ActualVersion = "$($version.MajorVersion).$($version.MinorVersion).$($version.PatchVersion)"
    CompatibleChangelist = $version.CompatibleChangelist
    PatchState = $patchState
    MissingFeatures = $missingFeatures
    CheckedAt = (Get-Date).ToString("o")
}

if ($ReportPath) {
    $reportDirectory = Split-Path -Parent $ReportPath
    if ($reportDirectory -and -not (Test-Path -LiteralPath $reportDirectory)) {
        New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ReportPath -Encoding utf8
}

$result | ConvertTo-Json -Depth 4
if (-not $passed) {
    throw "Renderer 补丁检查失败。请核对 UE 5.7.2 版本并重新应用补丁。"
}
