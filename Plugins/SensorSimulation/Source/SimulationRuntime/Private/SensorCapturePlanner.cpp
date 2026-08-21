#include "SensorCapturePlanner.h"

bool FSensorCapturePlanner::IsDue(const FGuid& SensorGuid, const double TimestampSeconds) const
{
    const double* NextDue = NextCaptureSeconds.Find(SensorGuid);
    return !NextDue || TimestampSeconds + UE_DOUBLE_SMALL_NUMBER >= *NextDue;
}

void FSensorCapturePlanner::MarkAttempt(
    const FGuid& SensorGuid,
    const float UpdateFrequencyHz,
    const double TimestampSeconds)
{
    const double PeriodSeconds = 1.0 / FMath::Max(0.01, static_cast<double>(UpdateFrequencyHz));
    double& NextDue = NextCaptureSeconds.FindOrAdd(SensorGuid, TimestampSeconds);
    do
    {
        NextDue += PeriodSeconds;
    }
    while (NextDue <= TimestampSeconds + UE_DOUBLE_SMALL_NUMBER);
}

void FSensorCapturePlanner::Remove(const FGuid& SensorGuid)
{
    NextCaptureSeconds.Remove(SensorGuid);
}

void FSensorCapturePlanner::Reset()
{
    NextCaptureSeconds.Reset();
}
