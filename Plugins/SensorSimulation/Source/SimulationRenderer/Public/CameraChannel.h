#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"
#include "CameraChannel.generated.h"

UENUM(BlueprintType)
/** 相机阵列可输出的渲染通道类型。 */
enum class ECameraChannelType : uint8
{
    Rgb,
    Semantic,
    Depth,
    Instance
};

USTRUCT(BlueprintType)
/** 单个相机输出通道的可编辑配置。 */
struct SIMULATIONRENDERER_API FCameraChannelConfig
{
    GENERATED_BODY()

    /**
     * 使用全局唯一标识符来表示通道的持久身份。
     *
     * ChannelType 描述“输出什么”，ChannelGuid 描述“是哪一条配置”。分离两者后，
     * 数组重排或未来同模态扩展不会让 Calibration 和运行时资源错误地串到另一通道。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Sensor")
    FGuid ChannelGuid;

    /** 此相机通道需要生成的 RGB、语义、深度或实例模态。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    ECameraChannelType ChannelType = ECameraChannelType::Rgb;

    /** 此通道渲染目标的宽高，单位为像素。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    FIntPoint Resolution = FIntPoint(1280, 720);

    /** 控制是否为此配置创建并采集运行时通道。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    bool bEnabled = true;

    /** 控制渲染目标是否绕过伽马变换，数值和标签通道通常应启用。 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sensor")
    bool bForceLinearGamma = false;

/** 将相机通道类型映射为帧数据模态位。 */
    EPayloadType ToPayloadType() const;
};
