#include "SimCameraSensorComponent.h"

#include "CameraRigComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "SimulationSubsystem.h"

/** 构造持续轮询异步读回结果的相机传感器适配器。 */
USimCameraSensorComponent::USimCameraSensorComponent()
{
    SensorName = TEXT("FrontCamera");
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
}

/** 解析同一 Actor 上的 Camera Rig，并在注册前同步稳定传感器名称。 */
void USimCameraSensorComponent::BeginPlay()
{
    if (!CameraRig && GetOwner())
    {
        CameraRig = GetOwner()->FindComponentByClass<UCameraRigComponent>();
    }
    if (CameraRig)
    {
        SensorName = CameraRig->SensorName;
        // Calibration 依赖 Resolution/FOV；监听热更新可防止会话文件继续保存启动时的旧内参。
        CameraRig->OnConfigurationChanged().AddUObject(
            this,
            &USimCameraSensorComponent::RegisterCurrentCalibration);
        RegisterCurrentCalibration();
        RegisterCurrentRendererMetrics();
    }
    Super::BeginPlay();
}

/** 解除配置通知，避免组件结束后继续接收热更新回调。 */
void USimCameraSensorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (CameraRig)
    {
        // EndPlay 可能先于 Subsystem::Deinitialize，先提交最终快照才能进入会话 metadata。
        RegisterCurrentRendererMetrics();
        CameraRig->OnConfigurationChanged().RemoveAll(this);
    }
    Super::EndPlay(EndPlayReason);
}

/** 把每条正式图像通道的最新内外参分别更新到 Dataset Session。 */
void USimCameraSensorComponent::RegisterCurrentCalibration()
{
    if (!CameraRig)
    {
        return;
    }

    SensorName = CameraRig->SensorName;
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            // RuntimeChannels 已排除重复、禁用和 Instance 占位，因此这里不会生成无法对应正式图像的标定项。
            for (FCalibration Calibration : CameraRig->BuildActiveCalibrations())
            {
                Calibration.SensorGuid = SensorGuid;
                Calibration.SensorToEgo = ResolveSensorToOwner();
                Subsystem->RegisterCalibration(Calibration);
            }
        }
    }
}

/** 采集 Camera Rig 的可观测性快照，并按 SensorGuid 更新当前 Dataset Session。 */
void USimCameraSensorComponent::RegisterCurrentRendererMetrics()
{
    if (!CameraRig || !GetWorld())
    {
        return;
    }
    if (USimulationSubsystem* Subsystem = GetWorld()->GetSubsystem<USimulationSubsystem>())
    {
        FCameraRendererMetricsSnapshot Metrics;
        Metrics.SensorName = CameraRig->SensorName;
        Metrics.SensorGuid = SensorGuid;
        Metrics.ResourceStats = CameraRig->GetResourceStats();
        Metrics.ReadbackStats = CameraRig->GetImageReadbackStats();
        Metrics.ChannelStats = CameraRig->GetImageReadbackChannelStats();
        Subsystem->RegisterRendererMetrics(Metrics);
    }
}
/** 清空完成队列并将拥有独立 CPU 内存的图像载荷移交给 Subsystem。 */
void USimCameraSensorComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    RendererMetricsElapsedSeconds += DeltaTime;
    if (RendererMetricsElapsedSeconds >= 1.0)
    {
        // 一秒级采样足以诊断长期背压，同时避免逐帧获取 ChannelStats 锁。
        RendererMetricsElapsedSeconds = 0.0;
        RegisterCurrentRendererMetrics();
    }

    if (!CameraRig || !GetWorld())
    {
        return;
    }

    USimulationSubsystem* Subsystem = GetWorld()->GetSubsystem<USimulationSubsystem>();
    if (!Subsystem)
    {
        return;
    }

    DrainCompletedImages();
}

void USimCameraSensorComponent::DrainCompletedImages()
{
    if (!CameraRig || !GetWorld()) return;
    USimulationSubsystem* Subsystem = GetWorld()->GetSubsystem<USimulationSubsystem>();
    if (!Subsystem) return;
    FImagePayload Image;
    while (CameraRig->PollCompletedImage(Image))
    {
        Subsystem->SubmitImage(MoveTemp(Image));
        Image = FImagePayload();
    }
}

void USimCameraSensorComponent::PrepareForShutdown()
{
    Super::PrepareForShutdown();
    DrainCompletedImages();
    RegisterCurrentRendererMetrics();
}

int32 USimCameraSensorComponent::GetInFlightCaptureCount() const
{
    return CameraRig ? CameraRig->GetImageReadbackStats().PendingCount : 0;
}

/** 返回适配器当前能够为同步帧生产的图像模态。 */
EPayloadType USimCameraSensorComponent::GetPayloadTypes() const
{
    return CameraRig ? CameraRig->GetEnabledPayloadTypes() : EPayloadType::None;
}

/** 返回 Camera Rig 当前实际创建的独立图像通道。 */
TArray<FExpectedImageChannel> USimCameraSensorComponent::GetExpectedImageChannels() const
{
    return CameraRig ? CameraRig->GetEnabledImageChannels() : TArray<FExpectedImageChannel>{};
}

/** 转发本帧请求，并补充 Camera Rig 相对于自车的外参。 */
ECaptureRequestResult USimCameraSensorComponent::RequestCapture(const FCaptureRequest& Request)
{
    if (!bSensorEnabled || !CameraRig)
    {
        return ECaptureRequestResult::Rejected;
    }

    // 先应用配置 Diff，再计算本帧能力位；否则刚启用的通道要到下一帧才会进入 ExpectedPayloads。
    CameraRig->ApplyConfiguration();
    SensorName = CameraRig->SensorName;

    FCaptureRequest CameraRequest = Request;
    CameraRequest.SensorName = SensorName;
    CameraRequest.SensorGuid = SensorGuid;
    CameraRequest.SensorToEgo = ResolveSensorToOwner();
    CameraRequest.ExpectedPayloads &= CameraRig->GetEnabledPayloadTypes();
    const TArray<FExpectedImageChannel> ActiveChannels = CameraRig->GetEnabledImageChannels();
    CameraRequest.ExpectedImageChannels.RemoveAll(
        [&ActiveChannels](const FExpectedImageChannel& Expected)
        {
            return !ActiveChannels.Contains(Expected);
        });
    return CameraRig->SubmitCapture(CameraRequest);
}

FTransform USimCameraSensorComponent::ResolveSensorToOwner() const
{
    const AActor* Owner = GetOwner();
    if (CameraRig && Owner && CameraRig->GetOwner() == Owner)
    {
        return CameraRig->GetComponentTransform().GetRelativeTransform(Owner->GetActorTransform());
    }
    return FTransform::Identity;
}
