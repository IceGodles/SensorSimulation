#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "FrameAssembler.h"
#include "SemanticRegistry.h"
#include "SimulationSubsystem.generated.h"

class USemanticObjectComponent;
class USimSensorComponentBase;

UCLASS()
/** 协调世界内传感器、语义对象、时钟与帧聚合的子系统。 */
class SIMULATIONRUNTIME_API USimulationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
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

/** 返回当前等待所有模态到齐的帧数量。 */
    int32 GetPendingFrameCount() const { return FrameAssembler.GetPendingFrameCount(); }

private:
    /** 当前世界内已注册传感器的弱引用集合。 */
    TArray<TWeakObjectPtr<USimSensorComponentBase>> Sensors;
    /** 当前世界的语义对象实例编号注册表。 */
    FSemanticRegistry SemanticRegistry;
    /** 负责把异步传感器结果合并为同步帧的聚合器。 */
    FFrameAssembler FrameAssembler;
    /** 当前仿真或数据集采集会话的序列编号。 */
    uint64 SequenceId = 1;
    /** 下一次发起采集时使用的帧编号。 */
    uint64 NextFrameId = 1;
    /** 尚未消耗为固定采样步长的游戏时间余量。 */
    double AccumulatedSeconds = 0.0;
    /** 子系统启动以来累计的仿真时间。 */
    double SimulationSeconds = 0.0;

/** 创建同步帧、采集真值并向所有启用的传感器下发请求。 */
    void RequestFrame(double TimestampSeconds);
/** 遍历带语义组件的 Actor 并采集位姿、包围盒和速度真值。 */
    TArray<FObjectGroundTruth> CaptureGroundTruth() const;
    /** 验证图像尺寸、紧密 RGBA8 布局以及 Semantic 标签集合与通道约束。 */
    bool ValidateImagePayload(const FImagePayload& Image) const;
};
