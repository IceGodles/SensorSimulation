#include "SemanticObjectComponent.h"
#include "SimulationSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/** 构造并初始化 USemanticObjectComponent 的默认状态。 */
USemanticObjectComponent::USemanticObjectComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

/** 组件注册时同步语义模板，使非 PIE 编辑器捕获也能读取标签。 */
void USemanticObjectComponent::OnRegister()
{
    Super::OnRegister();
    ApplySemanticRenderState();
}

/** 在组件开始运行时完成注册或初始化工作。 */
void USemanticObjectComponent::BeginPlay()
{
    Super::BeginPlay();

    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->RegisterSemanticObject(*this);
        }
    }

    if (bRenderToSemanticCapture && GetOwner())
    {
        TArray<UPrimitiveComponent*> Primitives;
        GetOwner()->GetComponents<UPrimitiveComponent>(Primitives);
        for (UPrimitiveComponent* Primitive : Primitives)
        {
            // 为所属 Actor 的图元启用 CustomDepth
            Primitive->SetRenderCustomDepth(true);
            // 把语义 ID 限制到 `0..255` 后写入 CustomStencil
            Primitive->SetCustomDepthStencilValue(FMath::Clamp(SemanticId, 0, 255));
        }
    }
}

/** 在组件停止运行时注销自身并释放关联关系。 */
void USemanticObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->UnregisterSemanticObject(*this);
        }
    }
    Super::EndPlay(EndPlayReason);
}

/** 保存语义注册表分配的会话内实例编号。 */
void USemanticObjectComponent::SetAssignedInstanceId(uint32 InInstanceId)
{
    InstanceId = static_cast<int32>(InInstanceId);
}
/** 把语义捕获开关和 8 位标签同步到所属 Actor 的所有可渲染图元。 */
void USemanticObjectComponent::ApplySemanticRenderState()
{
    if (!GetOwner())
    {
        return;
    }

    TArray<UPrimitiveComponent*> Primitives;
    GetOwner()->GetComponents<UPrimitiveComponent>(Primitives);
    for (UPrimitiveComponent* Primitive : Primitives)
    {
        Primitive->SetRenderCustomDepth(bRenderToSemanticCapture);
        if (bRenderToSemanticCapture)
        {
            // CustomStencil 只有 8 位；完整 SemanticId 仍保留给 LiDAR 和 Ground Truth。
            Primitive->SetCustomDepthStencilValue(FMath::Clamp(SemanticId, 0, 255));
        }
    }
}

#if WITH_EDITOR
/** 编辑器属性变化后刷新 CustomStencil，避免保存调试图前必须重新进入 PIE。 */
void USemanticObjectComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    ApplySemanticRenderState();
}
#endif