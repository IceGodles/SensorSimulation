using UnrealBuildTool;

/// <summary>定义 SimulationRenderer 模块的编译规则、依赖关系与构建选项。</summary>
public class SimulationRenderer : ModuleRules
{
    /// <summary>根据目标平台初始化 SimulationRenderer 模块规则并声明公共、私有依赖。</summary>
    public SimulationRenderer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "SimulationCore"
        });
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Projects", "RenderCore", "RHI", "Renderer"
        });
    }
}
