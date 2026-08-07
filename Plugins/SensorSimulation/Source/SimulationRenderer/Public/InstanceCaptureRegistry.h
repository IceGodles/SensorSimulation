#pragma once

#include "CoreMinimal.h"

class UPrimitiveComponent;

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
}
