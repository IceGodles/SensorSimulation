#include "SimLidarSensorComponent.h"
#include "SemanticObjectComponent.h"
#include "SimulationSubsystem.h"
#include "CoordinateConverter.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

/** 构造并初始化 USimLidarSensorComponent 的默认状态。 */
USimLidarSensorComponent::USimLidarSensorComponent()
{
    SensorName = TEXT("TopLidar");
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

/** 在组件开始运行时完成注册或初始化工作。 */
void USimLidarSensorComponent::BeginPlay()
{
    Super::BeginPlay();
    RebuildPattern();
    RegisterCurrentCalibration();
}

/** 根据当前雷达配置重新生成局部扫描射线。 */
void USimLidarSensorComponent::RebuildPattern()
{
    FLidarScanPattern Pattern;
    Pattern.Channels = Config.Channels;
    Pattern.HorizontalSamples = Config.HorizontalSamples;
    Pattern.VerticalFovUpperDegrees = Config.VerticalFovUpperDegrees;
    Pattern.VerticalFovLowerDegrees = Config.VerticalFovLowerDegrees;
    Pattern.BuildDirections(LocalRayDirections);
}

void USimLidarSensorComponent::RegisterCurrentCalibration()
{
    if (!GetWorld())
    {
        return;
    }
    if (USimulationSubsystem* Subsystem = GetWorld()->GetSubsystem<USimulationSubsystem>())
    {
        FLidarCalibration Calibration;
        Calibration.SensorName = SensorName;
        Calibration.SensorGuid = SensorGuid;
        Calibration.SensorToEgo = Config.SensorToOwner;
        Calibration.Channels = Config.Channels;
        Calibration.HorizontalSamples = Config.HorizontalSamples;
        Calibration.VerticalFovUpperDegrees = Config.VerticalFovUpperDegrees;
        Calibration.VerticalFovLowerDegrees = Config.VerticalFovLowerDegrees;
        Calibration.MinRangeMeters = Config.MinRangeMeters;
        Calibration.MaxRangeMeters = Config.MaxRangeMeters;
        Calibration.UpdateFrequencyHz = UpdateFrequencyHz;
        Calibration.RaysPerTick = Config.RaysPerTick;
        Subsystem->RegisterLidarCalibration(Calibration);
    }
}

/** 在传感器空闲且启用时初始化一次新的采集任务。 */
ECaptureRequestResult USimLidarSensorComponent::RequestCapture(const FCaptureRequest& Request)
{
    if (!bSensorEnabled || LocalRayDirections.IsEmpty())
    {
        return ECaptureRequestResult::Rejected;
    }
    if (ActiveRequest.IsSet())
    {
        return ECaptureRequestResult::Busy;
    }

    ActiveRequest = Request;
    ActiveScan = FLidarScanPayload();
    ActiveScan.Header = Request.Header;
    ActiveScan.SensorName = Request.SensorName;
    ActiveScan.SensorGuid = Request.SensorGuid;
    ActiveScan.SensorToEgo = Config.SensorToOwner;
    ActiveScan.ExpectedRayCount = static_cast<uint32>(LocalRayDirections.Num());
    ActiveScan.Points.Reserve(LocalRayDirections.Num());
    NextRayIndex = 0;
    SetComponentTickEnabled(true);
    return ECaptureRequestResult::Accepted;
}

/** 在组件 Tick 中推进当前分批扫描任务。 */
void USimLidarSensorComponent::TickComponent(
    float DeltaTime,
    ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (ActiveRequest.IsSet())
    {
        TraceBatch();
    }
}

/** 执行一批射线检测并生成带语义和相对时间的回波点。 */
void USimLidarSensorComponent::TraceBatch()
{
    UWorld* World = GetWorld();
    const AActor* Owner = GetOwner();
    if (!World || !Owner)
    {
        FinalizeScan();
        return;
    }

    const FTransform SensorTransform = Config.SensorToOwner * Owner->GetActorTransform();
    const FVector Origin = SensorTransform.GetLocation();
    // 将完整扫描分摊到多个 Tick，限制单帧物理查询量，避免大量射线造成明显卡顿。
    const int32 BatchEnd = FMath::Min(NextRayIndex + FMath::Max(1, Config.RaysPerTick), LocalRayDirections.Num());

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SensorSimulationLidar), false, Owner);
    for (; NextRayIndex < BatchEnd; ++NextRayIndex)
    {
        const FVector WorldDirection = SensorTransform.TransformVectorNoScale(FVector(LocalRayDirections[NextRayIndex])).GetSafeNormal();
        // Unreal 距离单位为厘米，而配置使用米；从最小量程处起射可模拟雷达近距离盲区。
        const FVector Start = Origin + WorldDirection * Config.MinRangeMeters * 100.0f;
        const FVector End = Origin + WorldDirection * Config.MaxRangeMeters * 100.0f;

        FHitResult Hit;
        if (World->LineTraceSingleByChannel(Hit, Start, End, Config.TraceChannel, QueryParams))
        {
            FLidarPoint& Point = ActiveScan.Points.AddDefaulted_GetRef();
            const FVector SensorLocalCm = SensorTransform.InverseTransformPosition(Hit.ImpactPoint);
            // 正式数据集声明为右手 FLU（前、左、上）米制；不能直接输出 UE 的前、右、上局部坐标。
            Point.PositionMeters = FVector3f(
                FCoordinateConverter::UnrealCentimetersToFrontLeftUpMeters(SensorLocalCm));
            // 以入射方向和表面法线夹角的余弦近似回波强度，正入射最强、掠射最弱。
            Point.Intensity = FMath::Clamp(FVector::DotProduct(-WorldDirection, Hit.ImpactNormal), 0.0f, 1.0f);
            Point.RelativeTimeSeconds = LocalRayDirections.Num() > 1
                ? static_cast<float>(NextRayIndex) / static_cast<float>(LocalRayDirections.Num() - 1) / UpdateFrequencyHz
                : 0.0f;

            if (const AActor* HitActor = Hit.GetActor())
            {
                if (const USemanticObjectComponent* Semantic = HitActor->FindComponentByClass<USemanticObjectComponent>())
                {
                    Point.SemanticId = static_cast<uint16>(FMath::Clamp(Semantic->SemanticId, 0, 65535));
                    Point.InstanceId =
                        Semantic->InstanceId > 0 ? static_cast<uint32>(Semantic->InstanceId) : 0u;
                }
            }
        }

        ++ActiveScan.CompletedRayCount;
    }

    if (NextRayIndex >= LocalRayDirections.Num())
    {
        FinalizeScan();
    }
}

/** 结束当前扫描、停用 Tick、提交给 Subsystem 并广播扫描结果。 */
void USimLidarSensorComponent::FinalizeScan()
{
    ActiveScan.bCompleteRevolution = ActiveScan.CompletedRayCount == ActiveScan.ExpectedRayCount;
    SetComponentTickEnabled(false);
    ActiveRequest.Reset();

    // 观察者必须看到完整扫描；广播完成后再把所有权移动到 Subsystem。
    ScanCompleteDelegate.Broadcast(ActiveScan);
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->SubmitLidar(MoveTemp(ActiveScan));
        }
    }

}

