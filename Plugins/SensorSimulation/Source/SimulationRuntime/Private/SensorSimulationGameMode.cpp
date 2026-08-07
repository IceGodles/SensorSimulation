#include "SensorSimulationGameMode.h"

ASensorSimulationGameMode::ASensorSimulationGameMode()
{
    // 传感器相机由场景中的 CameraRig 驱动，不需要玩家 Pawn 或 HUD。
    DefaultPawnClass = nullptr;
    HUDClass = nullptr;
    bStartPlayersAsSpectators = true;
}
