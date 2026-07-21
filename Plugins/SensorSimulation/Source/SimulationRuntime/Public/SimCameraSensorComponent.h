#pragma once

#include "SimSensorComponentBase.h"
#include "SimCameraSensorComponent.generated.h"

class UCameraRigComponent;

/** 把 Renderer 相机捕获与 Runtime 帧聚合子系统连接起来的传感器适配组件。 */
UCLASS(ClassGroup=(SensorSimulation), meta=(BlueprintSpawnableComponent))
class SIMULATIONRUNTIME_API USimCameraSensorComponent : public USimSensorComponentBase
{
    GENERATED_BODY()

public:
    /** 初始化持续轮询异步图像完成队列所需的组件 Tick。 */
    USimCameraSensorComponent();

    /** 提供实际多通道场景捕获与 GPU Readback 的相机阵列组件。 */
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Camera")
    TObjectPtr<UCameraRigComponent> CameraRig = nullptr;

    /** 查找关联 Camera Rig、同步传感器名称并注册到 Runtime Subsystem。 */
    virtual void BeginPlay() override;
    /** 非阻塞提取全部已完成图像并把所有权移交给 Runtime Subsystem。 */
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    /** 返回 Camera Rig 当前启用的 RGB/Semantic 模态。 */
    virtual EPayloadType GetPayloadTypes() const override;
    /** 把同步帧请求转发给 Camera Rig。 */
    virtual void RequestCapture(const FCaptureRequest& Request) override;
};
