#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"

namespace UE::SensorSimulation::ImageReadback
{
/** GPU Copy 通常保持底层像素格式 BGRA/RGBA
 * 将 BGRA/RGBA staging 行复制为协议统一的紧密 RGBA8 字节。 */
/* 独立函数让 RowPitch 与通道顺序逻辑可以脱离 GPU 做自动化测试，
 * 避免奇数分辨率 或 D3D 后端行填充导致图像错行，也避免把 BGRA 误当成 RGBA。
 */
bool CopyRgba8ToCanonical(
    const uint8* Source,
    int32 RowPitchInPixels,
    EPixelFormat SourceFormat,
    FIntPoint ImageSize,
    TArray<uint8>& OutBytes);

/**
 * 把带 GPU RowPitch 的 PF_R32_FLOAT 或 PF_A32B32G32R32F SceneDepth 转成紧密排列的 float32 米。
 * RGBA32F 只提取 SCS_SceneDepth 写入的逻辑 R 通道；UE SceneDepth 使用厘米，
 * 在协议边界统一乘 0.01，避免下游重复单位换算。
 */
bool CopyDepthR32FloatToMeters(
    const uint8* Source,
    int32 RowPitchInPixels,
    EPixelFormat SourceFormat,
    FIntPoint ImageSize,
    TArray<uint8>& OutBytes);

/**
 * 把带 GPU RowPitch 的 PF_R32_UINT 实例标签复制为紧密排列的 uint32。
 *
 * 该转换只移除行填充，不做归一化、通道交换或数值变换，确保 255 以上和最高位为 1
 * 的 InstanceId 在 GPU→CPU 边界保持逐位一致。
 */
bool CopyR32UintToCanonical(
    const uint8* Source,
    int32 RowPitchInPixels,
    EPixelFormat SourceFormat,
    FIntPoint ImageSize,
    TArray<uint8>& OutBytes);
}
