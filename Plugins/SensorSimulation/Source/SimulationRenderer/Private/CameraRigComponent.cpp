#include "CameraRigComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"

/** 构造并初始化 UCameraRigComponent 的默认状态。 */
UCameraRigComponent::UCameraRigComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

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
    BuildChannels();
}

/** 组件注销前销毁动态创建的捕获通道。 */
void UCameraRigComponent::OnUnregister()
{
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
        Runtime.Capture = NewObject<USceneCaptureComponent2D>(GetOwner());
        Runtime.Capture->SetupAttachment(this);
        Runtime.Capture->RegisterComponent();

        Runtime.Target = NewObject<UTextureRenderTarget2D>(this);
        Runtime.Target->bForceLinearGamma = Config.bForceLinearGamma;
        // 深度通道需要单通道浮点精度；颜色与离散标签通道使用紧凑的 8 位 RGBA。
        Runtime.Target->RenderTargetFormat = Config.ChannelType == ECameraChannelType::Depth
            ? ETextureRenderTargetFormat::RTF_R32f
            : ETextureRenderTargetFormat::RTF_RGBA8;
        Runtime.Target->InitAutoFormat(Config.Resolution.X, Config.Resolution.Y);
        Runtime.Target->UpdateResourceImmediate(true);

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

    if (Channel.Config.ChannelType == ECameraChannelType::Semantic ||
        Channel.Config.ChannelType == ECameraChannelType::Instance)
    {
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
        Channel.Capture->PostProcessSettings.bOverride_MotionBlurAmount = true;
        Channel.Capture->PostProcessSettings.MotionBlurAmount = 0.0f;
        Channel.Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
        Channel.Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    }
/** 执行 if 对应的接口行为。 */
    else if (Channel.Config.ChannelType == ECameraChannelType::Depth)
    {
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
    }
    else
    {
        Channel.Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    }
}

/** 触发所有通道延迟采集并广播本次请求。 */
void UCameraRigComponent::SubmitCapture(const FCaptureRequest& Request)
{
    for (FChannelRuntime& Runtime : RuntimeChannels)
    {
        Runtime.Capture->CaptureSceneDeferred();
    }
    CaptureSubmittedDelegate.Broadcast(Request);
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
