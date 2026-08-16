#include "CameraRigComponent.h"
#include "InstanceCaptureTarget.h"
#include "InstanceCaptureRegistry.h"
#include "InstanceCaptureViewExtension.h"
#include "SemanticCaptureViewExtension.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Serialization/BufferArchive.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraRigDebug, Log, All);

static void WaitForDebugCaptureShaders()
{
    if (GShaderCompilingManager)
    {
        // 同步调试导出必须等待新建验收材质完成编译，否则 UE 会把尚未就绪的材质显示为棋盘格回退材质。
        // 正式 SubmitCapture/异步 Readback 链路不调用此函数，因此连续采集不会被 Shader 编译等待阻塞。
        GShaderCompilingManager->FinishAllCompilation();
    }
}

/** 构造并初始化 UCameraRigComponent 的默认状态。 */
UCameraRigComponent::UCameraRigComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // 默认提供RGB通道和Semantic通道
    FCameraChannelConfig Rgb;
    Rgb.ChannelType = ECameraChannelType::Rgb;
    Channels.Add(Rgb);

    FCameraChannelConfig Semantic;
    Semantic.ChannelType = ECameraChannelType::Semantic;
    Semantic.bForceLinearGamma = true;
    Channels.Add(Semantic);
}

/** 组件注册后根据配置创建运行时捕获通道。 */
void UCameraRigComponent::OnRegister()
{
    Super::OnRegister();
    ImageReadbackManager = MakeUnique<FImageReadbackManager>(MaxPendingReadbacks);
    BuildChannels();
}

/** 组件注销前销毁动态创建的捕获通道。 */
void UCameraRigComponent::OnUnregister()
{
    // 先排队释放读回对象，再销毁 Render Target，保持渲染命令的资源使用顺序。
    ImageReadbackManager.Reset();
    // Manager 的 Release 命令已经先排队；现在可以注销并放开 Readback 保活的旧 Target。
    ReleaseRetiredTargetsIfSafe();
    DestroyChannels();
    Super::OnUnregister();
}

#if WITH_EDITOR
/** 编辑器属性变化后立即应用配置，避免必须重新注册组件才能看到结果。 */
void UCameraRigComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    if (IsRegistered())
    {
        // Details 面板修改数组、分辨率、FOV 或 Gamma 后立即执行同一套运行时 Diff。
        ApplyConfiguration();
    }
}
#endif

/** 返回普通 SceneCapture 目标实际需要的 RenderTarget 格式。 */
static ETextureRenderTargetFormat GetChannelRenderTargetFormat(const FCameraChannelConfig& Config)
{
    return Config.ChannelType == ECameraChannelType::Depth
        ? ETextureRenderTargetFormat::RTF_RGBA32f
        : ETextureRenderTargetFormat::RTF_RGBA8;
}

/** Semantic 和 Instance 都是数值标签，必须绕过伽马变换。 */
static bool GetEffectiveLinearGamma(const FCameraChannelConfig& Config)
{
    return Config.ChannelType == ECameraChannelType::Semantic ||
        Config.ChannelType == ECameraChannelType::Instance ||
        Config.bForceLinearGamma;
}

/**
 * 初始化正式输出目标。
 *
 * 普通通道继续使用 UObject RenderTarget；Instance 的 PF_R32_UINT 由独立
 * FInstanceCaptureTarget 创建，因此本函数不接受 Instance。
 */
static void InitializeOutputTarget(
    UTextureRenderTarget2D& Target,
    const FCameraChannelConfig& Config)
{
    check(Config.ChannelType != ECameraChannelType::Instance);
    Target.bForceLinearGamma = GetEffectiveLinearGamma(Config);
    Target.ClearColor = FLinearColor::Black;
    Target.RenderTargetFormat = GetChannelRenderTargetFormat(Config);
    Target.InitAutoFormat(Config.Resolution.X, Config.Resolution.Y);
    Target.UpdateResourceImmediate(true);
}

/**
 * 创建 Instance SceneCapture 的普通颜色工作目标。
 *
 * SceneCapture 仍需一张常规颜色纹理来建立 ViewFamily、可见性和 SceneDepth；
 * 独立 Instance Mesh Pass 随后将整数标签写入另一张 PF_R32_UINT 正式输出。
 */
/** 创建并排队初始化 Instance 正式 PF_R32_UINT 输出。 */
static TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe> CreateInstanceOutputTarget(
    const FIntPoint Resolution)
{
    TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe> Target =
        MakeShared<FInstanceCaptureTarget, ESPMode::ThreadSafe>(Resolution);
    BeginInitResource(Target.Get());
    return Target;
}

/** 排队释放整数 RHI 资源，并让最后一个 SharedPtr 活到 Release 命令之后。 */
static void ReleaseInstanceOutputTarget(
    TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe>& Target)
{
    if (!Target.IsValid())
    {
        return;
    }

    TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe> KeepAlive = MoveTemp(Target);
    BeginReleaseResource(KeepAlive.Get());
    ENQUEUE_RENDER_COMMAND(ReleaseInstanceCaptureTargetKeepAlive)(
        [KeepAlive = MoveTemp(KeepAlive)](FRHICommandListImmediate&) mutable
        {
            // 此命令排在 ReleaseResource 后；离开作用域时才允许删除资源对象。
            KeepAlive.Reset();
        });
}

static UTextureRenderTarget2D* CreateInstanceCaptureTarget(
    UObject* Outer,
    const FIntPoint Resolution)
{
    UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Outer);
    Target->bForceLinearGamma = true;
    Target->ClearColor = FLinearColor::Black;
    Target->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
    Target->InitAutoFormat(Resolution.X, Resolution.Y);
    Target->UpdateResourceImmediate(true);
    return Target;
}

/** 只比较会改变该通道运行时资源或捕获行为的字段。 */
static bool AreChannelConfigsEqual(const FCameraChannelConfig& A, const FCameraChannelConfig& B)
{
    return A.ChannelGuid == B.ChannelGuid &&
        A.ChannelType == B.ChannelType &&
        A.Resolution == B.Resolution &&
        A.bEnabled == B.bEnabled &&
        A.bForceLinearGamma == B.bForceLinearGamma;
}

/**
 * 收集本轮允许创建的正式通道。
 *
 * 有效性只由启用状态、尺寸和 ChannelGuid 决定；ChannelType 不再承担唯一身份。
 * 因此同一 Rig 可以安全保留多条相同模态配置。
 */
static void GatherEffectiveChannelConfigs(
    const TArray<FCameraChannelConfig>& Channels,
    TArray<const FCameraChannelConfig*>& OutConfigs,
    const bool bLogDiagnostics)
{
    OutConfigs.Reset();
    for (const FCameraChannelConfig& Config : Channels)
    {
        if (!Config.bEnabled || Config.Resolution.X <= 0 || Config.Resolution.Y <= 0)
        {
            continue;
        }

        if (!Config.ChannelGuid.IsValid())
        {
            if (bLogDiagnostics)
            {
                UE_LOG(LogCameraRigDebug, Warning, TEXT("Ignoring camera channel with invalid ChannelGuid."));
            }
            continue;
        }
        OutConfigs.Add(&Config);
    }
}

/** 为缺失或重复的通道身份生成新 GUID，保证数组重排后仍能匹配原运行时资源。 */
void UCameraRigComponent::EnsureChannelGuids()
{
    bool bNeedsRegeneration = false;
    TSet<FGuid> UsedGuids;
    for (const FCameraChannelConfig& Config : Channels)
    {
        if (!Config.ChannelGuid.IsValid() || UsedGuids.Contains(Config.ChannelGuid))
        {
            bNeedsRegeneration = true;
        }
        UsedGuids.Add(Config.ChannelGuid);
    }

    if (!bNeedsRegeneration)
    {
        return;
    }

#if WITH_EDITOR
    const UWorld* World = GetWorld();
    const bool bIsEditableWorld = World && !World->IsGameWorld();
    if (bIsEditableWorld && !HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
    {
        // GUID 是序列化配置的一部分。先记录事务，再修改并标脏包，
        // 让旧关卡首次迁移出的身份能够随下一次保存持久化，而不是每次打开都重新生成。
        Modify();
    }
#endif

    UsedGuids.Reset();
    for (FCameraChannelConfig& Config : Channels)
    {
        if (!Config.ChannelGuid.IsValid() || UsedGuids.Contains(Config.ChannelGuid))
        {
            Config.ChannelGuid = FGuid::NewGuid();
        }
        UsedGuids.Add(Config.ChannelGuid);
    }

#if WITH_EDITOR
    if (bIsEditableWorld && !HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
    {
        MarkPackageDirty();
    }
#endif
}

/** 为每个有效配置创建场景捕获组件和匹配的渲染目标。 */
void UCameraRigComponent::BuildChannels()
{
    DestroyChannels();
    bHasAppliedConfiguration = false;
    ApplyConfiguration();
}

/** 判断当前运行时状态是否已经与可编辑配置完全一致。 */
bool UCameraRigComponent::IsConfigurationCurrent() const
{
    if (!bHasAppliedConfiguration ||
        AppliedSensorName != SensorName ||
        !FMath::IsNearlyEqual(AppliedHorizontalFovDegrees, HorizontalFovDegrees) ||
        AppliedMaxPendingReadbacks != FMath::Max(1, MaxPendingReadbacks))
    {
        return false;
    }

    TArray<const FCameraChannelConfig*> EffectiveConfigs;
    GatherEffectiveChannelConfigs(Channels, EffectiveConfigs, false);
    if (EffectiveConfigs.Num() != RuntimeChannels.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < EffectiveConfigs.Num(); ++Index)
    {
        if (!RuntimeChannels.IsValidIndex(Index) ||
            !AreChannelConfigsEqual(RuntimeChannels[Index].Config, *EffectiveConfigs[Index]) ||
            !RuntimeChannels[Index].Capture ||
            (EffectiveConfigs[Index]->ChannelType == ECameraChannelType::Instance
                ? !RuntimeChannels[Index].CaptureTarget || !RuntimeChannels[Index].InstanceTarget.IsValid()
                : !RuntimeChannels[Index].Target))
        {
            return false;
        }
    }
    return true;
}

/** 创建一个尚不存在的 Capture/RenderTarget 通道。 */
void UCameraRigComponent::CreateChannel(FChannelRuntime& Runtime, const FCameraChannelConfig& Config)
{
    Runtime.Config = Config;
    Runtime.Capture = NewObject<USceneCaptureComponent2D>(GetOwner());
    Runtime.Capture->SetupAttachment(this);
    Runtime.Capture->RegisterComponent();
    ++ResourceStats.CreatedCaptureComponents;

    if (Config.ChannelType == ECameraChannelType::Instance)
    {
        Runtime.InstanceTarget = CreateInstanceOutputTarget(Config.Resolution);
        Runtime.CaptureTarget = CreateInstanceCaptureTarget(this, Config.Resolution);
        ResourceStats.CreatedRenderTargets += 2;
    }
    else
    {
        Runtime.Target = NewObject<UTextureRenderTarget2D>(this);
        InitializeOutputTarget(*Runtime.Target, Config);
        ++ResourceStats.CreatedRenderTargets;
    }

    ConfigureCapture(Runtime);
    if (Config.ChannelType == ECameraChannelType::Semantic)
    {
        // 显式登记新目标，使 View Extension 只处理真正的 Semantic ViewFamily。
        UE::SensorSimulation::SemanticCapture::RegisterTarget(
            Runtime.Target->GameThread_GetRenderTargetResource());
    }
    else if (Config.ChannelType == ECameraChannelType::Instance)
    {
        // 目标对是 Instance View 的显式身份，同时告诉 Mesh Pass 应写入哪张整数纹理。
        UE::SensorSimulation::InstanceCapture::RegisterCaptureTarget(
            Runtime.CaptureTarget->GameThread_GetRenderTargetResource(),
            Runtime.InstanceTarget.Get());
    }
}

/** 将旧 Target 延迟到 Readback 排空后释放，避免热更新破坏在途命令。 */
void UCameraRigComponent::RetireTarget(
    TObjectPtr<UTextureRenderTarget2D>& Target,
    const bool bSemanticTarget)
{
    if (!Target)
    {
        return;
    }

    if (ImageReadbackManager && ImageReadbackManager->GetPendingCount() > 0)
    {
        // Pending 任务可能仍通过旧 FTextureRenderTargetResource 排队 GPU Copy
        // 数组负责继续持有旧 RT
        RetiredTargets.Add(Target);
        if (bSemanticTarget)
        {
            // 旧 Semantic Capture 也可能尚未执行，必须等它完成后再从身份注册表移除。
            RetiredSemanticTargets.Add(Target);
        }
    }
    else if (bSemanticTarget)
    {
        UE::SensorSimulation::SemanticCapture::UnregisterTarget(
            Target->GameThread_GetRenderTargetResource());
    }

    Target = nullptr;
    ++ResourceStats.DestroyedRenderTargets;
}

/**
 * 按通道语义退休全部目标。
 *
 * Instance 的普通 CaptureTarget 是目标对的注册键，因此必须和正式整数输出一起保活，
 * 直到可能引用旧 View/Readback 的命令排空。
 */
void UCameraRigComponent::RetireChannelTargets(FChannelRuntime& Runtime)
{
    const bool bHasPendingReadback =
        ImageReadbackManager && ImageReadbackManager->GetPendingCount() > 0;
    if (Runtime.CaptureTarget)
    {
        if (bHasPendingReadback)
        {
            RetiredInstanceCaptureTargets.Add(Runtime.CaptureTarget);
        }
        else
        {
            UE::SensorSimulation::InstanceCapture::UnregisterCaptureTarget(
                Runtime.CaptureTarget->GameThread_GetRenderTargetResource());
        }
        RetireTarget(Runtime.CaptureTarget, false);
    }

    if (Runtime.InstanceTarget.IsValid())
    {
        if (bHasPendingReadback)
        {
            RetiredInstanceTargets.Add(MoveTemp(Runtime.InstanceTarget));
        }
        else
        {
            ReleaseInstanceOutputTarget(Runtime.InstanceTarget);
        }
    }

    RetireTarget(
        Runtime.Target,
        Runtime.Config.ChannelType == ECameraChannelType::Semantic);
}

/** 在没有 Pending Readback 时注销并释放全部退休 Target。 */
void UCameraRigComponent::ReleaseRetiredTargetsIfSafe()
{
    if (ImageReadbackManager && ImageReadbackManager->GetPendingCount() > 0)
    {
        return;
    }

    for (UTextureRenderTarget2D* SemanticTarget : RetiredSemanticTargets)
    {
        if (SemanticTarget)
        {
            UE::SensorSimulation::SemanticCapture::UnregisterTarget(
                SemanticTarget->GameThread_GetRenderTargetResource());
        }
    }
    RetiredSemanticTargets.Reset();

    for (UTextureRenderTarget2D* InstanceCaptureTarget : RetiredInstanceCaptureTargets)
    {
        if (InstanceCaptureTarget)
        {
            UE::SensorSimulation::InstanceCapture::UnregisterCaptureTarget(
                InstanceCaptureTarget->GameThread_GetRenderTargetResource());
        }
    }
    RetiredInstanceCaptureTargets.Reset();
    for (TSharedPtr<FInstanceCaptureTarget, ESPMode::ThreadSafe>& InstanceTarget : RetiredInstanceTargets)
    {
        ReleaseInstanceOutputTarget(InstanceTarget);
    }
    RetiredInstanceTargets.Reset();
    RetiredTargets.Reset();
}

/** 销毁单个运行时通道，并按正确顺序注销 Semantic Target。 */
void UCameraRigComponent::DestroyChannel(FChannelRuntime& Runtime)
{
    if (Runtime.Capture)
    {
        Runtime.Capture->DestroyComponent();
        Runtime.Capture = nullptr;
        ++ResourceStats.DestroyedCaptureComponents;
    }

    // Capture 先停止产生新命令；全部目标根据 Pending 状态立即释放或延迟退休。
    RetireChannelTargets(Runtime);
}

/** 比较编辑器/蓝图中的最新配置与当前运行时资源，只重建真正受影响的 Render Target，尽量复用已有的 Capture 和 RT。 */
bool UCameraRigComponent::ApplyConfiguration()
{
    check(IsInGameThread());
    ++ResourceStats.ConfigurationApplyCount;
    EnsureChannelGuids();
    ReleaseRetiredTargetsIfSafe();

    if (!ImageReadbackManager)
    {
        ImageReadbackManager = MakeUnique<FImageReadbackManager>(MaxPendingReadbacks);
    }
    // 容量变化不重建 Manager 或取消在途任务；新限制只作用于后续 Enqueue。
    ImageReadbackManager->SetCapacity(MaxPendingReadbacks);

    // 即使有效资源没有变化，也先报告重复/未实现通道，避免无效配置被静默吞掉。
    TArray<const FCameraChannelConfig*> EffectiveConfigs;
    GatherEffectiveChannelConfigs(Channels, EffectiveConfigs, true);

    // 配置没有变化，直接返回
    if (!GetOwner() || IsConfigurationCurrent())
    {
        ++ResourceStats.NoOpConfigurationApplyCount;
        return false;
    }

    ++ResourceStats.ConfigurationChangeCount;
    // 保存旧 RuntimeChannels
    TArray<FChannelRuntime> PreviousChannels = MoveTemp(RuntimeChannels);
    RuntimeChannels.Reset();

    // 只遍历通过产品约束验证的正式通道。
    for (const FCameraChannelConfig* ConfigPtr : EffectiveConfigs)
    {
        const FCameraChannelConfig& Config = *ConfigPtr;

        // ChannelGuid 是资源身份；ChannelType 只描述模态，不能再用于判断
        const int32 ExistingIndex = PreviousChannels.IndexOfByPredicate(
            [&Config](const FChannelRuntime& Existing)
            {
                return Existing.Config.ChannelGuid == Config.ChannelGuid;
            });

        // 找不到：创建 Capture + RT
        if (ExistingIndex == INDEX_NONE)
        {
            FChannelRuntime& NewRuntime = RuntimeChannels.AddDefaulted_GetRef();
            CreateChannel(NewRuntime, Config);
            continue;
        }

        // 找到：复用 Capture
        FChannelRuntime Runtime = MoveTemp(PreviousChannels[ExistingIndex]);
        PreviousChannels.RemoveAt(ExistingIndex);
        ++ResourceStats.ReusedCaptureComponents;

        // RT 属性是否变化
        const bool bTargetResourceChanged =
            Runtime.Config.ChannelType != Config.ChannelType ||
            Runtime.Config.Resolution != Config.Resolution ||
            GetEffectiveLinearGamma(Runtime.Config) != GetEffectiveLinearGamma(Config) ||
            GetChannelRenderTargetFormat(Runtime.Config) != GetChannelRenderTargetFormat(Config);

        if (bTargetResourceChanged)
        {
            // 退休旧 RT，创建新 RT
            // 不原地改写旧 Target：在途 Capture/Readback 可能仍引用旧资源，换新对象才能保持两代资源互不干扰。
            RetireChannelTargets(Runtime);
            if (Config.ChannelType == ECameraChannelType::Instance)
            {
                Runtime.InstanceTarget = CreateInstanceOutputTarget(Config.Resolution);
                Runtime.CaptureTarget = CreateInstanceCaptureTarget(this, Config.Resolution);
                ResourceStats.RebuiltRenderTargets += 2;
            }
            else
            {
                Runtime.Target = NewObject<UTextureRenderTarget2D>(this);
                InitializeOutputTarget(*Runtime.Target, Config);
                ++ResourceStats.RebuiltRenderTargets;
            }

            if (Config.ChannelType == ECameraChannelType::Semantic)
            {
                UE::SensorSimulation::SemanticCapture::RegisterTarget(
                    Runtime.Target->GameThread_GetRenderTargetResource());
            }
            else if (Config.ChannelType == ECameraChannelType::Instance)
            {
                UE::SensorSimulation::InstanceCapture::RegisterCaptureTarget(
                    Runtime.CaptureTarget->GameThread_GetRenderTargetResource(),
                    Runtime.InstanceTarget.Get());
            }
        }
        else
        {
            // 复用 RT
            // FOV、SensorName 或数组顺序变化不要求重新分配 GPU 纹理。
            ++ResourceStats.ReusedRenderTargets;
        }

        Runtime.Config = Config;
        ConfigureCapture(Runtime);
        RuntimeChannels.Add(MoveTemp(Runtime));
    }

    // 没有被新配置匹配的通道已经被禁用或删除，只销毁这些受影响对象。
    for (FChannelRuntime& RemovedRuntime : PreviousChannels)
    {
        DestroyChannel(RemovedRuntime);
    }

    AppliedSensorName = SensorName;
    AppliedHorizontalFovDegrees = HorizontalFovDegrees;
    AppliedMaxPendingReadbacks = FMath::Max(1, MaxPendingReadbacks);
    bHasAppliedConfiguration = true;

    // 通知依赖 Camera Rig 配置的上层对象
    ConfigurationChangedDelegate.Broadcast();
    return true;
}

/** 销毁所有动态捕获组件并清空运行时通道。 */
void UCameraRigComponent::DestroyChannels()
{
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        DestroyChannel(Runtime);
    }
    RuntimeChannels.Reset();
    bHasAppliedConfiguration = false;
}

/**
 * 让 OpaqueProxy 只进入标签通道。
 *
 * bVisibleInSceneCaptureOnly 已使代理不进入主视口；这里再把代理加入 RGB/Depth
 * SceneCapture 的 HiddenComponents。Semantic/Instance 则移除本 Rig 上一次管理的条目。
 */
void UCameraRigComponent::RefreshOpaqueLabelProxyVisibility(FChannelRuntime& Channel)
{
    if (!Channel.Capture)
    {
        return;
    }

    for (const TWeakObjectPtr<UPrimitiveComponent>& PreviousProxy : Channel.HiddenLabelProxies)
    {
        Channel.Capture->HiddenComponents.Remove(PreviousProxy);
    }
    Channel.HiddenLabelProxies.Reset();

    const bool bVisualChannel =
        Channel.Config.ChannelType == ECameraChannelType::Rgb ||
        Channel.Config.ChannelType == ECameraChannelType::Depth;
    const bool bInstanceChannel = Channel.Config.ChannelType == ECameraChannelType::Instance;
    if (!bVisualChannel && !bInstanceChannel)
    {
        return;
    }

    const TArray<TWeakObjectPtr<UPrimitiveComponent>> Proxies =
        UE::SensorSimulation::InstanceCapture::GetOpaqueLabelProxies(GetWorld());
    for (const TWeakObjectPtr<UPrimitiveComponent>& Proxy : Proxies)
    {
        if (!Proxy.IsValid())
        {
            continue;
        }
        // RGB/Depth 永远隔离标签代理；Instance 只显示当前确实绑定 ID 的激活代理。
        // Ignore 策略下代理仍保持 capture-only 资产状态，但不能遮挡其后的真实标签对象。
        const bool bHide = bVisualChannel ||
            !UE::SensorSimulation::InstanceCapture::IsPrimitiveRegistered(Proxy.Get());
        if (bHide)
        {
            Channel.HiddenLabelProxies.Add(Proxy);
            Channel.Capture->HideComponent(Proxy.Get());
        }
    }
}
/** 根据 RGB、标签或深度模态配置场景捕获参数。 */
void UCameraRigComponent::ConfigureCapture(FChannelRuntime& Channel)
{
    // Instance 使用普通颜色工作目标建立 View；其他通道直接捕获到正式输出。
    Channel.Capture->TextureTarget =
        Channel.CaptureTarget ? Channel.CaptureTarget : Channel.Target;
    Channel.Capture->FOVAngle = HorizontalFovDegrees;
    Channel.Capture->bCaptureEveryFrame = false;
    Channel.Capture->bCaptureOnMovement = false;

    if (Channel.Config.ChannelType == ECameraChannelType::Semantic)
    {
        // 保留易于 Unreal Insights 识别的事件名称；通道身份由 RenderTarget 注册表负责。
        Channel.Capture->ProfilingEventName =
            UE::SensorSimulation::SemanticCapture::ProfilingEventName;
        // FinalToneCurveHDR 只负责保留所需的后处理时序，不再承担“语义相机身份”哨兵。
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;

        // 标签是离散整数：关闭所有可能改变数值或混合相邻像素的渲染特性。
        Channel.Capture->ShowFlags.SetAntiAliasing(false);
        Channel.Capture->ShowFlags.SetBloom(false);
        Channel.Capture->ShowFlags.SetDepthOfField(false);
        Channel.Capture->ShowFlags.SetEyeAdaptation(false);
        Channel.Capture->ShowFlags.SetMotionBlur(false);
        Channel.Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
        Channel.Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        Channel.Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
        Channel.Capture->PostProcessSettings.AutoExposureBias = 0.0f;
    }
    else if (Channel.Config.ChannelType == ECameraChannelType::Instance)
    {
        // 颜色结果只是建立 View/SceneDepth 的工作纹理；独立 Mesh Pass 才生成正式 uint32 输出。
        Channel.Capture->ProfilingEventName =
            UE::SensorSimulation::InstanceCapture::ProfilingEventName;
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        Channel.Capture->ShowFlags.SetAntiAliasing(false);
        Channel.Capture->ShowFlags.SetMotionBlur(false);
    }
    else if (Channel.Config.ChannelType == ECameraChannelType::Depth)
    {
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    }
    else
    {
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }
}

/** 按下快门 触发所有通道延迟采集并广播本次请求。 */
ECaptureRequestResult UCameraRigComponent::SubmitCapture(const FCaptureRequest& Request)
{
    // 蓝图或 C++ 可能在两帧之间直接修改 UPROPERTY；提交前做轻量 Diff，保证本帧使用最新配置。
    ApplyConfiguration();

    if (!ImageReadbackManager)
    {
        ImageReadbackManager = MakeUnique<FImageReadbackManager>(MaxPendingReadbacks);
    }

    // 先进行整次请求准入，避免部分通道已提交、后续通道才因容量不足而失败。
    int32 RequestedChannelCount = 0;
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        const EPayloadType PayloadType = Runtime.Config.ToPayloadType();
        const bool bRequested = Request.ExpectedImageChannels.ContainsByPredicate(
            [&Runtime, PayloadType](const FExpectedImageChannel& Expected)
            {
                return Expected.ChannelGuid == Runtime.Config.ChannelGuid && Expected.PayloadType == PayloadType;
            });
        if (PayloadType == EPayloadType::None || !bRequested)
        {
            continue;
        }
        const bool bHasOutput = PayloadType == EPayloadType::Instance
            ? Runtime.InstanceTarget.IsValid()
            : IsValid(Runtime.Target);
        if (!IsValid(Runtime.Capture) || !bHasOutput)
        {
            return ECaptureRequestResult::Rejected;
        }
        ++RequestedChannelCount;
    }
    if (RequestedChannelCount == 0)
    {
        return ECaptureRequestResult::Rejected;
    }
    if (ImageReadbackManager->GetPendingCount() + RequestedChannelCount >
        ImageReadbackManager->GetCapacity())
    {
        return ECaptureRequestResult::Busy;
    }

    // 确定通道对应的 Payload 类型
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        EPayloadType PayloadType = EPayloadType::None;
        if (Runtime.Config.ChannelType == ECameraChannelType::Rgb)
        {
            PayloadType = EPayloadType::Rgb;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Semantic)
        {
            PayloadType = EPayloadType::Semantic;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Depth)
        {
            PayloadType = EPayloadType::Depth;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Instance)
        {
            PayloadType = EPayloadType::Instance;
        }

        const bool bRequested = Request.ExpectedImageChannels.ContainsByPredicate(
            [&Runtime, PayloadType](const FExpectedImageChannel& Expected)
            {
                return Expected.ChannelGuid == Runtime.Config.ChannelGuid && Expected.PayloadType == PayloadType;
            });
        if (PayloadType == EPayloadType::None || !bRequested)
        {
            continue;
        }

        // 标签代理可能在 Camera Rig 创建后才注册；每次提交前刷新，保证热更新立即生效。
        RefreshOpaqueLabelProxyVisibility(Runtime);
        // Instance 是离散标签快照，不应复用上一帧的遮挡/时序历史；CameraCut 强制当前 View
        // 使用本帧 Scene/GPU Scene 状态，避免运动对象在 D3D12 中沿用旧可见性结果。
        if (PayloadType == EPayloadType::Instance)
        {
            Runtime.Capture->bCameraCutThisFrame = true;
        }
        // 拍摄！
        // CaptureScene 提交本帧捕获命令，等待渲染线程执行
        Runtime.Capture->CaptureScene();
        // Instance 直接读原生整数资源；其余通道继续读 UObject RenderTarget。
        const bool bEnqueued = PayloadType == EPayloadType::Instance
            ? ImageReadbackManager->Enqueue(Runtime.InstanceTarget, Request, Runtime.Config.ChannelGuid, PayloadType)
            : ImageReadbackManager->Enqueue(Runtime.Target, Request, Runtime.Config.ChannelGuid, PayloadType);
        if (!bEnqueued)
        {
            UE_LOG(LogCameraRigDebug, Warning,
                TEXT("Image readback was rejected for sensor '%s', payload type %u."),
                *Request.SensorName.ToString(), static_cast<uint8>(PayloadType));
            return ECaptureRequestResult::Rejected;
        }
    }
    CaptureSubmittedDelegate.Broadcast(Request);
    return ECaptureRequestResult::Accepted;
}

/** 取出一个完成的 CPU 图像 FImagePayload。 
*   USimCameraSensorComponent::TickComponent()
    会不断调用 PollCompletedImage()
 */
bool UCameraRigComponent::PollCompletedImage(FImagePayload& OutPayload)
{
    if (!ImageReadbackManager)
    {
        return false;
    }

    const bool bDelivered = ImageReadbackManager->PollCompleted(OutPayload);
    // 成功交付或失败都可能让 Pending 归零；每次 Poll 后检查，确保失败路径也不会永久保留旧 Target。
    ReleaseRetiredTargetsIfSafe();
    return bDelivered;
}

/** 汇总当前启用且由正式异步读回链路支持的相机模态。 */
EPayloadType UCameraRigComponent::GetEnabledPayloadTypes() const
{
    EPayloadType Result = EPayloadType::None;
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Config.ChannelType == ECameraChannelType::Rgb)
        {
            Result |= EPayloadType::Rgb;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Semantic)
        {
            Result |= EPayloadType::Semantic;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Depth)
        {
            Result |= EPayloadType::Depth;
        }
        else if (Runtime.Config.ChannelType == ECameraChannelType::Instance)
        {
            Result |= EPayloadType::Instance;
        }
    }
    return Result;
}

/** 返回当前实际创建的独立图像通道描述。 */
TArray<FExpectedImageChannel> UCameraRigComponent::GetEnabledImageChannels() const
{
    TArray<FExpectedImageChannel> Result;
    Result.Reserve(RuntimeChannels.Num());
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        const EPayloadType PayloadType = Runtime.Config.ToPayloadType();
        if (Runtime.Config.ChannelGuid.IsValid() && PayloadType != EPayloadType::None)
        {
            FExpectedImageChannel& Channel = Result.AddDefaulted_GetRef();
            Channel.ChannelGuid = Runtime.Config.ChannelGuid;
            Channel.PayloadType = PayloadType;
        }
    }
    return Result;
}

FImageReadbackStats UCameraRigComponent::GetImageReadbackStats() const
{
    return ImageReadbackManager ? ImageReadbackManager->GetStats() : FImageReadbackStats{};
}

TArray<FImageReadbackChannelStats> UCameraRigComponent::GetImageReadbackChannelStats() const
{
    return ImageReadbackManager
        ? ImageReadbackManager->GetChannelStats()
        : TArray<FImageReadbackChannelStats>{};
}

/** 立即执行全部有效通道的单帧调试采集。 */
void UCameraRigComponent::CaptureDebugFrame()
{
    // 调试捕获与正式路径必须共享同一热更新入口，避免编辑器预览和数据集采集使用不同配置。
    ApplyConfiguration();

    int32 CapturedChannelCount = 0;
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Capture && (Runtime.Target || Runtime.InstanceTarget.IsValid()))
        {
            // CaptureScene 会提交一次即时场景捕获；后续 PNG 导出进行显式同步读回。
            Runtime.Capture->CaptureScene();
            ++CapturedChannelCount;
        }
    }

    UE_LOG(LogCameraRigDebug, Display, TEXT("Debug capture submitted for %d channel(s) on '%s'."),
        CapturedChannelCount,
        *GetNameSafe(GetOwner()));
}

/** 按稳定 ChannelGuid 返回当前使用的瞬态 Render Target。 */
UTextureRenderTarget2D* UCameraRigComponent::GetChannelRenderTarget(const FGuid ChannelGuid) const
{
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Config.ChannelGuid == ChannelGuid)
        {
            return Runtime.Target;
        }
    }
    return nullptr;
}

/** 按稳定 ChannelGuid 返回正式输出像素格式；Instance 不依赖 UTextureRenderTarget2D。 */
EPixelFormat UCameraRigComponent::GetChannelPixelFormat(const FGuid& ChannelGuid) const
{
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Config.ChannelGuid == ChannelGuid)
        {
            return Runtime.InstanceTarget.IsValid()
                ? PF_R32_UINT
                : Runtime.Target ? Runtime.Target->GetFormat() : PF_Unknown;
        }
    }
    return PF_Unknown;
}

/** 人工调试按钮仍按模态选择第一条通道，但返回稳定身份后再查询资源。 */
FGuid UCameraRigComponent::FindFirstChannelGuid(const ECameraChannelType ChannelType) const
{
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Config.ChannelType == ChannelType)
        {
            return Runtime.Config.ChannelGuid;
        }
    }
    return FGuid();
}

/** 捕获并把 Semantic Render Target 显式同步导出为 PNG；仅用于人工调试。 */
void UCameraRigComponent::SaveSemanticDebugImage()
{
    // 先提交一次预热捕获，使本关卡首次可见的材质产生 Shader 编译任务；等待完成后再拍摄验收帧。
    CaptureDebugFrame();
    FlushRenderingCommands();
    WaitForDebugCaptureShaders();
    CaptureDebugFrame();

    // 调试保存需要当前帧的确定结果；正式 SubmitCapture/Readback 链路仍保持异步。
    FlushRenderingCommands();

    UTextureRenderTarget2D* SemanticTarget = GetChannelRenderTarget(FindFirstChannelGuid(ECameraChannelType::Semantic));
    if (!SemanticTarget)
    {
        UE_LOG(LogCameraRigDebug, Error,
            TEXT("Cannot save Semantic debug image: the Semantic channel is missing or disabled on '%s'."),
            *GetNameSafe(GetOwner()));
        return;
    }

    FBufferArchive PngBytes;
    if (!FImageUtils::ExportRenderTarget2DAsPNG(SemanticTarget, PngBytes))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to encode the Semantic Render Target as PNG."));
        return;
    }

    const FString OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("SensorSimulation"),
        TEXT("Debug"));
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);

    const FString Filename = FString::Printf(
        TEXT("Semantic_%s_%s.png"),
        *SensorName.ToString(),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    const FString OutputPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, Filename));
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to write Semantic debug image: %s"), *OutputPath);
        return;
    }

    LastSemanticDebugImagePath = OutputPath;
    UE_LOG(LogCameraRigDebug, Display, TEXT("Semantic debug image saved: %s"), *OutputPath);
}

/** 捕获并把 RGB Render Target 同步导出为 PNG；仅用于颜色基准验收。 */
void UCameraRigComponent::SaveRgbDebugImage()
{
    // 预热捕获会触发尚未就绪的材质 Shader 编译，第二次捕获才作为确定性的 RGB 验收结果。
    CaptureDebugFrame();
    FlushRenderingCommands();
    WaitForDebugCaptureShaders();
    CaptureDebugFrame();

    // 防止同步导出读取到第二次 CaptureScene 提交前的预热 RenderTarget。
    FlushRenderingCommands();
    UTextureRenderTarget2D* RgbTarget = GetChannelRenderTarget(FindFirstChannelGuid(ECameraChannelType::Rgb));
    if (!RgbTarget)
    {
        UE_LOG(LogCameraRigDebug, Error,
            TEXT("Cannot save RGB debug image: the RGB channel is missing or disabled on '%s'."),
            *GetNameSafe(GetOwner()));
        return;
    }

    FBufferArchive PngBytes;
    if (!FImageUtils::ExportRenderTarget2DAsPNG(RgbTarget, PngBytes))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to encode the RGB Render Target as PNG."));
        return;
    }

    const FString OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("SensorSimulation"),
        TEXT("Debug"));
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString Filename = FString::Printf(
        TEXT("RGB_%s_%s.png"),
        *SensorName.ToString(),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    const FString OutputPath =
        FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, Filename));
    if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to write RGB debug image: %s"), *OutputPath);
        return;
    }

    LastRgbDebugImagePath = OutputPath;
    UE_LOG(LogCameraRigDebug, Display, TEXT("RGB debug image saved: %s"), *OutputPath);
}

/** 捕获并把 RGBA32F Depth Render Target 同步导出为 EXR；正式 Payload 只保留 R 并转换为 R32F 米。 */
void UCameraRigComponent::SaveDepthDebugImage()
{
    // SceneDepth 同样先预热一次，避免首次场景代理/材质尚未就绪时把初始化纹理误判为有效深度。
    CaptureDebugFrame();
    FlushRenderingCommands();
    WaitForDebugCaptureShaders();
    CaptureDebugFrame();

    // RGBA32F EXR 必须等待 SceneDepth 捕获完成，否则初始化纹理会表现为全 0。
    FlushRenderingCommands();
    UTextureRenderTarget2D* DepthTarget = GetChannelRenderTarget(FindFirstChannelGuid(ECameraChannelType::Depth));
    if (!DepthTarget)
    {
        UE_LOG(LogCameraRigDebug, Error,
            TEXT("Cannot save Depth debug image: the Depth channel is missing or disabled on '%s'."),
            *GetNameSafe(GetOwner()));
        return;
    }

    FBufferArchive ExrBytes;
    if (!FImageUtils::ExportRenderTarget2DAsEXR(DepthTarget, ExrBytes))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to encode the Depth Render Target as EXR."));
        return;
    }

    const FString OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("SensorSimulation"),
        TEXT("Debug"));
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString Filename = FString::Printf(
        TEXT("Depth_%s_%s.exr"),
        *SensorName.ToString(),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    const FString OutputPath =
        FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, Filename));
    if (!FFileHelper::SaveArrayToFile(ExrBytes, *OutputPath))
    {
        UE_LOG(LogCameraRigDebug, Error, TEXT("Failed to write Depth debug image: %s"), *OutputPath);
        return;
    }

    LastDepthDebugImagePath = OutputPath;
    UE_LOG(LogCameraRigDebug, Display, TEXT("Depth debug image saved: %s"), *OutputPath);
}
/** 由分辨率和水平视场角计算针孔相机内参。 */
FCalibration UCameraRigComponent::BuildCalibration(const FCameraChannelConfig& Channel) const
{
    FCalibration Calibration;
    Calibration.SensorName = SensorName;
    Calibration.ChannelGuid = Channel.ChannelGuid;
    Calibration.PayloadType = Channel.ToPayloadType();
    Calibration.SensorToEgo = GetRelativeTransform();
    Calibration.ImageSize = Channel.Resolution;
    // 采用像素中心坐标约定，主点位于 [0, width-1] 与 [0, height-1] 的几何中心。
    Calibration.Cx = (static_cast<double>(Channel.Resolution.X) - 1.0) * 0.5;
    Calibration.Cy = (static_cast<double>(Channel.Resolution.Y) - 1.0) * 0.5;
    // 针孔模型关系 width = 2 * fx * tan(horizontal_fov / 2)，据此反求像素焦距。
    Calibration.Fx = static_cast<double>(Channel.Resolution.X) /
        (2.0 * FMath::Tan(FMath::DegreesToRadians(static_cast<double>(HorizontalFovDegrees)) * 0.5));
    Calibration.Fy = Calibration.Fx;
    return Calibration;
}

/** 返回当前实际创建通道的独立标定，确保不同分辨率输出不会共享错误内参。 */
TArray<FCalibration> UCameraRigComponent::BuildActiveCalibrations() const
{
    TArray<FCalibration> Result;
    Result.Reserve(RuntimeChannels.Num());
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        Result.Add(BuildCalibration(Runtime.Config));
    }
    return Result;
}
