#include "SemanticObjectComponent.h"
#include "InstanceCaptureRegistry.h"
#include "SemanticImageLabel.h"
#include "SimulationSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

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

/** 返回 OpaqueProxy 是否属于同一 Actor、已注册且自身不是透明材质。 */
bool USemanticObjectComponent::HasValidOpaqueLabelProxy() const
{
    if (TranslucentLabelPolicy != ETranslucentLabelPolicy::OpaqueProxy ||
        !OpaqueLabelProxy || OpaqueLabelProxy->GetOwner() != GetOwner() ||
        !OpaqueLabelProxy->IsRegistered())
    {
        return false;
    }
    for (int32 MaterialIndex = 0; MaterialIndex < OpaqueLabelProxy->GetNumMaterials(); ++MaterialIndex)
    {
        const UMaterialInterface* Material = OpaqueLabelProxy->GetMaterial(MaterialIndex);
        if (Material && IsTranslucentBlendMode(Material->GetBlendMode()))
        {
            return false;
        }
    }
    return true;
}

#if WITH_EDITOR
/**
 * 把 OpaqueProxy 的产品约束接入 UE Data Validation。
 *
 * 缺失代理、跨 Actor 和透明代理会直接产生 Invalid；未注册、找不到透明源或
 * 包围盒偏差过大属于资产制作警告，避免近似代理因保守阈值阻塞整个数据集构建。
 */
EDataValidationResult USemanticObjectComponent::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result = EDataValidationResult::Valid;
    if (TranslucentLabelPolicy != ETranslucentLabelPolicy::OpaqueProxy)
    {
        return CombineDataValidationResults(Result, Super::IsDataValid(Context));
    }

    if (!OpaqueLabelProxy)
    {
        Context.AddError(FText::FromString(TEXT("OpaqueProxy 策略必须指定 OpaqueLabelProxy。")));
        Result = EDataValidationResult::Invalid;
        return CombineDataValidationResults(Result, Super::IsDataValid(Context));
    }
    if (OpaqueLabelProxy->GetOwner() != GetOwner())
    {
        Context.AddError(FText::FromString(TEXT("OpaqueLabelProxy 必须与 SemanticObjectComponent 属于同一 Actor。")));
        Result = EDataValidationResult::Invalid;
    }

    bool bProxyUsesTranslucentMaterial = false;
    for (int32 MaterialIndex = 0; MaterialIndex < OpaqueLabelProxy->GetNumMaterials(); ++MaterialIndex)
    {
        const UMaterialInterface* Material = OpaqueLabelProxy->GetMaterial(MaterialIndex);
        bProxyUsesTranslucentMaterial |= Material && IsTranslucentBlendMode(Material->GetBlendMode());
    }
    if (bProxyUsesTranslucentMaterial)
    {
        Context.AddError(FText::FromString(TEXT("OpaqueLabelProxy 不能使用 Translucent 材质。")));
        Result = EDataValidationResult::Invalid;
    }
    if (!OpaqueLabelProxy->IsRegistered() && !OpaqueLabelProxy->IsTemplate())
    {
        Context.AddWarning(FText::FromString(TEXT("OpaqueLabelProxy 当前未注册，运行时不会参与标签捕获。")));
    }

    // 汇总同 Actor 上真实半透明图元的世界包围盒，用于发现明显错位或尺寸错误的代理。
    FBox TranslucentBounds(ForceInit);
    if (const AActor* Owner = GetOwner())
    {
        TInlineComponentArray<UPrimitiveComponent*> Primitives(Owner);
        for (const UPrimitiveComponent* Primitive : Primitives)
        {
            if (!Primitive || Primitive == OpaqueLabelProxy)
            {
                continue;
            }
            bool bTranslucent = false;
            for (int32 MaterialIndex = 0; MaterialIndex < Primitive->GetNumMaterials(); ++MaterialIndex)
            {
                const UMaterialInterface* Material = Primitive->GetMaterial(MaterialIndex);
                bTranslucent |= Material && IsTranslucentBlendMode(Material->GetBlendMode());
            }
            if (bTranslucent)
            {
                TranslucentBounds += Primitive->Bounds.GetBox();
            }
        }
    }

    if (!TranslucentBounds.IsValid)
    {
        Context.AddWarning(FText::FromString(TEXT("同一 Actor 上没有可用于比较 OpaqueLabelProxy 的半透明源图元。")));
    }
    else if (OpaqueLabelProxy->IsRegistered())
    {
        const FBox ProxyBounds = OpaqueLabelProxy->Bounds.GetBox();
        const FVector SourceSize = TranslucentBounds.GetSize();
        const FVector ProxySize = ProxyBounds.GetSize();
        const FVector CenterDelta = (ProxyBounds.GetCenter() - TranslucentBounds.GetCenter()).GetAbs();
        float MaxRelativeError = 0.0f;
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            MaxRelativeError = FMath::Max(MaxRelativeError,
                CenterDelta[Axis] / FMath::Max(SourceSize[Axis], 1.0));
            MaxRelativeError = FMath::Max(MaxRelativeError,
                FMath::Abs(ProxySize[Axis] - SourceSize[Axis]) / FMath::Max(SourceSize[Axis], 1.0));
        }
        if (MaxRelativeError > FMath::Clamp(OpaqueProxyBoundsTolerance, 0.0f, 1.0f))
        {
            Context.AddWarning(FText::Format(
                FText::FromString(TEXT("OpaqueLabelProxy 与半透明源包围盒的最大相对偏差为 {0}，超过容差 {1}。")),
                FText::AsNumber(MaxRelativeError),
                FText::AsNumber(OpaqueProxyBoundsTolerance)));
        }
    }

    return CombineDataValidationResults(Result, Super::IsDataValid(Context));
}
#endif
/** 恢复旧代理状态，避免策略热更新后残留标签专用可见性。 */
void USemanticObjectComponent::ReleaseOpaqueLabelProxyState()
{
    if (UPrimitiveComponent* PreviousProxy = AppliedOpaqueLabelProxy.Get())
    {
        UE::SensorSimulation::InstanceCapture::UnregisterOpaqueLabelProxy(PreviousProxy);
        UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(PreviousProxy);
        PreviousProxy->SetRenderCustomDepth(false);
        PreviousProxy->SetVisibleInSceneCaptureOnly(bAppliedProxyWasCaptureOnly);
    }
    AppliedOpaqueLabelProxy.Reset();
    bAppliedProxyWasCaptureOnly = false;
}

/** 接管有效代理，使其只对标签 SceneCapture 可见。 */
void USemanticObjectComponent::ApplyOpaqueLabelProxyState()
{
    // 代理隔离生命周期与标签激活策略分离：即使切到 Ignore，只要仍配置有效代理，
    // 它就继续保持“主视口隐藏、RGB/Depth SceneCapture 隐藏”，但不写 Semantic/Instance。
    UPrimitiveComponent* DesiredProxy = OpaqueLabelProxy.Get();
    if (!DesiredProxy || DesiredProxy->GetOwner() != GetOwner() || !DesiredProxy->IsRegistered())
    {
        DesiredProxy = nullptr;
    }
    if (DesiredProxy)
    {
        for (int32 MaterialIndex = 0; MaterialIndex < DesiredProxy->GetNumMaterials(); ++MaterialIndex)
        {
            const UMaterialInterface* Material = DesiredProxy->GetMaterial(MaterialIndex);
            if (Material && IsTranslucentBlendMode(Material->GetBlendMode()))
            {
                DesiredProxy = nullptr;
                break;
            }
        }
    }
    if (AppliedOpaqueLabelProxy.Get() != DesiredProxy)
    {
        ReleaseOpaqueLabelProxyState();
    }
    if (!DesiredProxy)
    {
        if (TranslucentLabelPolicy == ETranslucentLabelPolicy::OpaqueProxy)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("OpaqueProxy policy on '%s' requires a registered, opaque PrimitiveComponent on the same Actor."),
                *GetNameSafe(GetOwner()));
        }
        return;
    }
    if (!AppliedOpaqueLabelProxy.IsValid())
    {
        bAppliedProxyWasCaptureOnly = DesiredProxy->bVisibleInSceneCaptureOnly;
        AppliedOpaqueLabelProxy = DesiredProxy;
    }
    DesiredProxy->SetVisibleInSceneCaptureOnly(true);
    UE::SensorSimulation::InstanceCapture::RegisterOpaqueLabelProxy(DesiredProxy);
}
/** 公开应用运行时标签配置，确保策略切换在下一次 Capture 前完成。 */
void USemanticObjectComponent::ApplyCaptureConfiguration()
{
    ApplyCaptureRenderState();
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

/** 把语义模板和完整 InstanceId 同步到所属 Actor 的图元或显式标签代理。 */
void USemanticObjectComponent::ApplyCaptureRenderState()
{
    if (!GetOwner())
    {
        return;
    }

    ApplyOpaqueLabelProxyState();
    UPrimitiveComponent* ActiveProxy =
        TranslucentLabelPolicy == ETranslucentLabelPolicy::OpaqueProxy
            ? AppliedOpaqueLabelProxy.Get()
            : nullptr;
    uint8 ImageSemanticId = 0;
    const bool bValidImageId =
        UE::SensorSimulation::SemanticLabels::TryConvertToImageId(SemanticId, ImageSemanticId);
    const bool bShouldRenderSemantic = bRenderToSemanticCapture && bValidImageId;
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
        bool bIsTranslucent = false;
        for (int32 MaterialIndex = 0; MaterialIndex < Primitive->GetNumMaterials(); ++MaterialIndex)
        {
            const UMaterialInterface* Material = Primitive->GetMaterial(MaterialIndex);
            bIsTranslucent |= Material && IsTranslucentBlendMode(Material->GetBlendMode());
        }
        const bool bIsActiveProxy = Primitive == ActiveProxy;
        // A configured proxy may write labels only while a valid OpaqueProxy policy is active.
        const bool bIsConfiguredProxy = Primitive == OpaqueLabelProxy;
        const bool bEligibleForLabels = bIsActiveProxy || (!bIsTranslucent && !bIsConfiguredProxy);
        const bool bRenderSemanticPrimitive = bShouldRenderSemantic && bEligibleForLabels;
        Primitive->SetRenderCustomDepth(bRenderSemanticPrimitive);
        if (bRenderSemanticPrimitive)
        {
            Primitive->SetCustomDepthStencilValue(ImageSemanticId);
        }

        if (bRenderToInstanceCapture && InstanceId > 0 && Primitive->IsRegistered() && bEligibleForLabels)
        {
            if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Primitive))
            {
                const int32 InternalCount = FMath::Max(0, Instanced->GetInstanceCount());
                if (InternalCount == 0)
                {
                    UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
                }
                else if (NextInternalInstanceId + static_cast<uint64>(InternalCount) <=
                    static_cast<uint64>(InstanceId) + AllocatedInstanceIdCount)
                {
                    UE::SensorSimulation::InstanceCapture::RegisterPrimitive(
                        Primitive, static_cast<uint32>(NextInternalInstanceId), true,
                        static_cast<uint32>(InternalCount));
                    NextInternalInstanceId += static_cast<uint64>(InternalCount);
                }
                else
                {
                    UE::SensorSimulation::InstanceCapture::UnregisterPrimitive(Primitive);
                    UE_LOG(LogTemp, Error,
                        TEXT("ISM/HISM '%s' changed instance count after ID allocation; re-register the SemanticObjectComponent before Instance Capture."),
                        *GetNameSafe(Primitive));
                }
            }
            else
            {
                UE::SensorSimulation::InstanceCapture::RegisterPrimitive(Primitive, static_cast<uint32>(InstanceId));
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
