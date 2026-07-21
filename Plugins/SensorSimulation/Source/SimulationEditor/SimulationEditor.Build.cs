using UnrealBuildTool;

/// <summary>定义 SimulationEditor 模块的编译规则、依赖关系与构建选项。</summary>
public class SimulationEditor : ModuleRules
{
    /// <summary>根据目标平台初始化 SimulationEditor 模块规则并声明公共、私有依赖。</summary>
    public SimulationEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "UnrealEd",
            "SimulationCore", "SimulationRuntime", "SimulationRenderer"
        });
    }
}
