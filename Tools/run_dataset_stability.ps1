param(
    [int]$TargetFrames = 1000,
    [int[]]$FrameRates = @(30, 60, 120),
    [string]$EditorCmd = "E:\UE572\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
)
$ErrorActionPreference = "Stop"
$Project = (Resolve-Path "$PSScriptRoot\..\SensorSimulationHost.uproject").Path
$Before = @(Get-ChildItem "$PSScriptRoot\..\Saved\Dataset" -Directory | ForEach-Object FullName)
$Sessions = @()
foreach ($Fps in $FrameRates) {
    & $EditorCmd $Project /Game/Acceptance/Maps/L_SensorAcceptance -game -unattended -nop4 -nosplash -RenderOffscreen -d3d12 -benchmark "-fps=$Fps" "-SensorTargetCommittedFrames=$TargetFrames" -SensorSimulationMode=DeterministicDataset -SensorShutdownDrainSeconds=10 -log
    if ($LASTEXITCODE -ne 0) { throw "UE capture failed at $Fps FPS" }
    $Session = Get-ChildItem "$PSScriptRoot\..\Saved\Dataset" -Directory | Where-Object { $_.FullName -notin $Before -and $_.FullName -notin $Sessions } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $Session) { throw "No new dataset Session found at $Fps FPS" }
    $Sessions += $Session.FullName
    & "$PSScriptRoot\..\Saved\Acceptance\PythonEnv\Scripts\python.exe" "$PSScriptRoot\validate_dataset.py" $Session.FullName
    if ($LASTEXITCODE -ne 0) { throw "Dataset validation failed at $Fps FPS" }
}
& "$PSScriptRoot\..\Saved\Acceptance\PythonEnv\Scripts\python.exe" "$PSScriptRoot\compare_dataset_hashes.py" @Sessions --exclude "rgb_*.png" --report "$PSScriptRoot\..\Saved\Acceptance\stability_hash_report.json"
exit $LASTEXITCODE
