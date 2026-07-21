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
            // 自定义模板缓冲仅有 8 位，因此渲染标签需限制在 0..255；完整语义 ID 仍保存在组件中。
            Primitive->SetRenderCustomDepth(true);
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
