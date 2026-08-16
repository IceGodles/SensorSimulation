#include "InstanceCaptureRegistry.h"
#include "InstanceCaptureViewExtension.h"

#include "Components/PrimitiveComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeRWLock.h"
#include "PrimitiveSceneProxy.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogInstanceCaptureRegistry, Log, All);

namespace UE::SensorSimulation::InstanceCapture
{
namespace
{
/** 同时保护游戏线程注册和渲染线程查询。 */
FRWLock RegistryLock;

/** 稳定组件标识到普通/逐内部实例身份绑定的映射。 */
TMap<FPrimitiveComponentId, FPrimitiveInstanceBinding> PrimitiveInstanceBindings;

/** 当前有效的不透明标签代理；弱引用避免延长组件生命周期。 */
TSet<TWeakObjectPtr<UPrimitiveComponent>> OpaqueLabelProxies;

/** 每个图元最近一次的 R15 支持状态，用于避免热更新时重复输出相同诊断。 */
enum class EPrimitiveSupport : uint8
{
    Supported,
    PerInternalInstance,
    Nanite,
    UnsupportedTranslucent
};

/** 最近一次分类结果；只保存组件 ID，不持有 UObject。 */
TMap<FPrimitiveComponentId, EPrimitiveSupport> PrimitiveSupportStates;

/**
 * 判断图元应走普通 Mesh Pass、Nanite VisBuffer Pass，还是透明对象策略。
 * Masked 材质受支持；Nanite 由专用路径解码；Translucent 暂不输出确定性单标签。
 */
EPrimitiveSupport ClassifyPrimitive(const UPrimitiveComponent& Primitive)
{
    if (const UStaticMeshComponent* StaticMesh = Cast<UStaticMeshComponent>(&Primitive))
    {
        if (StaticMesh->HasValidNaniteData())
        {
            // Nanite ISM/HISM 仍需逐内部实例 ID；专用像素着色器会读取 GPU Scene RelativeId。
            return Primitive.IsA<UInstancedStaticMeshComponent>()
                ? EPrimitiveSupport::PerInternalInstance
                : EPrimitiveSupport::Nanite;
        }
    }
    else if (const USkinnedMeshComponent* SkinnedMesh = Cast<USkinnedMeshComponent>(&Primitive))
    {
        if (SkinnedMesh->HasValidNaniteData())
        {
            return EPrimitiveSupport::Nanite;
        }
    }

    for (int32 MaterialIndex = 0; MaterialIndex < Primitive.GetNumMaterials(); ++MaterialIndex)
    {
        const UMaterialInterface* Material = Primitive.GetMaterial(MaterialIndex);
        if (Material && IsTranslucentBlendMode(Material->GetBlendMode()))
        {
            return EPrimitiveSupport::UnsupportedTranslucent;
        }
    }

    return Primitive.IsA<UInstancedStaticMeshComponent>()
        ? EPrimitiveSupport::PerInternalInstance
        : EPrimitiveSupport::Supported;
}

const TCHAR* DescribeSupport(const EPrimitiveSupport Support)
{
    switch (Support)
    {
    case EPrimitiveSupport::PerInternalInstance:
        return TEXT("ISM/HISM writes BaseInstanceId + GPU Scene RelativeId for each internal instance");
    case EPrimitiveSupport::Nanite:
        return TEXT("Nanite is exported from VisBuffer through the dedicated Instance pass");
    case EPrimitiveSupport::UnsupportedTranslucent:
        return TEXT("translucent surfaces have no deterministic single-label visibility policy");
    default:
        return TEXT("supported");
    }
}

/**
 * 普通 SceneCapture 目标到独立整数输出目标的非拥有型映射。
 *
 * Camera Rig 负责保证两端目标在注销前仍然存活；查询时取得的 FTextureRHIRef
 * 会增加底层 RHI 引用计数，使本次 RDG Pass 执行期间纹理保持有效。
 */
TMap<const FRenderTarget*, const FRenderTarget*> CaptureTargets;
}

/** 注册图元的完整 InstanceId；0 保留给背景，因此按注销处理。 */
void RegisterPrimitive(
    const UPrimitiveComponent* Primitive,
    const uint32 InstanceId,
    const bool bUseInternalInstanceId,
    const uint32 InternalInstanceCount)
{
    if (!Primitive)
    {
        return;
    }

    const FPrimitiveComponentId PrimitiveId = Primitive->GetPrimitiveSceneId();
    const EPrimitiveSupport Support = ClassifyPrimitive(*Primitive);
    FWriteScopeLock ScopeLock(RegistryLock);

    const EPrimitiveSupport* PreviousSupport = PrimitiveSupportStates.Find(PrimitiveId);
    const bool bSupportChanged = !PreviousSupport || *PreviousSupport != Support;
    PrimitiveSupportStates.Add(PrimitiveId, Support);

    if (Support == EPrimitiveSupport::UnsupportedTranslucent)
    {
        PrimitiveInstanceBindings.Remove(PrimitiveId);
        if (bSupportChanged)
        {
            UE_LOG(LogInstanceCaptureRegistry, Warning,
                TEXT("Excluded primitive '%s' from Instance Capture: %s."),
                *GetNameSafe(Primitive),
                DescribeSupport(Support));
        }
        return;
    }

    if (InstanceId == 0)
    {
        PrimitiveInstanceBindings.Remove(PrimitiveId);
    }
    else
    {
        FPrimitiveInstanceBinding Binding;
        Binding.BaseInstanceId = InstanceId;
        Binding.bUseInternalInstanceId =
            bUseInternalInstanceId && Support == EPrimitiveSupport::PerInternalInstance;
        Binding.InternalInstanceCount = Binding.bUseInternalInstanceId
            ? FMath::Max(1u, InternalInstanceCount)
            : 1u;
        PrimitiveInstanceBindings.Add(PrimitiveId, Binding);
        if (bSupportChanged && Support == EPrimitiveSupport::PerInternalInstance)
        {
            UE_LOG(LogInstanceCaptureRegistry, Display,
                TEXT("Registered primitive '%s' with per-internal-instance base %u: %s."),
                *GetNameSafe(Primitive),
                InstanceId,
                DescribeSupport(Support));
        }
        UE_LOG(LogInstanceCaptureRegistry, VeryVerbose,
            TEXT("Registered primitive %u with InstanceId %u."),
            PrimitiveId.PrimIDValue,
            InstanceId);
    }
}

/** 注销图元身份，不延长 PrimitiveComponent 或 SceneProxy 生命周期。 */
void UnregisterPrimitive(const UPrimitiveComponent* Primitive)
{
    if (!Primitive)
    {
        return;
    }

    FWriteScopeLock ScopeLock(RegistryLock);
    const FPrimitiveComponentId PrimitiveId = Primitive->GetPrimitiveSceneId();
    PrimitiveInstanceBindings.Remove(PrimitiveId);
    PrimitiveSupportStates.Remove(PrimitiveId);
}

/** 返回图元当前是否有非零 Instance 绑定；读锁允许与渲染线程查询并发。 */
bool IsPrimitiveRegistered(const UPrimitiveComponent* Primitive)
{
    if (!Primitive)
    {
        return false;
    }
    FReadScopeLock ScopeLock(RegistryLock);
    const FPrimitiveInstanceBinding* Binding =
        PrimitiveInstanceBindings.Find(Primitive->GetPrimitiveSceneId());
    return Binding && Binding->BaseInstanceId != 0;
}

/** 登记只供标签捕获使用的不透明代理。 */
void RegisterOpaqueLabelProxy(UPrimitiveComponent* Primitive)
{
    if (!Primitive)
    {
        return;
    }
    FWriteScopeLock ScopeLock(RegistryLock);
    OpaqueLabelProxies.Add(Primitive);
}

/** 注销标签代理并顺便清理失效弱引用。 */
void UnregisterOpaqueLabelProxy(UPrimitiveComponent* Primitive)
{
    FWriteScopeLock ScopeLock(RegistryLock);
    if (Primitive)
    {
        OpaqueLabelProxies.Remove(Primitive);
    }
    for (auto It = OpaqueLabelProxies.CreateIterator(); It; ++It)
    {
        if (!It->IsValid())
        {
            It.RemoveCurrent();
        }
    }
}

/** 返回指定 World 的有效标签代理快照。 */
TArray<TWeakObjectPtr<UPrimitiveComponent>> GetOpaqueLabelProxies(const UWorld* World)
{
    TArray<TWeakObjectPtr<UPrimitiveComponent>> Result;
    FReadScopeLock ScopeLock(RegistryLock);
    for (const TWeakObjectPtr<UPrimitiveComponent>& Proxy : OpaqueLabelProxies)
    {
        if (Proxy.IsValid() && Proxy->GetWorld() == World)
        {
            Result.Add(Proxy);
        }
    }
    return Result;
}
/** 登记用于识别 Instance ViewFamily 的普通 Capture Target 和整数输出 Target。 */
void RegisterCaptureTarget(
    const FRenderTarget* CaptureTarget,
    const FRenderTarget* OutputTarget)
{
    if (!CaptureTarget || !OutputTarget)
    {
        return;
    }

    FWriteScopeLock ScopeLock(RegistryLock);
    CaptureTargets.Add(CaptureTarget, OutputTarget);
    UE_LOG(LogInstanceCaptureRegistry, VeryVerbose,
        TEXT("Registered capture target pair: capture=%p output=%p."),
        CaptureTarget,
        OutputTarget);
}

/** 注销 Capture Target；整数输出目标由 Camera Rig 自己管理生命周期。 */
void UnregisterCaptureTarget(const FRenderTarget* CaptureTarget)
{
    if (!CaptureTarget)
    {
        return;
    }

    FWriteScopeLock ScopeLock(RegistryLock);
    CaptureTargets.Remove(CaptureTarget);
}

/** 在锁内取得强 RHI 引用，防止返回后目标资源恰好被销毁。 */
FTextureRHIRef FindOutputTexture_RenderThread(const FRenderTarget* CaptureTarget)
{
    check(IsInRenderingThread());
    FReadScopeLock ScopeLock(RegistryLock);
    const FRenderTarget* const* OutputTarget = CaptureTargets.Find(CaptureTarget);
    return OutputTarget && *OutputTarget
        ? (*OutputTarget)->GetRenderTargetTexture()
        : FTextureRHIRef();
}

/** 用 SceneProxy 的稳定组件标识查询每个 Draw 应写出的 uint32 InstanceId。 */
FPrimitiveInstanceBinding FindInstanceBinding_RenderThread(
    const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
    if (!PrimitiveSceneProxy)
    {
        return {};
    }

    FReadScopeLock ScopeLock(RegistryLock);
    const FPrimitiveInstanceBinding* Binding =
        PrimitiveInstanceBindings.Find(PrimitiveSceneProxy->GetPrimitiveComponentId());
    return Binding ? *Binding : FPrimitiveInstanceBinding{};
}
}
