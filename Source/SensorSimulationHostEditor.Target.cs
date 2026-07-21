using UnrealBuildTool;

public class SensorSimulationHostEditorTarget : TargetRules
{
    public SensorSimulationHostEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("SensorSimulationHost");
    }
}
