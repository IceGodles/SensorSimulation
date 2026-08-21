#include "SimulationSettings.h"

#include "Misc/Paths.h"

/** 把空路径和相对路径统一锚定到 Project/Saved，并拒绝相对路径逃逸该目录。 */
FString USimulationSettings::ResolveDatasetRoot(const FDirectoryPath& ConfiguredRoot)
{
    FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    FPaths::NormalizeDirectoryName(SavedRoot);

    FString ConfiguredPath = ConfiguredRoot.Path;
    ConfiguredPath.TrimStartAndEndInline();
    FString Resolved;
    if (ConfiguredPath.IsEmpty())
    {
        Resolved = FPaths::Combine(SavedRoot, TEXT("SensorSimulation"));
    }
    else if (FPaths::IsRelative(ConfiguredPath))
    {
        Resolved = FPaths::ConvertRelativePathToFull(SavedRoot, ConfiguredPath);
        FPaths::NormalizeDirectoryName(Resolved);
        if (!FPaths::IsUnderDirectory(Resolved, SavedRoot) && !Resolved.Equals(SavedRoot))
        {
            UE_LOG(LogTemp, Error,
                TEXT("Relative DatasetRoot '%s' escapes Project/Saved; using Saved/SensorSimulation instead."),
                *ConfiguredPath);
            Resolved = FPaths::Combine(SavedRoot, TEXT("SensorSimulation"));
        }
    }
    else
    {
        Resolved = FPaths::ConvertRelativePathToFull(ConfiguredPath);
    }

    FPaths::NormalizeDirectoryName(Resolved);
    return Resolved;
}

/** 从 Settings CDO 复制一次不可变会话配置，后续 CDO 修改不会改变当前 Session。 */
FSimulationRuntimeSettingsSnapshot FSimulationRuntimeSettingsSnapshot::Capture(
    const USimulationSettings& Settings)
{
    FSimulationRuntimeSettingsSnapshot Snapshot;
    Snapshot.SimulationMode = Settings.SimulationMode;
    Snapshot.FixedStepSeconds = FMath::Max(0.001, Settings.FixedStepSeconds);
    Snapshot.MaxPendingFrames = FMath::Max(1, Settings.MaxPendingFrames);
    Snapshot.MaxPendingAssemblyFrames = FMath::Max(1, Settings.MaxPendingAssemblyFrames);
    Snapshot.TerminalFrameHistoryCapacity = FMath::Max(1, Settings.TerminalFrameHistoryCapacity);
    Snapshot.DatasetRoot = USimulationSettings::ResolveDatasetRoot(Settings.DatasetRoot);
    Snapshot.FrameTimeoutSeconds = FMath::Max(0.1, Settings.FrameTimeoutSeconds);
    Snapshot.RandomSeed = Settings.RandomSeed;
    Snapshot.TargetCommittedFrames = FMath::Max<int64>(0, Settings.TargetCommittedFrames);
    Snapshot.ShutdownDrainTimeoutSeconds = FMath::Max(0.0, Settings.ShutdownDrainTimeoutSeconds);
    return Snapshot;
}
