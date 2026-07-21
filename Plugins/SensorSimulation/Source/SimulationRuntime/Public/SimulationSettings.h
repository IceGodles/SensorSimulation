#pragma once

#include "Engine/DeveloperSettings.h"
#include "SimulationSettings.generated.h"

UENUM(BlueprintType)
/** 运行时子系统使用的时钟模式。 */
enum class ERuntimeMode : uint8
{
    Realtime,
    DeterministicDataset
};

UCLASS(Config=SensorSimulation, DefaultConfig, meta=(DisplayName="Sensor Simulation"))
/** SensorSimulation 插件的项目级开发者设置。 */
class SIMULATIONRUNTIME_API USimulationSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** 运行时采用实时推进还是确定性数据集时钟。 */
    UPROPERTY(Config, EditAnywhere, Category="Clock")
    ERuntimeMode SimulationMode = ERuntimeMode::Realtime;

    /** 固定采集时钟的步长，单位为秒。 */
    UPROPERTY(Config, EditAnywhere, Category="Clock", meta=(ClampMin="0.001"))
    double FixedStepSeconds = 0.05;

    /** 导出服务允许积压的最大完整帧数量。 */
    UPROPERTY(Config, EditAnywhere, Category="Export", meta=(ClampMin="1"))
    int32 MaxPendingFrames = 8;

    /** 数据集文件的输出根目录。 */
    UPROPERTY(Config, EditAnywhere, Category="Export")
    FDirectoryPath DatasetRoot;
};
