#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;
class UWorld;

namespace UE::SensorSimulation::InstanceCapture
{
/**
 * 把一个可渲染图元与完整 32 位 InstanceId 关联。
 *
 * 注册表只保存 FPrimitiveComponentId 和整数，不持有 UObject，也不复用
 * CustomStencil。独立 Instance Mesh Pass 会在每个 Draw 中查询并绑定该值。
 */
SIMULATIONRENDERER_API void RegisterPrimitive(
    const UPrimitiveComponent* Primitive,
    uint32 InstanceId,
    bool bUseInternalInstanceId = false,
    uint32 InternalInstanceCount = 1);

/**
 * 移除图元的 InstanceId 关联。
 *
 * 组件注销或不再参与 Instance Capture 时必须调用，避免新建 SceneProxy
 * 继续继承已经失效的对象身份。
 */
SIMULATIONRENDERER_API void UnregisterPrimitive(
    const UPrimitiveComponent* Primitive);

/** 游戏线程查询图元当前是否拥有有效 InstanceId，用于隔离未激活的标签代理。 */
SIMULATIONRENDERER_API bool IsPrimitiveRegistered(
    const UPrimitiveComponent* Primitive);

/** 登记只供 Semantic/Instance 使用的不透明标签代理。 */
SIMULATIONRENDERER_API void RegisterOpaqueLabelProxy(UPrimitiveComponent* Primitive);

/** 注销不透明标签代理；可安全重复调用。 */
SIMULATIONRENDERER_API void UnregisterOpaqueLabelProxy(UPrimitiveComponent* Primitive);

/** 返回当前 World 中有效的不透明标签代理，供 Camera Rig 隔离 RGB/Depth 捕获。 */
SIMULATIONRENDERER_API TArray<TWeakObjectPtr<UPrimitiveComponent>> GetOpaqueLabelProxies(
    const UWorld* World);
}
