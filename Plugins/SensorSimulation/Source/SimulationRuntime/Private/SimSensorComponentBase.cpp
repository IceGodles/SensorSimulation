#include "SimSensorComponentBase.h"
#include "SimulationSubsystem.h"
#include "Engine/World.h"

/** 构造并初始化 USimSensorComponentBase 的默认状态。 */
USimSensorComponentBase::USimSensorComponentBase()
{
    PrimaryComponentTick.bCanEverTick = false;
}

/** 确保每个持久传感器组件拥有非零身份。 */
void USimSensorComponentBase::EnsureSensorGuid()
{
    if (!SensorGuid.IsValid())
    {
        SensorGuid = FGuid::NewGuid();
#if WITH_EDITOR
        if (!HasAnyFlags(RF_ClassDefaultObject | RF_Transient))
        {
            Modify();
            MarkPackageDirty();
        }
#endif
    }
}

/** 新建组件时立即分配身份，保存关卡后可跨运行保持稳定。 */
void USimSensorComponentBase::OnComponentCreated()
{
    Super::OnComponentCreated();
    EnsureSensorGuid();
}

/** 为升级前保存的组件补齐身份。 */
void USimSensorComponentBase::PostLoad()
{
    Super::PostLoad();
    EnsureSensorGuid();
}

/** 普通复制必须产生新传感器；PIE 世界复制保持编辑器组件身份。 */
void USimSensorComponentBase::PostDuplicate(const bool bDuplicateForPIE)
{
    Super::PostDuplicate(bDuplicateForPIE);
    if (!bDuplicateForPIE)
    {
        SensorGuid = FGuid::NewGuid();
    }
    else
    {
        EnsureSensorGuid();
    }
}
/** 在组件开始运行时完成注册或初始化工作。 */
void USimSensorComponentBase::BeginPlay()
{
    Super::BeginPlay();
    EnsureSensorGuid();
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
    PrepareForShutdown();
    if (UWorld* World = GetWorld())
    {
        if (USimulationSubsystem* Subsystem = World->GetSubsystem<USimulationSubsystem>())
        {
            Subsystem->UnregisterSensor(*this);
        }
    }
    Super::EndPlay(EndPlayReason);
}
