#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 调度器当前不能发起新采样步的原因。 */
enum class ESimulationSchedulerPauseReason : uint8
{
    /** 调度器可正常推进，或实时模式尚未累计到下一个采样点。 */
    None,
    /** 上一个同步帧仍在等待传感器 Payload 或等待移交 Export。 */
    FramePipelineBusy,
    /** Export Queue 已满，确定性时间轴主动保持不动。 */
    ExportBackpressure
};

/**
 * 与游戏帧率解耦的采样时间轴状态机。
 *
 * 确定性模式完全忽略 Tick DeltaTime，只在上一帧流水线空闲且 Export 有容量时推进一个
 * FixedStep；Realtime 模式仍使用 DeltaTime 累积采样。实际访问 World/UObject 的捕获命令
 * 仍由 Subsystem 在游戏线程安全点执行。
 */
class SIMULATIONRUNTIME_API FSimulationScheduler
{
public:
    /** 用会话设置初始化模式和固定步长，并把时间轴复位到零。 */
    void Initialize(ESimulationMode InMode, double InFixedStepSeconds);

    /**
     * 评估当前是否应发起一帧；返回值存在时即为该帧固定的仿真时间戳。
     * 确定性模式不会读取 DeltaTime，因而渲染帧率变化不会改变采样序列。
     */
    TOptional<double> Poll(
        double DeltaTime,
        bool bFramePipelineIdle,
        bool bExportHasCapacity);

    /** 返回已经提交到采集流水线的最新仿真时间。 */
    double GetSimulationSeconds() const { return SimulationSeconds; }
    /** 返回当前显式暂停原因，供诊断和自动化测试读取。 */
    ESimulationSchedulerPauseReason GetPauseReason() const { return PauseReason; }
    /** 返回当前是否处于确定性数据集模式。 */
    bool IsDeterministic() const { return Mode == ESimulationMode::DeterministicDataset; }

private:
    /** 当前会话采用的运行模式。 */
    ESimulationMode Mode = ESimulationMode::Realtime;
    /** 相邻采样时间戳之间的固定间隔。 */
    double FixedStepSeconds = 0.05;
    /** Realtime 模式尚未转化为采样步的游戏时间余量。 */
    double RealtimeAccumulatorSeconds = 0.0;
    /** 最新一次已发起采样的确定性时间戳。 */
    double SimulationSeconds = 0.0;
    /** 最近一次 Poll 得出的显式暂停原因。 */
    ESimulationSchedulerPauseReason PauseReason = ESimulationSchedulerPauseReason::None;
};
