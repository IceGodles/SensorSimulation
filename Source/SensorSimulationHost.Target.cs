using UnrealBuildTool;

public class SensorSimulationHostTarget : TargetRules
{
    public SensorSimulationHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("SensorSimulationHost");
    }
}
