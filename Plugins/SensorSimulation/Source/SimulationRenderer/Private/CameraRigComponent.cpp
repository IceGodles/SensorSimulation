#include "CameraRigComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Serialization/BufferArchive.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCameraRigDebug, Log, All);

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
    DestroyChannels();
    Super::OnUnregister();
}

/** 为每个有效配置创建场景捕获组件和匹配的渲染目标。 */
void UCameraRigComponent::BuildChannels()
{
    DestroyChannels();
    if (!GetOwner())
    {
        return;
    }

    for (const FCameraChannelConfig& Config : Channels)
    {
        if (!Config.bEnabled || Config.Resolution.X <= 0 || Config.Resolution.Y <= 0)
        {
            continue;
        }

        FChannelRuntime& Runtime = RuntimeChannels.AddDefaulted_GetRef();
        Runtime.Config = Config;
        // 创建 Capture 组件，绑定到 Rig 
        Runtime.Capture = NewObject<USceneCaptureComponent2D>(GetOwner());
        Runtime.Capture->SetupAttachment(this);
        Runtime.Capture->RegisterComponent();

        // 创建 render target
        Runtime.Target = NewObject<UTextureRenderTarget2D>(this);
        Runtime.Target->bForceLinearGamma = Config.ChannelType == ECameraChannelType::Semantic
            ? true
            : Config.bForceLinearGamma;
        // 深度通道需要单通道浮点精度；颜色与离散标签通道使用紧凑的 8 位 RGBA。
        Runtime.Target->RenderTargetFormat = Config.ChannelType == ECameraChannelType::Depth
            ? ETextureRenderTargetFormat::RTF_R32f
            : ETextureRenderTargetFormat::RTF_RGBA8;
        Runtime.Target->InitAutoFormat(Config.Resolution.X, Config.Resolution.Y);
        Runtime.Target->UpdateResourceImmediate(true);

        // 配置各通道渲染行为和参数
        ConfigureCapture(Runtime);
    }
}

/** 销毁所有动态捕获组件并清空运行时通道。 */
void UCameraRigComponent::DestroyChannels()
{
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Capture)
        {
            Runtime.Capture->DestroyComponent();
        }
    }
    RuntimeChannels.Reset();
}

/** 根据 RGB、标签或深度模态配置场景捕获参数。 */
void UCameraRigComponent::ConfigureCapture(FChannelRuntime& Channel)
{
    Channel.Capture->TextureTarget = Channel.Target;
    Channel.Capture->FOVAngle = HorizontalFovDegrees;
    Channel.Capture->bCaptureEveryFrame = false;
    Channel.Capture->bCaptureOnMovement = false;

    if (Channel.Config.ChannelType == ECameraChannelType::Semantic)
    {
        // FinalToneCurveHDR 作为 Semantic 专用捕获源标记，View Extension 据此只接管该通道。
        Channel.Capture->ProfilingEventName = TEXT("SensorSimulation.Semantic");
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
        // Instance 通道暂保留原框架行为；后续应使用独立 32 位实例标签 Pass。
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
void UCameraRigComponent::SubmitCapture(const FCaptureRequest& Request)
{
    if (!ImageReadbackManager)
    {
        ImageReadbackManager = MakeUnique<FImageReadbackManager>(MaxPendingReadbacks);
    }

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

        if (PayloadType == EPayloadType::None || !EnumHasAnyFlags(Request.ExpectedPayloads, PayloadType))
        {
            continue;
        }

        // 拍摄！
        // CaptureScene 先把本帧捕获命令排入渲染线程
        Runtime.Capture->CaptureScene();
        // 把结果从 GPU显存的 Render Target 异步回读到 CPU FImagePayload::Bytes
        if (!ImageReadbackManager->Enqueue(Runtime.Target, Request, PayloadType))
        {
            UE_LOG(LogCameraRigDebug, Warning,
                TEXT("Image readback was rejected for sensor '%s', payload type %u."),
                *Request.SensorName.ToString(), static_cast<uint8>(PayloadType));
        }
    }
    CaptureSubmittedDelegate.Broadcast(Request);
}

/** 取出一个完成的 CPU 图像 FImagePayload。 
*   USimCameraSensorComponent::TickComponent()
    会不断调用 PollCompletedImage()
 */
bool UCameraRigComponent::PollCompletedImage(FImagePayload& OutPayload)
{
    return ImageReadbackManager && ImageReadbackManager->PollCompleted(OutPayload);
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
    }
    return Result;
}

/** 立即执行全部有效通道的单帧调试采集。 */
void UCameraRigComponent::CaptureDebugFrame()
{
    if (RuntimeChannels.IsEmpty())
    {
        BuildChannels();
    }

    int32 CapturedChannelCount = 0;
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Capture && Runtime.Target)
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

/** 返回指定通道当前使用的瞬态 Render Target。 */
UTextureRenderTarget2D* UCameraRigComponent::GetChannelRenderTarget(ECameraChannelType ChannelType) const
{
    for (const FChannelRuntime& Runtime : RuntimeChannels)
    {
        if (Runtime.Config.ChannelType == ChannelType)
        {
            return Runtime.Target;
        }
    }
    return nullptr;
}

/** 捕获并把 Semantic Render Target 显式同步导出为 PNG；仅用于人工调试。 */
void UCameraRigComponent::SaveSemanticDebugImage()
{
    CaptureDebugFrame();

    UTextureRenderTarget2D* SemanticTarget = GetChannelRenderTarget(ECameraChannelType::Semantic);
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
/** 由分辨率和水平视场角计算针孔相机内参。 */
FCalibration UCameraRigComponent::BuildCalibration(const FCameraChannelConfig& Channel) const
{
    FCalibration Calibration;
    Calibration.SensorName = SensorName;
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
