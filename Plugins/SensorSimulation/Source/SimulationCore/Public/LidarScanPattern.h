#pragma once

#include "CoreMinimal.h"

/** 机械式多线激光雷达的扫描图案配置与方向生成器。 */
struct SIMULATIONCORE_API FLidarScanPattern
{
    /** 垂直激光通道数，或相机阵列中需要创建的输出通道配置。 */
    int32 Channels = 16;
    /** 完整水平扫描周期内的方位角采样数量。 */
    int32 HorizontalSamples = 512;
    /** 水平扫描视场的起始方位角，单位为度。 */
    float HorizontalFovStartDegrees = 0.0f;
    /** 水平扫描视场的结束方位角，单位为度。 */
    float HorizontalFovEndDegrees = 360.0f;
    /** 垂直视场最高线束的仰角，单位为度。 */
    float VerticalFovUpperDegrees = 10.0f;
    /** 垂直视场最低线束的俯角，单位为度。 */
    float VerticalFovLowerDegrees = -10.0f;
    /** 可选的逐通道垂直标定角；存在时覆盖均匀视场插值。 */
    TArray<float> ExplicitVerticalAnglesDegrees;

/** 计算完整扫描包含的射线总数，并将负数配置安全地视为零。 */
    uint32 GetRayCount() const;
/** 根据水平采样和垂直通道角生成归一化的局部射线方向。 */
    void BuildDirections(TArray<FVector3f>& OutLocalDirections) const;
};
