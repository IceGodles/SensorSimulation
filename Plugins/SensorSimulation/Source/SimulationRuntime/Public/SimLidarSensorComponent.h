#pragma once

#include "LidarScanPattern.h"
#include "SimSensorComponentBase.h"
#include "SimLidarSensorComponent.generated.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnLidarScanComplete, const FLidarScanPayload&);

USTRUCT(BlueprintType)
/** 可在编辑器和蓝图中调整的激光雷达参数。 */
struct SIMULATIONRUNTIME_API FSimLidarConfig
{
    GENERATED_BODY()

    /** 垂直激光通道数，或相机阵列中需要创建的输出通道配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR", meta=(ClampMin="1"))
    int32 Channels = 16;

    /** 完整水平扫描周期内的方位角采样数量。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR", meta=(ClampMin="1"))
    int32 HorizontalSamples = 512;

    /** 垂直视场最高线束的仰角，单位为度。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR")
    float VerticalFovUpperDegrees = 10.0f;

    /** 垂直视场最低线束的俯角，单位为度。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR")
    float VerticalFovLowerDegrees = -10.0f;

    /** 雷达可感知的最小距离，单位为米。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR", meta=(ClampMin="0.0"))
    float MinRangeMeters = 0.5f;

    /** 雷达射线检测的最大距离，单位为米。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR", meta=(ClampMin="0.1"))
    float MaxRangeMeters = 100.0f;

    /** 每个组件 Tick 最多执行的射线数量。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR", meta=(ClampMin="1"))
    int32 RaysPerTick = 1024;

    /** 雷达射线检测使用的 Unreal 碰撞通道。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR")
    TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;
};

UCLASS(ClassGroup=(SensorSimulation), meta=(BlueprintSpawnableComponent))
/** 以分批射线检测模拟多线激光雷达的传感器组件。 */
class SIMULATIONRUNTIME_API USimLidarSensorComponent : public USimSensorComponentBase
{
    GENERATED_BODY()

public:
/** 构造并初始化 USimLidarSensorComponent 的默认状态。 */
    USimLidarSensorComponent();

    /** 此运行时对象对应的可编辑通道或雷达配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LiDAR")
    FSimLidarConfig Config;

/** 在组件开始运行时完成注册或初始化工作。 */
    virtual void BeginPlay() override;
/** 在组件 Tick 中推进当前分批扫描任务。 */
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
/** 在传感器空闲且启用时初始化一次新的采集任务。 */
    /** 返回该传感器生产 LiDAR 模态。 */
    virtual EPayloadType GetPayloadTypes() const override { return EPayloadType::Lidar; }
    /** 在传感器空闲且启用时初始化一次新的采集任务。 */
    virtual void RequestCapture(const FCaptureRequest& Request) override;

/** 返回扫描完成事件，供调用方绑定监听器。 */
    FOnLidarScanComplete& OnScanComplete() { return ScanCompleteDelegate; }

private:
    /** 按稳定扫描顺序缓存的传感器局部单位射线方向。 */
    TArray<FVector3f> LocalRayDirections;
    /** 当前正在执行的采集请求；未设置表示传感器空闲。 */
    TOptional<FCaptureRequest> ActiveRequest;
    /** 正在累积回波点和扫描进度的点云载荷。 */
    FLidarScanPayload ActiveScan;
    /** 下一批检测从局部射线数组中的哪个索引开始。 */
    int32 NextRayIndex = 0;
    /** 扫描完成后向外部监听器广播点云的多播委托。 */
    FOnLidarScanComplete ScanCompleteDelegate;

/** 根据当前雷达配置重新生成局部扫描射线。 */
    void RebuildPattern();
/** 执行一批射线检测并生成带语义和相对时间的回波点。 */
    void TraceBatch();
/** 结束当前扫描、停用 Tick 并广播扫描结果。 */
    void FinalizeScan();
};

