#include "DatasetSession.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformMisc.h"

namespace
{
/** 把公共模态枚举转换为稳定的 metadata/calibration 文本，不输出依赖枚举序号的裸整数。 */
const TCHAR* PayloadTypeToString(const EPayloadType PayloadType)
{
    switch (PayloadType)
    {
    case EPayloadType::Rgb: return TEXT("rgb");
    case EPayloadType::Semantic: return TEXT("semantic");
    case EPayloadType::Depth: return TEXT("depth");
    case EPayloadType::Instance: return TEXT("instance");
    default: return TEXT("unknown");
    }
}
}

FDatasetSession::FDatasetSession()
{
}

FDatasetSession::~FDatasetSession()
{
    if (State == ESessionState::Running)
    {
        Stop();
    }
}

/** 启动新的采集会话，创建输出目录。 */
bool FDatasetSession::Start(const FString& BaseRoot, const FString& SessionName)
{
    if (State == ESessionState::Running)
    {
        UE_LOG(LogTemp, Warning, TEXT("DatasetSession: Cannot start, session already running."));
        return false;
    }

    SessionId = GenerateSessionId();
    StartTime = FDateTime::Now();

    FString DirName = SessionName.IsEmpty()
        ? GenerateSessionDirectoryName()
        : FString::Printf(TEXT("%s_%s"), *GenerateSessionDirectoryName(), *SessionName);

    SessionDirectory = BaseRoot / DirName;

    // 创建目录
    if (!IFileManager::Get().MakeDirectory(*SessionDirectory, true))
    {
        UE_LOG(LogTemp, Error, TEXT("DatasetSession: Failed to create directory: %s"), *SessionDirectory);
        return false;
    }

    State = ESessionState::Running;
    Calibrations.Reset();
    RendererMetrics.Reset();

    UE_LOG(LogTemp, Log, TEXT("DatasetSession started: %s -> %s"), *SessionId, *SessionDirectory);
    return true;
}

/** 停止当前会话。 */
void FDatasetSession::Stop()
{
    if (State != ESessionState::Running)
    {
        return;
    }

    State = ESessionState::Stopping;

    // 写入 calibration.json
    if (Calibrations.Num() > 0)
    {
        WriteCalibrationJson();
    }

    State = ESessionState::Idle;
    UE_LOG(LogTemp, Log, TEXT("DatasetSession stopped: %s"), *SessionId);
}

/** 注册或更新逐通道相机标定参数。 */
void FDatasetSession::RegisterCalibration(const FCalibration& Calibration)
{
    for (FCalibration& Existing : Calibrations)
    {
        const bool bSameChannel = Calibration.SensorGuid.IsValid() &&
            Calibration.ChannelGuid.IsValid()
            ? Existing.SensorGuid == Calibration.SensorGuid &&
                Existing.ChannelGuid == Calibration.ChannelGuid
            : Existing.SensorGuid == Calibration.SensorGuid &&
                Existing.PayloadType == Calibration.PayloadType;
        if (bSameChannel)
        {
            // Resolution/FOV 可以热更新；只覆盖同一 Sensor + ChannelGuid，
            // 避免复制 Camera Rig 时继承的 GUID 意外覆盖另一个传感器的标定。
            Existing = Calibration;
            return;
        }
    }
    Calibrations.Add(Calibration);
}

/** 按 SensorGuid 更新 Camera Rig 的最新 Renderer 指标快照。 */
void FDatasetSession::RegisterRendererMetrics(const FCameraRendererMetricsSnapshot& Metrics)
{
    for (FCameraRendererMetricsSnapshot& Existing : RendererMetrics)
    {
        if (Existing.SensorGuid == Metrics.SensorGuid)
        {
            Existing = Metrics;
            return;
        }
    }
    RendererMetrics.Add(Metrics);
}

/** 写入 metadata.json。 */
void FDatasetSession::WriteMetadata(int64 TotalFrames, int64 CompletedFrames, int64 FailedFrames,
                                     int64 TimeoutFrames, int64 BusyFrames, int64 RejectedFrames,
                                     int64 DuplicatePayloads, int64 LatePayloads,
                                     int32 Seed, const FString& Mode)
{
    if (SessionDirectory.IsEmpty())
    {
        return;
    }

    FString JsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);

    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("session_id"), SessionId);
    Writer->WriteValue(TEXT("start_time"), StartTime.ToString());
    Writer->WriteValue(TEXT("end_time"), FDateTime::Now().ToString());
    Writer->WriteValue(TEXT("simulation_mode"), Mode);
    Writer->WriteValue(TEXT("random_seed"), Seed);

    Writer->WriteObjectStart(TEXT("statistics"));
    Writer->WriteValue(TEXT("total_frames"), TotalFrames);
    Writer->WriteValue(TEXT("completed_frames"), CompletedFrames);
    Writer->WriteValue(TEXT("failed_frames"), FailedFrames);
    Writer->WriteValue(TEXT("timeout_frames"), TimeoutFrames);
    Writer->WriteValue(TEXT("busy_frames"), BusyFrames);
    Writer->WriteValue(TEXT("rejected_frames"), RejectedFrames);
    Writer->WriteValue(TEXT("duplicate_payloads"), DuplicatePayloads);
    Writer->WriteValue(TEXT("late_payloads"), LatePayloads);
    Writer->WriteObjectEnd();

    // Renderer 指标以最终/最近快照写入会话，便于离线判断背压、资源重建和特定模态延迟。
    Writer->WriteObjectStart(TEXT("renderer"));
    Writer->WriteArrayStart(TEXT("camera_rigs"));
    for (const FCameraRendererMetricsSnapshot& Metrics : RendererMetrics)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("sensor_guid"), Metrics.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Writer->WriteValue(TEXT("sensor_name"), Metrics.SensorName.ToString());

        const FCameraRigResourceStats& Resources = Metrics.ResourceStats;
        Writer->WriteObjectStart(TEXT("resources"));
        Writer->WriteValue(TEXT("configuration_apply_count"), Resources.ConfigurationApplyCount);
        Writer->WriteValue(TEXT("configuration_change_count"), Resources.ConfigurationChangeCount);
        Writer->WriteValue(TEXT("configuration_noop_count"), Resources.NoOpConfigurationApplyCount);
        Writer->WriteValue(TEXT("created_capture_components"), Resources.CreatedCaptureComponents);
        Writer->WriteValue(TEXT("reused_capture_components"), Resources.ReusedCaptureComponents);
        Writer->WriteValue(TEXT("destroyed_capture_components"), Resources.DestroyedCaptureComponents);
        Writer->WriteValue(TEXT("created_render_targets"), Resources.CreatedRenderTargets);
        Writer->WriteValue(TEXT("reused_render_targets"), Resources.ReusedRenderTargets);
        Writer->WriteValue(TEXT("rebuilt_render_targets"), Resources.RebuiltRenderTargets);
        Writer->WriteValue(TEXT("destroyed_render_targets"), Resources.DestroyedRenderTargets);
        Writer->WriteObjectEnd();

        const FImageReadbackStats& Readback = Metrics.ReadbackStats;
        Writer->WriteObjectStart(TEXT("readback"));
        Writer->WriteValue(TEXT("capacity"), Readback.Capacity);
        Writer->WriteValue(TEXT("pending"), Readback.PendingCount);
        Writer->WriteValue(TEXT("peak_pending"), Readback.PeakPendingCount);
        Writer->WriteValue(TEXT("enqueued"), Readback.EnqueuedCount);
        Writer->WriteValue(TEXT("completed"), Readback.CompletedCount);
        Writer->WriteValue(TEXT("rejected"), Readback.RejectedCount);
        Writer->WriteValue(TEXT("failed"), Readback.FailedCount);
        Writer->WriteValue(TEXT("created_resources"), Readback.CreatedReadbackResources);
        Writer->WriteValue(TEXT("reused_resources"), Readback.ReusedReadbackResources);
        Writer->WriteObjectEnd();

        Writer->WriteArrayStart(TEXT("channels"));
        for (const FImageReadbackChannelStats& Channel : Metrics.ChannelStats)
        {
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("sensor_guid"), Channel.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Writer->WriteValue(TEXT("sensor_name"), Channel.SensorName.ToString());
            Writer->WriteValue(TEXT("channel_guid"), Channel.ChannelGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
            Writer->WriteValue(TEXT("payload_type"), PayloadTypeToString(Channel.PayloadType));
            Writer->WriteValue(TEXT("pending"), Channel.PendingCount);
            Writer->WriteValue(TEXT("peak_pending"), Channel.PeakPendingCount);
            Writer->WriteValue(TEXT("enqueued"), Channel.EnqueuedCount);
            Writer->WriteValue(TEXT("completed"), Channel.CompletedCount);
            Writer->WriteValue(TEXT("delivered"), Channel.DeliveredCount);
            Writer->WriteValue(TEXT("rejected"), Channel.RejectedCount);
            Writer->WriteValue(TEXT("failed"), Channel.FailedCount);
            Writer->WriteValue(TEXT("average_gpu_latency_ms"), Channel.AverageGpuLatencyMs);
            Writer->WriteValue(TEXT("max_gpu_latency_ms"), Channel.MaxGpuLatencyMs);
            Writer->WriteValue(TEXT("average_delivery_latency_ms"), Channel.AverageDeliveryLatencyMs);
            Writer->WriteValue(TEXT("max_delivery_latency_ms"), Channel.MaxDeliveryLatencyMs);
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
        Writer->WriteObjectEnd();
    }
    Writer->WriteArrayEnd();
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("coordinate_system"));
    Writer->WriteValue(TEXT("description"), TEXT("Right-hand vehicle frame: X Forward, Y Left, Z Up"));
    Writer->WriteValue(TEXT("unit"), TEXT("meters"));
    Writer->WriteValue(TEXT("ue_internal"), TEXT("Left-hand: X Forward, Y Right, Z Up, unit=cm"));
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("engine"));
    Writer->WriteValue(TEXT("ue_version"), FEngineVersion::Current().ToString());
    Writer->WriteValue(TEXT("plugin_version"), TEXT("0.1.0"));
    Writer->WriteObjectEnd();

    Writer->WriteObjectEnd();
    Writer->Close();

    const FString FilePath = SessionDirectory / TEXT("metadata.json");
    FFileHelper::SaveStringToFile(JsonStr, *FilePath);
}

/** 写入 calibration.json。 */
void FDatasetSession::WriteCalibrationJson() const
{
    FString JsonStr;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);

    Writer->WriteObjectStart();
    Writer->WriteArrayStart(TEXT("sensors"));

    for (const FCalibration& Cal : Calibrations)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("sensor_guid"), Cal.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Writer->WriteValue(TEXT("sensor_name"), Cal.SensorName.ToString());
        Writer->WriteValue(TEXT("channel_guid"), Cal.ChannelGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Writer->WriteValue(TEXT("payload_type"), PayloadTypeToString(Cal.PayloadType));
        Writer->WriteValue(TEXT("image_width"), Cal.ImageSize.X);
        Writer->WriteValue(TEXT("image_height"), Cal.ImageSize.Y);
        Writer->WriteValue(TEXT("fx"), Cal.Fx);
        Writer->WriteValue(TEXT("fy"), Cal.Fy);
        Writer->WriteValue(TEXT("cx"), Cal.Cx);
        Writer->WriteValue(TEXT("cy"), Cal.Cy);

        const FVector T = Cal.SensorToEgo.GetLocation();
        Writer->WriteValue(TEXT("sensor_to_ego_tx"), T.X);
        Writer->WriteValue(TEXT("sensor_to_ego_ty"), T.Y);
        Writer->WriteValue(TEXT("sensor_to_ego_tz"), T.Z);

        const FQuat Q = Cal.SensorToEgo.GetRotation();
        Writer->WriteValue(TEXT("sensor_to_ego_qw"), Q.W);
        Writer->WriteValue(TEXT("sensor_to_ego_qx"), Q.X);
        Writer->WriteValue(TEXT("sensor_to_ego_qy"), Q.Y);
        Writer->WriteValue(TEXT("sensor_to_ego_qz"), Q.Z);

        Writer->WriteObjectEnd();
    }

    Writer->WriteArrayEnd();
    Writer->WriteObjectEnd();
    Writer->Close();

    const FString FilePath = SessionDirectory / TEXT("calibration.json");
    FFileHelper::SaveStringToFile(JsonStr, *FilePath);
}

/** 生成唯一的会话标识符。 */
FString FDatasetSession::GenerateSessionId()
{
    return FGuid::NewGuid().ToString(EGuidFormats::Short);
}

/** 生成带时间戳的目录名。 */
FString FDatasetSession::GenerateSessionDirectoryName()
{
    const FDateTime Now = FDateTime::Now();
    return FString::Printf(TEXT("%04d%02d%02d_%02d%02d%02d"),
        Now.GetYear(), Now.GetMonth(), Now.GetDay(),
        Now.GetHour(), Now.GetMinute(), Now.GetSecond());
}
