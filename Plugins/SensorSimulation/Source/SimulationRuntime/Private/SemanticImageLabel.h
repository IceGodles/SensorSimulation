#pragma once

#include "CoreMinimal.h"

namespace UE::SensorSimulation::SemanticLabels
{
inline constexpr int32 BackgroundId = 0;
inline constexpr int32 MinObjectId = 1;
inline constexpr int32 MaxObjectId = 255;

/**
 * Semantic PNG/CustomStencil 只有 8 位，并且 0 保留给背景。
 * 非法对象 ID 明确返回失败，不再静默 Clamp 成另一个有效类别。
 */
inline bool TryConvertToImageId(const int32 SemanticId, uint8& OutImageId)
{
    if (SemanticId < MinObjectId || SemanticId > MaxObjectId)
    {
        OutImageId = static_cast<uint8>(BackgroundId);
        return false;
    }

    OutImageId = static_cast<uint8>(SemanticId);
    return true;
}
}
