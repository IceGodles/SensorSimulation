#pragma once

#include "CoreMinimal.h"
#include "SimulationTypes.h"
#include "CameraRigComponent.h"
#include "SemanticTaxonomy.h"

struct FFrameAssemblerStats;
struct FExportServiceStats;

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

/** 单台 Camera Rig 写入会话 metadata 的 Renderer 指标快照。 */
struct SIMULATIONRUNTIME_API FCameraRendererMetricsSnapshot
{
    /** Camera Adapter 的持久身份，是指标 Upsert 的唯一键。 */
    FGuid SensorGuid;
    /** Camera Rig 的人类可读显示名称。 */
    FName SensorName = NAME_None;
    /** Capture/RenderTarget 创建、复用和重建计数。 */
    FCameraRigResourceStats ResourceStats;
    /** 该 Rig 的 Readback 汇总容量与结果计数。 */
    FImageReadbackStats ReadbackStats;
    /** 按 SensorGuid + ChannelGuid 细分的延迟与背压指标。 */
    TArray<FImageReadbackChannelStats> ChannelStats;
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
    bool Stop();

    /** 注册或更新单个相机通道标定，会话结束时按 ChannelGuid 分别写入 calibration.json。 */
    void RegisterCalibration(const FCalibration& Calibration);
    /** 注册或更新一台 LiDAR 的外参与扫描配置。 */
    void RegisterLidarCalibration(const FLidarCalibration& Calibration);
    /** 注册或更新 Camera Rig 的 Renderer 指标；会话结束时写入 metadata.json。 */
    void RegisterRendererMetrics(const FCameraRendererMetricsSnapshot& Metrics);
    void RegisterSemanticTaxonomy(const USemanticTaxonomy& Taxonomy);

    /** 返回当前会话的输出目录路径。 */
    FString GetSessionDirectory() const { return SessionDirectory; }
    /** 返回当前会话状态。 */
    ESessionState GetState() const { return State; }
    /** 返回当前会话的唯一标识。 */
    FString GetSessionId() const { return SessionId; }

    /** 写入 metadata.json，包含会话配置、帧终态和异常 Payload 统计。 */
    bool WriteMetadata(const FFrameAssemblerStats& FrameStats,
                       const FExportServiceStats& ExportStats,
                       int32 Seed, const FString& Mode);

private:
    /** 将已注册的逐通道相机标定写入 calibration.json。 */
    bool WriteCalibrationJson() const;
    /** 根据最终 frame 目录生成可重建、稳定排序的 committed frame 索引。 */
    bool WriteManifestJsonLines() const;
    /** 先写同目录临时文件，再原子替换最终文件。 */
    static bool WriteTextFileAtomically(const FString& FinalPath, const FString& Contents);
    /** 生成唯一的会话标识符。 */
    static FString GenerateSessionId();
    /** 生成带时间戳的目录名。 */
    static FString GenerateSessionDirectoryName();

    ESessionState State = ESessionState::Idle;
    FString SessionId;
    FString SessionDirectory;
    FDateTime StartTime;
    bool bMetadataWritten = false;
    bool bConsistencyPassed = false;

    /** 已注册的逐通道相机标定参数。 */
    TArray<FCalibration> Calibrations;
    /** 按 SensorGuid 保存的 LiDAR 标定和扫描参数。 */
    TArray<FLidarCalibration> LidarCalibrations;
    /** 每台 Camera Rig 的最新 Renderer 指标快照。 */
    TArray<FCameraRendererMetricsSnapshot> RendererMetrics;
    FString SemanticTaxonomyAsset;
    FString SemanticTaxonomyVersion;
    TArray<FSemanticTaxonomyEntry> SemanticTaxonomyEntries;
};
