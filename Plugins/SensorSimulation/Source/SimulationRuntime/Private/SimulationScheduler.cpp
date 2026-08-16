#include "SimulationScheduler.h"

/** 初始化独立采样时间轴。 */
void FSimulationScheduler::Initialize(
    const ESimulationMode InMode,
    const double InFixedStepSeconds)
{
    Mode = InMode;
    FixedStepSeconds = FMath::Max(0.001, InFixedStepSeconds);
    RealtimeAccumulatorSeconds = 0.0;
    SimulationSeconds = 0.0;
    PauseReason = ESimulationSchedulerPauseReason::None;
}

/** 根据流水线与 Export 状态决定是否推进一个采样步。 */
TOptional<double> FSimulationScheduler::Poll(
    const double DeltaTime,
    const bool bFramePipelineIdle,
    const bool bExportHasCapacity)
{
    PauseReason = ESimulationSchedulerPauseReason::None;

    if (Mode == ESimulationMode::DeterministicDataset)
    {
        // 背压优先报告：即使当前还有完整帧停留在 FrameAssembler，根因也是 Export 无容量。
        if (!bExportHasCapacity)
        {
            PauseReason = ESimulationSchedulerPauseReason::ExportBackpressure;
            return {};
        }
        if (!bFramePipelineIdle)
        {
            PauseReason = ESimulationSchedulerPauseReason::FramePipelineBusy;
            return {};
        }

        // 确定性模式不读取 DeltaTime；每次获准只推进一个固定步，绝不追赶游戏帧。
        SimulationSeconds += FixedStepSeconds;
        return SimulationSeconds;
    }

    RealtimeAccumulatorSeconds += FMath::Max(0.0, DeltaTime);
    if (RealtimeAccumulatorSeconds < FixedStepSeconds)
    {
        return {};
    }

    // 一次游戏 Tick 最多发起一个采样，避免卡顿后瞬间制造大量在途 Capture。
    RealtimeAccumulatorSeconds = FMath::Fmod(RealtimeAccumulatorSeconds, FixedStepSeconds);
    SimulationSeconds += FixedStepSeconds;
    return SimulationSeconds;
}
