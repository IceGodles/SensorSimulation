#pragma once

#include "CoreMinimal.h"

/** 注册插件渲染资源与着色器目录的模块入口。 */
class SIMULATIONRENDERER_API FSimulationRendererModule : public IModuleInterface
{
public:
/** 在模块加载时注册插件着色器虚拟路径。 */
    virtual void StartupModule() override;
/** 执行模块卸载阶段的清理钩子。 */
    virtual void ShutdownModule() override;
};
