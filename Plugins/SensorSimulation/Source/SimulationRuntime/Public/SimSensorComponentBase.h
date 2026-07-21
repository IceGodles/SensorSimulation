#pragma once

#include "Components/ActorComponent.h"
#include "SimulationTypes.h"
#include "SimSensorComponentBase.generated.h"

UCLASS(Abstract, ClassGroup=(SensorSimulation))
/** 所有世界传感器组件的注册与采集接口基类。 */
class SIMULATIONRUNTIME_API USimSensorComponentBase : public UActorComponent
{
    GENERATED_BODY()

public:
/** 构造并初始化 USimSensorComponentBase 的默认状态。 */
    USimSensorComponentBase();

    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    FName SensorName = NAME_None;

    /** 传感器期望的更新频率，单位为赫兹。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor", meta=(ClampMin="0.01"))
    float UpdateFrequencyHz = 10.0f;

    /** 控制子系统是否向此传感器下发采集请求。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    bool bSensorEnabled = true;

/** 在组件开始运行时完成注册或初始化工作。 */
    virtual void BeginPlay() override;
/** 在组件停止运行时注销自身并释放关联关系。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
/** 在传感器空闲且启用时初始化一次新的采集任务。 */
    /** 返回该传感器一次请求能够生产的模态位集合。 */
    virtual EPayloadType GetPayloadTypes() const PURE_VIRTUAL(USimSensorComponentBase::GetPayloadTypes, return EPayloadType::None;);
    /** 在传感器空闲且启用时初始化一次新的采集任务。 */
    virtual void RequestCapture(const FCaptureRequest& Request) PURE_VIRTUAL(USimSensorComponentBase::RequestCapture, );
};
