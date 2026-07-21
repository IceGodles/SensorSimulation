#include "SimulationRenderer.h"
#include "SemanticCaptureViewExtension.h"
#include "Engine/Engine.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"

namespace
{
/** 模块持有 View Extension，保证后处理委托使用期间对象始终有效。 */
TSharedPtr<FSemanticCaptureViewExtension, ESPMode::ThreadSafe> SemanticCaptureViewExtension;

/** GEngine 创建后才能向 FSceneViewExtensions 注册扩展。 */
FDelegateHandle PostEngineInitHandle;

/** 在引擎完成初始化后创建 Semantic View Extension。 */
void InitializeSemanticCaptureViewExtension()
{
    if (!SemanticCaptureViewExtension.IsValid())
    {
        SemanticCaptureViewExtension = FSceneViewExtensions::NewExtension<FSemanticCaptureViewExtension>();
    }
}
}

/** 在模块加载时注册插件着色器虚拟路径，并安排引擎初始化后的 View Extension 注册。 */
void FSimulationRendererModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SensorSimulation"));
    if (Plugin.IsValid())
    {
        // Shader 路径必须在 PostConfigInit 阶段建立，保证全局 Shader 类型首次编译时即可解析虚拟路径。
        AddShaderSourceDirectoryMapping(
            TEXT("/Plugin/SensorSimulation"),
            FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
    }

    if (GEngine)
    {
        InitializeSemanticCaptureViewExtension();
    }
    else
    {
        PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&InitializeSemanticCaptureViewExtension);
    }
}

/** 注销延迟初始化委托和语义捕获扩展，阻止模块卸载后继续触发渲染回调。 */
void FSimulationRendererModule::ShutdownModule()
{
    if (PostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
        PostEngineInitHandle.Reset();
    }
    SemanticCaptureViewExtension.Reset();
}

IMPLEMENT_MODULE(FSimulationRendererModule, SimulationRenderer);