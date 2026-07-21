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
    }
    Super::BeginPlay();
}

/** 清空完成队列并将拥有独立 CPU 内存的图像载荷移交给 Subsystem。 */
void USimCameraSensorComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!CameraRig || !GetWorld())
    {
        return;
    }

    USimulationSubsystem* Subsystem = GetWorld()->GetSubsystem<USimulationSubsystem>();
    if (!Subsystem)
    {
        return;
    }

    FImagePayload Image;
    while (CameraRig->PollCompletedImage(Image))
    {
        Subsystem->SubmitImage(MoveTemp(Image));
        Image = FImagePayload();
    }
}

/** 返回适配器当前能够为同步帧生产的图像模态。 */
EPayloadType USimCameraSensorComponent::GetPayloadTypes() const
{
    return CameraRig ? CameraRig->GetEnabledPayloadTypes() : EPayloadType::None;
}

/** 转发本帧请求，并补充 Camera Rig 相对于自车的外参。 */
void USimCameraSensorComponent::RequestCapture(const FCaptureRequest& Request)
{
    if (!bSensorEnabled || !CameraRig)
    {
        return;
    }

    FCaptureRequest CameraRequest = Request;
    CameraRequest.SensorName = SensorName;
    CameraRequest.SensorToEgo = CameraRig->GetRelativeTransform();
    CameraRequest.ExpectedPayloads &= GetPayloadTypes();
    CameraRig->SubmitCapture(CameraRequest);
}
