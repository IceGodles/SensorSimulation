#pragma once

#include "CoreMinimal.h"

/** 将 Session 主时钟转换为逐传感器到期计划，不访问 World/UObject。 */
class SIMULATIONRUNTIME_API FSensorCapturePlanner
{
public:
    /** 新注册传感器首次采样立即到期；之后根据稳定 SensorGuid 查询自己的时间线。 */
    bool IsDue(const FGuid& SensorGuid, double TimestampSeconds) const;
    /** 一次到期请求后按传感器频率推进 NextDue，跳过已经落后的周期且不产生追赶风暴。 */
    void MarkAttempt(const FGuid& SensorGuid, float UpdateFrequencyHz, double TimestampSeconds);
    void Remove(const FGuid& SensorGuid);
    void Reset();

private:
    TMap<FGuid, double> NextCaptureSeconds;
};
