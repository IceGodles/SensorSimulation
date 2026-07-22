#include "DatasetSession.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformMisc.h"

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

/** 注册一帧的相机标定参数。 */
void FDatasetSession::RegisterCalibration(const FCalibration& Calibration)
{
    // 避免重复注册同名传感器
    for (const FCalibration& Existing : Calibrations)
    {
        if (Existing.SensorName == Calibration.SensorName)
        {
            return;
        }
    }
    Calibrations.Add(Calibration);
}

/** 写入 metadata.json。 */
void FDatasetSession::WriteMetadata(int64 TotalFrames, int64 CompletedFrames, int64 FailedFrames,
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
        Writer->WriteValue(TEXT("sensor_name"), Cal.SensorName.ToString());
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
