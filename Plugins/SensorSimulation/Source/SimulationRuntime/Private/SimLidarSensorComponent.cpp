#include "SimLidarSensorComponent.h"
#include "SemanticObjectComponent.h"
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

/** 在传感器空闲且启用时初始化一次新的采集任务。 */
void USimLidarSensorComponent::RequestCapture(const FCaptureRequest& Request)
{
    if (!bSensorEnabled || ActiveRequest.IsSet())
    {
        return;
    }

    ActiveRequest = Request;
    ActiveScan = FLidarScanPayload();
    ActiveScan.Header = Request.Header;
    ActiveScan.SensorName = SensorName;
    ActiveScan.SensorToEgo = Request.SensorToEgo;
    ActiveScan.ExpectedRayCount = static_cast<uint32>(LocalRayDirections.Num());
    ActiveScan.Points.Reserve(LocalRayDirections.Num());
    NextRayIndex = 0;
    SetComponentTickEnabled(true);
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

    const FTransform SensorTransform = Owner->GetActorTransform();
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
            Point.PositionMeters = FVector3f(SensorLocalCm / 100.0f);
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
                    Point.InstanceId = static_cast<uint32>(FMath::Max(0, Semantic->InstanceId));
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

/** 结束当前扫描、停用 Tick 并广播扫描结果。 */
void USimLidarSensorComponent::FinalizeScan()
{
    ActiveScan.bCompleteRevolution = ActiveScan.CompletedRayCount == ActiveScan.ExpectedRayCount;
    SetComponentTickEnabled(false);
    ActiveRequest.Reset();
    ScanCompleteDelegate.Broadcast(ActiveScan);
}

