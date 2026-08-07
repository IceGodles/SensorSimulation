using UnrealBuildTool;

/// <summary>定义 SimulationCore 模块的编译规则、依赖关系与构建选项。</summary>
public class SimulationCore : ModuleRules
{
    /// <summary>根据目标平台初始化 SimulationCore 模块规则并声明公共、私有依赖。</summary>
    public SimulationCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
    }
}
