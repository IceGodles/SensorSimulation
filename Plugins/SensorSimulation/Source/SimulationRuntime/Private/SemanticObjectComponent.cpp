#include "SemanticObjectComponent.h"
#include "InstanceCaptureRegistry.h"
#include "SemanticImageLabel.h"
#include "SimulationSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
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
    ApplyCaptureRenderState();
}

/** 组件注销前删除 Instance 图元映射，避免注册表持有已销毁组件身份。 */
void USemanticObjectComponent::OnUnregister()
{
    UnregisterInstancePrimitives();
    Super::OnUnregister();
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

    ApplyCaptureRenderState();
}

/** 在组件停止运行时注销自身并释放关联关系。 */
void USemanticObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 先停止 Instance Mesh Pass 查询该对象，再移除上层语义注册项。
    UnregisterInstancePrimitives();
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
void USemanticObjectComponent::SetAssignedInstanceId(
    const uint32 InInstanceId,
    const uint32 InAllocatedInstanceIdCount)
{
    InstanceId = static_cast<int64>(InInstanceId);
    AllocatedInstanceIdCount = FMath::Max(1u, InAllocatedInstanceIdCount);
    // 分配发生在 BeginPlay 注册期间；立即刷新，保证第一帧捕获就能读取 InstanceId。
    ApplyCaptureRenderState();
}

/** 计算 Actor ID 加全部 ISM/HISM 内部实例所需的连续编号数量。 */
uint32 USemanticObjectComponent::GetRequiredInstanceIdCount() const
{
    if (!GetOwner())
    {
        return 1;
    }

    uint64 RequiredCount = 1;
    TArray<UInstancedStaticMeshComponent*> InstancedPrimitives;
    GetOwner()->GetComponents<UInstancedStaticMeshComponent>(InstancedPrimitives);
    for (const UInstancedStaticMeshComponent* Primitive : InstancedPrimitives)
    {
        RequiredCount += static_cast<uint64>(FMath::Max(0, Primitive->GetInstanceCount()));
    }
    return static_cast<uint32>(FMath::Min<uint64>(RequiredCount, MAX_uint32));
}

/** 把语义模板和完整 InstanceId 同步到所属 Actor 的所有可渲染图元。 */
void USemanticObjectComponent::ApplyCaptureRenderState()
{
    if (!GetOwner())
    {
        return;
    }

    uint8 ImageSemanticId = 0;
    const bool bValidImageId =
        UE::SensorSimulation::SemanticLabels::TryConvertToImageId(SemanticId, ImageSemanticId);
    const bool bShouldRender = bRenderToSemanticCapture && bValidImageId;
    if (bRenderToSemanticCapture && !bValidImageId)
    {
        UE_LOG(LogTemp, Error,
            TEXT("SemanticId %d on '%s' is invalid for the 8-bit image channel; valid object IDs are 1..255. ")
            TEXT("The object is excluded from Semantic images but retains its full ID for LiDAR/Ground Truth."),
            SemanticId,
            *GetNameSafe(GetOwner()));
    }

    TArray<UPrimitiveComponent*> Primitives;
    GetOwner()->GetComponents<UPrimitiveComponent>(Primitives);
    uint64 NextInternalInstanceId = static_cast<uint64>(InstanceId) + 1u;
    for (UPrimitiveComponent* Primitive : Primitives)
    {
        // 为所属 Actor 的图元启用 CustomDepth
        Primitive->SetRenderCustomDepth(bShouldRender);
        if (bShouldRender)
        {
            // CustomStencil 只有 8 位；完整 SemanticId 仍保留给 LiDAR 和 Ground Truth。
            // 有效 ID 原样写入，不再把非法值静默 Clamp 成其他类别。
            Primitive->SetCustomDepthStencilValue(ImageSemanticId);
        }

        // InstanceId 通过独立图元注册表绑定到每个 Draw，不进入 8 位 CustomStencil。
        if (bRenderToInstanceCapture && InstanceId > 0 && Primitive->IsRegistered())
        {
            if (const UInstancedStaticMeshComponent* Instanced =
                Cast<UInstancedStaticMeshComponent>(Primitive))
            {
                const int32 InternalCount = FMath::Max(0, Instanced->GetInstanceCount());
                if (InternalCount == 0)
                {
                    // 空 ISM/HISM 没有可写像素，也不应被误报为热更新后的数量越界。
                    UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
                }
                else if (
                    NextInternalInstanceId + static_cast<uint64>(InternalCount) <=
                    static_cast<uint64>(InstanceId) + AllocatedInstanceIdCount)
                {
                    UE::SensorSimulation::InstanceCapture::RegisterPrimitive(
                        Primitive,
                        static_cast<uint32>(NextInternalInstanceId),
                        true,
                        static_cast<uint32>(InternalCount));
                    NextInternalInstanceId += static_cast<uint64>(InternalCount);
                }
                else
                {
                    UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
                    UE_LOG(LogTemp, Error,
                        TEXT("ISM/HISM '%s' changed instance count after ID allocation; ")
                        TEXT("re-register the SemanticObjectComponent before Instance Capture."),
                        *GetNameSafe(Primitive));
                }
            }
            else
            {
                UE::SensorSimulation::InstanceCapture::RegisterPrimitive(
                    Primitive,
                    static_cast<uint32>(InstanceId));
            }
        }
        else
        {
            UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
        }
    }
}

/** 从 Instance 注册表移除 Actor 的全部图元；可安全重复调用。 */
void USemanticObjectComponent::UnregisterInstancePrimitives()
{
    if (!GetOwner())
    {
        return;
    }

    TArray<UPrimitiveComponent*> Primitives;
    GetOwner()->GetComponents<UPrimitiveComponent>(Primitives);
    for (UPrimitiveComponent* Primitive : Primitives)
    {
        UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
    }
}

#if WITH_EDITOR
/** 编辑器属性变化后刷新 Semantic/Instance 状态，避免保存调试结果前必须重新进入 PIE。 */
void USemanticObjectComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    ApplyCaptureRenderState();
}
#endif