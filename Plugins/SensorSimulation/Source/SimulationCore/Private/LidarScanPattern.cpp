#include "LidarScanPattern.h"

/** 计算完整扫描包含的射线总数，并将负数配置安全地视为零。 */
uint32 FLidarScanPattern::GetRayCount() const
{
    return static_cast<uint32>(FMath::Max(0, Channels) * FMath::Max(0, HorizontalSamples));
}

/** 根据水平采样和垂直通道角生成归一化的局部射线方向。 */
void FLidarScanPattern::BuildDirections(TArray<FVector3f>& OutLocalDirections) const
{
    OutLocalDirections.Reset(GetRayCount());
    if (Channels <= 0 || HorizontalSamples <= 0)
    {
        return;
    }

    // 固定遍历顺序保证射线索引稳定：每个方位角下依次排列所有垂直通道。
    for (int32 HorizontalIndex = 0; HorizontalIndex < HorizontalSamples; ++HorizontalIndex)
    {
        const float HorizontalAlpha = HorizontalSamples > 1
            ? static_cast<float>(HorizontalIndex) / static_cast<float>(HorizontalSamples - 1)
            : 0.0f;
        const float Azimuth = FMath::Lerp(HorizontalFovStartDegrees, HorizontalFovEndDegrees, HorizontalAlpha);

        for (int32 Channel = 0; Channel < Channels; ++Channel)
        {
                // 真实雷达常有非均匀线束角：若提供逐线标定值则优先使用，否则在上下视场间均匀插值。
            const float Elevation = ExplicitVerticalAnglesDegrees.IsValidIndex(Channel)
                ? ExplicitVerticalAnglesDegrees[Channel]
                : FMath::Lerp(
                    VerticalFovUpperDegrees,
                    VerticalFovLowerDegrees,
                    Channels > 1 ? static_cast<float>(Channel) / static_cast<float>(Channels - 1) : 0.0f);

            const FRotator RayRotation(Elevation, Azimuth, 0.0f);
            OutLocalDirections.Add(FVector3f(RayRotation.Vector().GetSafeNormal()));
        }
    }
}
