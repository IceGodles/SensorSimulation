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
    /** 解除配置通知，避免组件结束后继续接收热更新回调。 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    /** 非阻塞提取全部已完成图像并把所有权移交给 Runtime Subsystem。 */
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
    /** 返回 Camera Rig 当前启用的图像模态位。 */
    virtual EPayloadType GetPayloadTypes() const override;
    /** 返回 Camera Rig 当前启用的逐 ChannelGuid 图像通道。 */
    virtual TArray<FExpectedImageChannel> GetExpectedImageChannels() const override;
    /** 把同步帧请求转发给 Camera Rig。 */
    virtual ECaptureRequestResult RequestCapture(const FCaptureRequest& Request) override;
    virtual void PrepareForShutdown() override;
    virtual int32 GetInFlightCaptureCount() const override;

private:
    /** 把每条正式图像通道的最新内外参分别更新到 Dataset Session。 */
    void RegisterCurrentCalibration();
    /** 把 Camera Rig 的资源、Readback 与按通道延迟快照更新到 Dataset Session。 */
    void RegisterCurrentRendererMetrics();
    /** 排出 Readback Manager 已经完成并拥有独立 CPU 内存的图像。 */
    void DrainCompletedImages();
    /** 控制指标采样频率，避免每帧构造并锁定完整统计快照。 */
    double RendererMetricsElapsedSeconds = 0.0;
};
