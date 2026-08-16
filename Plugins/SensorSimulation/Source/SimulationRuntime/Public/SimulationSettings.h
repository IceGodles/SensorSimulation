#pragma once

#include "Engine/DeveloperSettings.h"
#include "SimulationTypes.h"
#include "SimulationSettings.generated.h"

UCLASS(Config=SensorSimulation, DefaultConfig, meta=(DisplayName="Sensor Simulation"))
/** SensorSimulation 插件的项目级开发者设置。 */
class SIMULATIONRUNTIME_API USimulationSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** 运行时采用实时推进还是确定性数据集时钟。 */
    UPROPERTY(Config, EditAnywhere, Category="Clock")
    ESimulationMode SimulationMode = ESimulationMode::Realtime;

    /** 固定采集时钟的步长，单位为秒。 */
    UPROPERTY(Config, EditAnywhere, Category="Clock", meta=(ClampMin="0.001"))
    double FixedStepSeconds = 0.05;

    /** 导出服务允许积压的最大完整帧数量。 */
    UPROPERTY(Config, EditAnywhere, Category="Export", meta=(ClampMin="1"))
    int32 MaxPendingFrames = 8;

    /** 数据集文件的输出根目录。 */
    UPROPERTY(Config, EditAnywhere, Category="Export")
    FDirectoryPath DatasetRoot;

    /** 帧等待所有模态到齐的超时时间，超时帧将被丢弃并记录日志。单位为秒。 */
    UPROPERTY(Config, EditAnywhere, Category="Frame", meta=(ClampMin="0.1"))
    double FrameTimeoutSeconds = 2.0;

    /** 确定性数据集模式使用的随机种子，相同种子保证可复现的 Actor 轨迹和帧序列。 */
    UPROPERTY(Config, EditAnywhere, Category="Deterministic")
    int32 RandomSeed = 42;

    /** 将配置目录解析为稳定绝对路径：空值和相对值均锚定 Project/Saved。 */
    static FString ResolveDatasetRoot(const FDirectoryPath& ConfiguredRoot);
};

/** 当前 Dataset Session 从 Settings CDO 一次性复制的不可变运行参数。 */
struct SIMULATIONRUNTIME_API FSimulationRuntimeSettingsSnapshot
{
    ESimulationMode SimulationMode = ESimulationMode::Realtime;
    double FixedStepSeconds = 0.05;
    int32 MaxPendingFrames = 8;
    FString DatasetRoot;
    double FrameTimeoutSeconds = 2.0;
    int32 RandomSeed = 42;

    /** 从 Settings 对象捕获一份值语义快照。 */
    static FSimulationRuntimeSettingsSnapshot Capture(const USimulationSettings& Settings);
};
