#pragma once

#include "GameFramework/GameModeBase.h"
#include "SensorSimulationGameMode.generated.h"

/**
 * 数据采集专用 GameMode。
 * 不自动生成 DefaultPawn，避免不可控的球形 Pawn 进入 RGB/Depth/Semantic 基准图。
 */
UCLASS()
class SIMULATIONRUNTIME_API ASensorSimulationGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ASensorSimulationGameMode();
};
