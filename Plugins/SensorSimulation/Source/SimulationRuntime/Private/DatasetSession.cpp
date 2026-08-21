#include "DatasetSession.h"
#include "FrameAssembler.h"
#include "ExportService.h"
#include "CoordinateConverter.h"
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

    // 秒级时间戳不足以区分同一进程内并发 World、快速 PIE 重启或自动化测试。
    // SessionId 让每次会话拥有独立事务命名空间，避免两个 Export Worker 写入同一 frame_*.tmp。
    DirName += FString::Printf(TEXT("_%s"), *SessionId);

    SessionDirectory = BaseRoot / DirName;

    // 创建目录
    if (!IFileManager::Get().MakeDirectory(*SessionDirectory, true))
    {
        UE_LOG(LogTemp, Error, TEXT("DatasetSession: Failed to create directory: %s"), *SessionDirectory);
        return false;
    }

    State = ESessionState::Running;
    bMetadataWritten = false;
    bConsistencyPassed = false;
    Calibrations.Reset();
    LidarCalibrations.Reset();
    RendererMetrics.Reset();

    UE_LOG(LogTemp, Log, TEXT("DatasetSession started: %s -> %s"), *SessionId, *SessionDirectory);
    return true;
}

/** 停止当前会话。 */
bool FDatasetSession::Stop()
{
    if (State != ESessionState::Running)
    {
        return State == ESessionState::Idle;
    }

    State = ESessionState::Stopping;

    // 写入 calibration.json
    bool bSuccess = true;
    if (Calibrations.Num() > 0 || LidarCalibrations.Num() > 0)
    {
        bSuccess = WriteCalibrationJson();
    }

    // COMPLETED 是会话唯一的可消费标志；metadata 或 calibration 任一失败都不得生成。
    bSuccess = bSuccess && bMetadataWritten && bConsistencyPassed;
    if (bSuccess)
    {
        bSuccess = WriteTextFileAtomically(
            SessionDirectory / TEXT("COMPLETED"),
            FString::Printf(TEXT("session_id=%s\n"), *SessionId));
    }

    State = ESessionState::Idle;
    if (bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("DatasetSession stopped: %s finalized=1"), *SessionId);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("DatasetSession stopped: %s finalized=0"), *SessionId);
    }
    return bSuccess;
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

void FDatasetSession::RegisterLidarCalibration(const FLidarCalibration& Calibration)
{
    for (FLidarCalibration& Existing : LidarCalibrations)
    {
        if (Existing.SensorGuid == Calibration.SensorGuid)
        {
            Existing = Calibration;
            return;
        }
    }
    LidarCalibrations.Add(Calibration);
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
bool FDatasetSession::WriteMetadata(
    const FFrameAssemblerStats& FrameStats,
    const FExportServiceStats& ExportStats,
    const int32 Seed,
    const FString& Mode)
{
    if (SessionDirectory.IsEmpty())
    {
        return false;
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
    Writer->WriteValue(TEXT("total_frames"), FrameStats.TotalFrames);
    Writer->WriteValue(TEXT("completed_frames"), FrameStats.CompletedFrames);
    Writer->WriteValue(TEXT("failed_frames"), FrameStats.FailedFrames);
    Writer->WriteValue(TEXT("timeout_frames"), FrameStats.TimeoutFrames);
    Writer->WriteValue(TEXT("busy_frames"), FrameStats.BusyFrames);
    Writer->WriteValue(TEXT("rejected_frames"), FrameStats.RejectedFrames);
    Writer->WriteValue(TEXT("capacity_rejected_frames"), FrameStats.CapacityRejectedFrames);
    Writer->WriteValue(TEXT("cancelled_frames"), FrameStats.CancelledFrames);
    Writer->WriteValue(TEXT("duplicate_payloads"), FrameStats.DuplicatePayloads);
    Writer->WriteValue(TEXT("late_payloads"), FrameStats.LatePayloads);
    Writer->WriteValue(TEXT("frame_assembler_peak_pending"), FrameStats.PeakPendingFrames);
    Writer->WriteValue(TEXT("export_enqueued_frames"), ExportStats.EnqueuedFrames);
    Writer->WriteValue(TEXT("export_rejected_frames"), ExportStats.RejectedFrames);
    Writer->WriteValue(TEXT("export_dropped_frames"), ExportStats.DroppedFrames);
    Writer->WriteValue(TEXT("export_committed_frames"), ExportStats.CommittedFrames);
    Writer->WriteValue(TEXT("export_failed_frames"), ExportStats.FailedFrames);
    Writer->WriteValue(TEXT("export_peak_pending"), ExportStats.PeakPendingFrames);
    Writer->WriteObjectEnd();

    const bool bAssemblerConserved = FrameStats.TotalFrames ==
        FrameStats.CompletedFrames + FrameStats.FailedFrames + FrameStats.TimeoutFrames;
    const bool bExportConserved = ExportStats.EnqueuedFrames ==
        ExportStats.CommittedFrames + ExportStats.FailedFrames + ExportStats.DroppedFrames;
    const bool bPipelineConserved = FrameStats.CompletedFrames == ExportStats.EnqueuedFrames;
    bConsistencyPassed = bAssemblerConserved && bExportConserved && bPipelineConserved;
    Writer->WriteObjectStart(TEXT("consistency"));
    Writer->WriteValue(TEXT("assembler_terminal_counts_conserved"), bAssemblerConserved);
    Writer->WriteValue(TEXT("export_terminal_counts_conserved"), bExportConserved);
    Writer->WriteValue(TEXT("assembled_equals_export_enqueued"), bPipelineConserved);
    Writer->WriteValue(TEXT("passed"), bConsistencyPassed);
    Writer->WriteObjectEnd();

    Writer->WriteObjectStart(TEXT("lidar_schema"));
    Writer->WriteValue(TEXT("byte_order"), TEXT("little_endian"));
    Writer->WriteValue(TEXT("coordinate_frame"), TEXT("sensor_flu"));
    Writer->WriteValue(TEXT("unit"), TEXT("meters"));
    Writer->WriteValue(TEXT("basic_version"), 1);
    Writer->WriteValue(TEXT("basic_point_stride_bytes"), 16);
    Writer->WriteValue(TEXT("basic_fields"), TEXT("float32 x,y,z,intensity"));
    Writer->WriteValue(TEXT("extended_version"), 2);
    Writer->WriteValue(TEXT("extended_header_bytes"), 32);
    Writer->WriteValue(TEXT("extended_point_stride_bytes"), 28);
    Writer->WriteValue(TEXT("extended_fields"),
        TEXT("float32 x,y,z,intensity; uint16 semantic_id; uint16 reserved; uint32 instance_id; float32 relative_time_seconds"));
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

    Writer->WriteArrayStart(TEXT("lidars"));
    for (const FLidarCalibration& Cal : LidarCalibrations)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("sensor_guid"), Cal.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Writer->WriteValue(TEXT("sensor_name"), Cal.SensorName.ToString());
        Writer->WriteValue(TEXT("channels"), Cal.Channels);
        Writer->WriteValue(TEXT("horizontal_samples"), Cal.HorizontalSamples);
        Writer->WriteValue(TEXT("vertical_fov_upper_degrees"), Cal.VerticalFovUpperDegrees);
        Writer->WriteValue(TEXT("vertical_fov_lower_degrees"), Cal.VerticalFovLowerDegrees);
        Writer->WriteValue(TEXT("min_range_meters"), Cal.MinRangeMeters);
        Writer->WriteValue(TEXT("max_range_meters"), Cal.MaxRangeMeters);
        Writer->WriteValue(TEXT("update_frequency_hz"), Cal.UpdateFrequencyHz);
        Writer->WriteValue(TEXT("rays_per_tick"), Cal.RaysPerTick);

        const FTransform FluTransform = FCoordinateConverter::UnrealToFrontLeftUpTransform(Cal.SensorToEgo);
        const FVector T = FluTransform.GetLocation();
        Writer->WriteValue(TEXT("sensor_to_ego_tx"), T.X);
        Writer->WriteValue(TEXT("sensor_to_ego_ty"), T.Y);
        Writer->WriteValue(TEXT("sensor_to_ego_tz"), T.Z);
        const FQuat Q = FluTransform.GetRotation();
        Writer->WriteValue(TEXT("sensor_to_ego_qw"), Q.W);
        Writer->WriteValue(TEXT("sensor_to_ego_qx"), Q.X);
        Writer->WriteValue(TEXT("sensor_to_ego_qy"), Q.Y);
        Writer->WriteValue(TEXT("sensor_to_ego_qz"), Q.Z);
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
    bMetadataWritten = WriteTextFileAtomically(FilePath, JsonStr);
    return bMetadataWritten;
}

/** 写入 calibration.json。 */
bool FDatasetSession::WriteCalibrationJson() const
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

    Writer->WriteArrayStart(TEXT("lidars"));
    for (const FLidarCalibration& Cal : LidarCalibrations)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("sensor_guid"), Cal.SensorGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        Writer->WriteValue(TEXT("sensor_name"), Cal.SensorName.ToString());
        Writer->WriteValue(TEXT("channels"), Cal.Channels);
        Writer->WriteValue(TEXT("horizontal_samples"), Cal.HorizontalSamples);
        Writer->WriteValue(TEXT("vertical_fov_upper_degrees"), Cal.VerticalFovUpperDegrees);
        Writer->WriteValue(TEXT("vertical_fov_lower_degrees"), Cal.VerticalFovLowerDegrees);
        Writer->WriteValue(TEXT("min_range_meters"), Cal.MinRangeMeters);
        Writer->WriteValue(TEXT("max_range_meters"), Cal.MaxRangeMeters);
        Writer->WriteValue(TEXT("update_frequency_hz"), Cal.UpdateFrequencyHz);
        Writer->WriteValue(TEXT("rays_per_tick"), Cal.RaysPerTick);
        const FTransform FluTransform = FCoordinateConverter::UnrealToFrontLeftUpTransform(Cal.SensorToEgo);
        const FVector T = FluTransform.GetLocation();
        const FQuat Q = FluTransform.GetRotation();
        Writer->WriteValue(TEXT("sensor_to_ego_tx"), T.X);
        Writer->WriteValue(TEXT("sensor_to_ego_ty"), T.Y);
        Writer->WriteValue(TEXT("sensor_to_ego_tz"), T.Z);
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
    return WriteTextFileAtomically(FilePath, JsonStr);
}

bool FDatasetSession::WriteTextFileAtomically(const FString& FinalPath, const FString& Contents)
{
    const FString TempPath = FinalPath + TEXT(".tmp");
    IFileManager::Get().Delete(*TempPath, false, true, true);
    if (!FFileHelper::SaveStringToFile(Contents, *TempPath))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to write session temporary file: %s"), *TempPath);
        return false;
    }
    if (!IFileManager::Get().Move(*FinalPath, *TempPath, true, true, false, true))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to atomically commit session file: %s"), *FinalPath);
        return false;
    }
    return true;
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
