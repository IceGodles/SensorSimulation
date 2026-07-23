#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"

/** 数据集采集会话的运行时状态。 */
enum class ESessionState : uint8
{
    /** 未启动。 */
    Idle,
    /** 正在采集数据。 */
    Running,
    /** 已停止，正在写入元数据。 */
    Stopping
};

/** 管理一次数据集采集会话的生命周期、输出目录和元数据。 */
class SIMULATIONRUNTIME_API FDatasetSession
{
public:
    FDatasetSession();
    ~FDatasetSession();

/** 启动新的采集会话，创建输出目录。返回是否成功。 */
    bool Start(const FString& BaseRoot, const FString& SessionName = TEXT(""));
/** 停止当前会话，写入 metadata.json 和 calibration.json。 */
    void Stop();

/** 注册一帧的相机标定参数，会话结束时写入 calibration.json。 */
    void RegisterCalibration(const FCalibration& Calibration);

/** 返回当前会话的输出目录路径。 */
    FString GetSessionDirectory() const { return SessionDirectory; }
/** 返回当前会话状态。 */
    ESessionState GetState() const { return State; }
/** 返回当前会话的唯一标识。 */
    FString GetSessionId() const { return SessionId; }

/** 写入 metadata.json，包含会话配置和统计信息。 */
    void WriteMetadata(int64 TotalFrames, int64 CompletedFrames, int64 FailedFrames,
                       int32 Seed, const FString& Mode);

private:
    /** Write registered camera calibration parameters to calibration.json. */
    void WriteCalibrationJson() const;
/** 生成唯一的会话标识符。 */
    static FString GenerateSessionId();
/** 生成带时间戳的目录名。 */
    static FString GenerateSessionDirectoryName();

    ESessionState State = ESessionState::Idle;
    FString SessionId;
    FString SessionDirectory;
    FDateTime StartTime;

/** 已注册的相机标定参数。 */
    TArray<FCalibration> Calibrations;
};
