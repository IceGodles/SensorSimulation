#include "SimulationRenderer.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

/** 在模块加载时注册插件着色器虚拟路径。 */
void FSimulationRendererModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SensorSimulation"));
    if (Plugin.IsValid())
    {
        AddShaderSourceDirectoryMapping(
            TEXT("/Plugin/SensorSimulation"),
/** 执行 Combine 对应的接口行为。 */
            FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
    }
}

/** 执行模块卸载阶段的清理钩子。 */
void FSimulationRendererModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FSimulationRendererModule, SimulationRenderer);
