using UnrealBuildTool;

/// <summary>定义 SimulationRuntime 模块的编译规则、依赖关系与构建选项。</summary>
public class SimulationRuntime : ModuleRules
{
    /// <summary>根据目标平台初始化 SimulationRuntime 模块规则并声明公共、私有依赖。</summary>
    public SimulationRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "DeveloperSettings",
            "SimulationCore", "SimulationRenderer"
        });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "JsonUtilities", "Projects" });
    }
}
