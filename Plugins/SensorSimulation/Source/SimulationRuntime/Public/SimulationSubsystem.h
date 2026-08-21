#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "FrameAssembler.h"
#include "SemanticRegistry.h"
#include "ExportService.h"
#include "DatasetSession.h"
#include "SimulationScheduler.h"
#include "SimulationSettings.h"
#include "SensorCapturePlanner.h"
#include "SimulationSubsystem.generated.h"

class USemanticObjectComponent;
class USimSensorComponentBase;

UCLASS()
/** 协调世界内传感器、语义对象、时钟与帧聚合的子系统。 */
class SIMULATIONRUNTIME_API USimulationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    /** 仅在真正运行采集的 Game/PIE World 中创建，避免编辑器加载资产时产生空会话。 */
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
/** 初始化世界级传感器仿真子系统。 */
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
/** 清空传感器和语义状态后反初始化子系统。 */
    virtual void Deinitialize() override;
/** 推进仿真时钟、按固定步长发起采集并消费完整帧。 */
    virtual void Tick(float DeltaTime) override;
/** 返回 Unreal Tick 性能统计标识。 */
    virtual TStatId GetStatId() const override;
/** 指示该子系统不在编辑器非运行状态中 Tick。 */
    virtual bool IsTickableInEditor() const override { return false; }

/** 把传感器加入当前世界的弱引用集合并避免重复。 */
    void RegisterSensor(USimSensorComponentBase& Sensor);
/** 从当前世界移除指定传感器的弱引用。 */
    void UnregisterSensor(const USimSensorComponentBase& Sensor);
/** 将语义组件注册到实例编号注册表。 */
    uint32 RegisterSemanticObject(USemanticObjectComponent& Component);
/** 从实例编号注册表注销语义组件。 */
    void UnregisterSemanticObject(const USemanticObjectComponent& Component);
/** 把完成的图像移交给帧聚合器。 */
    bool SubmitImage(FImagePayload&& Image);
/** 把完成的点云移交给帧聚合器。 */
    void SubmitLidar(FLidarScanPayload&& Scan);
/** 注册单通道相机标定参数，会话结束时写入 calibration.json。 */
    void RegisterCalibration(const FCalibration& Calibration);
    /** 注册 Camera Rig 的最新资源与 Readback 指标，会话结束时写入 metadata.json。 */
    void RegisterRendererMetrics(const FCameraRendererMetricsSnapshot& Metrics);

/** 返回当前等待所有模态到齐的帧数量。 */
    int32 GetPendingFrameCount() const { return FrameAssembler.GetPendingFrameCount(); }

private:
    /** 当前世界内已注册传感器的弱引用集合。 */
    TArray<TWeakObjectPtr<USimSensorComponentBase>> Sensors;
    /** 根据稳定 SensorGuid 为混合频率传感器生成逐帧 Capture Plan。 */
    FSensorCapturePlanner CapturePlanner;
    /** 当前世界的语义对象实例编号注册表。 */
    FSemanticRegistry SemanticRegistry;
    /** 负责把异步传感器结果合并为同步帧的聚合器。 */
    FFrameAssembler FrameAssembler;
    /** 负责将完整帧异步写出到磁盘的导出服务。 */
    TUniquePtr<FExportService> ExportService;
    /** 管理数据集采集会话的生命周期和元数据。 */
    TUniquePtr<FDatasetSession> DatasetSession;
    /** 当前仿真或数据集采集会话的序列编号。 */
    uint64 SequenceId = 1;
    /** 下一次发起采集时使用的帧编号。 */
    uint64 NextFrameId = 1;
    /** 当前 Session 启动时捕获的不可变设置，运行中不再读取 Settings CDO。 */
    FSimulationRuntimeSettingsSnapshot SettingsSnapshot;
    /** 独立决定固定时间戳、流水线等待和 Export 背压暂停的调度状态机。 */
    FSimulationScheduler Scheduler;
    /** 会话单调时钟起点，仅用于真实等待超时，不参与确定性采样时间戳。 */
    double SessionStartPlatformSeconds = 0.0;

/** 创建同步帧、采集真值并向所有启用的传感器下发请求。 */
    void RequestFrame(double TimestampSeconds, double CreationTimeSeconds);
    /** 将完整帧非阻塞移交 Export；确定性模式队列满时保留在 FrameAssembler。 */
    bool FlushCompleteFramesToExport();
/** 遍历带语义组件的 Actor 并采集位姿、包围盒和速度真值。 */
    TArray<FObjectGroundTruth> CaptureGroundTruth() const;
    /** 验证图像尺寸、紧密 RGBA8 布局以及 Semantic 标签集合与通道约束。 */
    bool ValidateImagePayload(const FImagePayload& Image) const;
};
