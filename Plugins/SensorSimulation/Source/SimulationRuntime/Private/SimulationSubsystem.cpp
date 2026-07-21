#include "SimulationSubsystem.h"
#include "SemanticObjectComponent.h"
#include "SimSensorComponentBase.h"
#include "SimulationSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

/** 初始化世界级传感器仿真子系统。 */
void USimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

/** 清空传感器和语义状态后反初始化子系统。 */
void USimulationSubsystem::Deinitialize()
{
    Sensors.Reset();
    SemanticRegistry.Reset();
    Super::Deinitialize();
}

/** 推进仿真时钟、按固定步长发起采集并消费完整帧。 */
void USimulationSubsystem::Tick(float DeltaTime)
{
    const USimulationSettings* Settings = GetDefault<USimulationSettings>();
    const double Step = FMath::Max(0.001, Settings->FixedStepSeconds);
    AccumulatedSeconds += DeltaTime;
    SimulationSeconds += DeltaTime;

    // 累加器把不稳定的游戏帧 DeltaTime 转换为固定频率采样；Fmod 保留不足一步的余量。
    if (AccumulatedSeconds >= Step)
    {
        AccumulatedSeconds = FMath::Fmod(AccumulatedSeconds, Step);
        RequestFrame(SimulationSeconds);
    }

    FFramePacket CompletePacket;
    while (FrameAssembler.PopCompleteFrame(CompletePacket))
    {
        // Export service integration point. A packet is published only after
        // every expected modality has arrived.
    }
}

/** 返回 Unreal Tick 性能统计标识。 */
TStatId USimulationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USimulationSubsystem, STATGROUP_Tickables);
}

/** 把传感器加入当前世界的弱引用集合并避免重复。 */
void USimulationSubsystem::RegisterSensor(USimSensorComponentBase& Sensor)
{
    Sensors.AddUnique(&Sensor);
}

/** 从当前世界移除指定传感器的弱引用。 */
void USimulationSubsystem::UnregisterSensor(const USimSensorComponentBase& Sensor)
{
    Sensors.RemoveAll([&Sensor](const TWeakObjectPtr<USimSensorComponentBase>& Item) { return Item.Get() == &Sensor; });
}

/** 将语义组件注册到实例编号注册表。 */
uint32 USimulationSubsystem::RegisterSemanticObject(USemanticObjectComponent& Component)
{
    return SemanticRegistry.Register(Component);
}

/** 从实例编号注册表注销语义组件。 */
void USimulationSubsystem::UnregisterSemanticObject(const USemanticObjectComponent& Component)
{
    SemanticRegistry.Unregister(Component);
}

/** 把完成的图像移交给帧聚合器。 */
void USimulationSubsystem::SubmitImage(FImagePayload&& Image)
{
    FrameAssembler.AddImage(MoveTemp(Image));
}

/** 把完成的点云移交给帧聚合器。 */
void USimulationSubsystem::SubmitLidar(FLidarScanPayload&& Scan)
{
    FrameAssembler.AddLidar(MoveTemp(Scan));
}

/** 创建同步帧、采集真值并向所有启用的传感器下发请求。 */
void USimulationSubsystem::RequestFrame(double TimestampSeconds)
{
    FFrameHeader Header;
    Header.SequenceId = SequenceId;
    Header.FrameId = NextFrameId++;
    Header.SimulationTimestampSeconds = TimestampSeconds;

    // 先声明整帧预期模态，聚合器据此判断异步返回的数据何时全部到齐。
    EPayloadType Expected = EPayloadType::GroundTruth;
    for (const TWeakObjectPtr<USimSensorComponentBase>& Sensor : Sensors)
    {
        if (Sensor.IsValid() && Sensor->bSensorEnabled)
        {
            Expected |= EPayloadType::Lidar;
        }
    }

    FrameAssembler.BeginFrame(Header, Expected);
    FrameAssembler.AddGroundTruth(Header.FrameId, CaptureGroundTruth());

    for (const TWeakObjectPtr<USimSensorComponentBase>& Sensor : Sensors)
    {
        if (!Sensor.IsValid() || !Sensor->bSensorEnabled)
        {
            continue;
        }

        FCaptureRequest Request;
        Request.Header = Header;
        Request.SensorName = Sensor->SensorName;
        Request.ExpectedPayloads = EPayloadType::Lidar;
        Sensor->RequestCapture(Request);
    }
}

/** 遍历带语义组件的 Actor 并采集位姿、包围盒和速度真值。 */
TArray<FObjectGroundTruth> USimulationSubsystem::CaptureGroundTruth() const
{
    TArray<FObjectGroundTruth> Output;
    if (!GetWorld())
    {
        return Output;
    }

    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        const AActor* Actor = *It;
        const USemanticObjectComponent* Semantic = Actor->FindComponentByClass<USemanticObjectComponent>();
        if (!Semantic)
        {
            continue;
        }

        FObjectGroundTruth& Item = Output.AddDefaulted_GetRef();
        Item.InstanceId = static_cast<uint32>(FMath::Max(0, Semantic->InstanceId));
        Item.SemanticId = static_cast<uint16>(FMath::Clamp(Semantic->SemanticId, 0, 65535));
        Item.WorldTransform = Actor->GetActorTransform();
        Item.WorldBounds = FBox3d(Actor->GetComponentsBoundingBox(true));
        Item.LinearVelocity = FVector3d(Actor->GetVelocity());
    }
    return Output;
}

