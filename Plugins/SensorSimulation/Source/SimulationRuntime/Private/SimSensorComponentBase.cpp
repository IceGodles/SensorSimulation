#include "SimSensorComponentBase.h"
#include "SimulationSubsystem.h"
#include "Engine/World.h"

/** 构造并初始化 USimSensorComponentBase 的默认状态。 */
USimSensorComponentBase::USimSensorComponentBase()
{
    PrimaryComponentTick.bCanEverTick = false;
}

/** 在组件开始运行时完成注册或初始化工作。 */
void USimSensorComponentBase::BeginPlay()
{
    Super::BeginPlay();
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->RegisterSensor(*this);
        }
    }
}

/** 在组件停止运行时注销自身并释放关联关系。 */
void USimSensorComponentBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->UnregisterSensor(*this);
        }
    }
    Super::EndPlay(EndPlayReason);
}
