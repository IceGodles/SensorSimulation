#pragma once

#include "Components/SceneComponent.h"
#include "CameraChannel.h"
#include "SimulationTypes.h"
#include "CameraRigComponent.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

DECLARE_MULTICAST_DELEGATE_OneParam(FOnCaptureSubmitted, const FCaptureRequest&);

UCLASS(ClassGroup=(SensorSimulation), meta=(BlueprintSpawnableComponent))
/** 管理多模态场景捕获组件与相机标定的传感器阵列组件。 */
class SIMULATIONRENDERER_API UCameraRigComponent : public USceneComponent
{
    GENERATED_BODY()

public:
/** 构造并初始化 UCameraRigComponent 的默认状态。 */
    UCameraRigComponent();

    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    FName SensorName = TEXT("FrontCamera");

    /** 相机的水平视场角，单位为度。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor", meta=(ClampMin="1.0", ClampMax="170.0"))
    float HorizontalFovDegrees = 90.0f;

    /** 垂直激光通道数，或相机阵列中需要创建的输出通道配置。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    TArray<FCameraChannelConfig> Channels;

/** 组件注册后根据配置创建运行时捕获通道。 */
    virtual void OnRegister() override;
/** 组件注销前销毁动态创建的捕获通道。 */
    virtual void OnUnregister() override;

/** 触发所有通道延迟采集并广播本次请求。 */
    void SubmitCapture(const FCaptureRequest& Request);
/** 由分辨率和水平视场角计算针孔相机内参。 */
    FCalibration BuildCalibration(const FCameraChannelConfig& Channel) const;

/** 返回采集请求已提交事件，供调用方绑定监听器。 */
    FOnCaptureSubmitted& OnCaptureSubmitted() { return CaptureSubmittedDelegate; }

private:
/** 一个已创建通道的配置、捕获组件和渲染目标。 */
    struct FChannelRuntime
    {
        /** 此运行时对象对应的可编辑通道或雷达配置。 */
        FCameraChannelConfig Config;
        /** 实际执行场景渲染的动态场景捕获组件。 */
        TObjectPtr<USceneCaptureComponent2D> Capture = nullptr;
        /** 接收此通道渲染结果的纹理渲染目标。 */
        TObjectPtr<UTextureRenderTarget2D> Target = nullptr;
    };

    /** 根据有效配置动态创建的全部运行时捕获通道。 */
    TArray<FChannelRuntime> RuntimeChannels;
    /** 采集命令提交后向外部监听器广播的多播委托。 */
    FOnCaptureSubmitted CaptureSubmittedDelegate;

/** 为每个有效配置创建场景捕获组件和匹配的渲染目标。 */
    void BuildChannels();
/** 销毁所有动态捕获组件并清空运行时通道。 */
    void DestroyChannels();
/** 根据 RGB、标签或深度模态配置场景捕获参数。 */
    void ConfigureCapture(FChannelRuntime& Channel);
};
