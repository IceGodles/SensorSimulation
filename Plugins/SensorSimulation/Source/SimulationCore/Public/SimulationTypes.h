#pragma once

#include "CoreMinimal.h"

/** 一帧传感器数据中可组合的数据模态位掩码。 */
enum class EPayloadType : uint8
{
    None = 0,
    Rgb = 1 << 0,
    Semantic = 1 << 1,
    Depth = 1 << 2,
    Instance = 1 << 3,
    Lidar = 1 << 4,
    GroundTruth = 1 << 5
};

ENUM_CLASS_FLAGS(EPayloadType);

/** 传感器仿真的时钟推进模式。 */
enum class ESimulationMode : uint8
{
    Realtime,
    DeterministicDataset
};

/** 数据位置或姿态采用的坐标系约定。 */
enum class ECoordinateFrame : uint8
{
    UnrealWorld,
    EgoFrontLeftUp,
    SensorFrontLeftUp,
    OpenCVCamera
};

/** 一次同步采集的公共帧标识与时间信息。 */
struct FFrameHeader
{
    /** 当前仿真或数据集采集会话的序列编号。 */
    uint64 SequenceId = 0;
    /** 序列内单调递增的同步帧编号。 */
    uint64 FrameId = 0;
    /** 采集发生时的仿真时间戳，单位为秒。 */
    double SimulationTimestampSeconds = 0.0;
    /** 采集时刻自车相对于 Unreal 世界的位姿。 */
    FTransform EgoWorldTransform = FTransform::Identity;
};

/** 子系统发送给具体传感器的采集命令。 */
struct FCaptureRequest
{
    /** 所属同步帧的公共标识、时间戳和自车位姿。 */
    FFrameHeader Header;
    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    FName SensorName = NAME_None;
    /** 该请求或帧在发布前必须产生的数据模态位集合。 */
    EPayloadType ExpectedPayloads = EPayloadType::None;
    /** 传感器坐标系相对于自车坐标系的外参变换。 */
    FTransform SensorToEgo = FTransform::Identity;
};

/** 相机传感器的外参与针孔模型内参。 */
struct FCalibration
{
    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    FName SensorName = NAME_None;
    /** 传感器坐标系相对于自车坐标系的外参变换。 */
    FTransform SensorToEgo = FTransform::Identity;
    /** 输出图像的宽度和高度，单位为像素。 */
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    /** 针孔相机模型的水平焦距，单位为像素。 */
    double Fx = 0.0;
    /** 针孔相机模型的垂直焦距，单位为像素。 */
    double Fy = 0.0;
    /** 相机主点的水平像素坐标。 */
    double Cx = 0.0;
    /** 相机主点的垂直像素坐标。 */
    double Cy = 0.0;
};

/** 从渲染目标读回的 CPU 图像数据。 */
struct FImagePayload
{
    /** 所属同步帧的公共标识、时间戳和自车位姿。 */
    FFrameHeader Header;
    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    FName SensorName = NAME_None;
    /** 当前图像或读回任务对应的数据模态。 */
    EPayloadType PayloadType = EPayloadType::None;
    /** 输出图像的宽度和高度，单位为像素。 */
    FIntPoint ImageSize = FIntPoint::ZeroValue;
    /** 每个像素在原始缓冲区中占用的字节数。 */
    int32 BytesPerPixel = 0;
    /** 按行连续存储的 CPU 端原始像素字节。 */
    TArray<uint8> Bytes;
};

/** 单个激光回波点及其语义标签。 */
struct FLidarPoint
{
    /** 回波点在传感器局部坐标系中的位置，单位为米。 */
    FVector3f PositionMeters = FVector3f::ZeroVector;
    /** 根据射线入射角估算的归一化回波强度。 */
    float Intensity = 0.0f;
    /** 对象或回波点所属的语义类别编号。 */
    uint16 SemanticId = 0;
    /** 对象在当前仿真会话中唯一的实例编号。 */
    uint32 InstanceId = 0;
    /** 此射线相对整次扫描起点的时间偏移，单位为秒。 */
    float RelativeTimeSeconds = 0.0f;
};

/** 一次激光雷达扫描的点云与进度元数据。 */
struct FLidarScanPayload
{
    /** 所属同步帧的公共标识、时间戳和自车位姿。 */
    FFrameHeader Header;
    /** 传感器的稳定名称，用于区分同一帧内的多个数据源。 */
    FName SensorName = NAME_None;
    /** 传感器坐标系相对于自车坐标系的外参变换。 */
    FTransform SensorToEgo = FTransform::Identity;
    /** 完整扫描计划执行的射线总数。 */
    uint32 ExpectedRayCount = 0;
    /** 当前已经执行的射线数，包括未命中的射线。 */
    uint32 CompletedRayCount = 0;
    /** 标识扫描是否已执行全部计划射线。 */
    bool bCompleteRevolution = false;
    /** 本次扫描中产生有效碰撞回波的点集合。 */
    TArray<FLidarPoint> Points;
};

/** 带语义对象在采集时刻的真值状态。 */
struct FObjectGroundTruth
{
    /** 对象在当前仿真会话中唯一的实例编号。 */
    uint32 InstanceId = 0;
    /** 对象或回波点所属的语义类别编号。 */
    uint16 SemanticId = 0;
    /** 对象在采集时刻相对于 Unreal 世界的位姿。 */
    FTransform WorldTransform = FTransform::Identity;
    /** 对象在世界坐标系中的线速度。 */
    FVector3d LinearVelocity = FVector3d::ZeroVector;
    /** 对象在世界坐标系中的角速度。 */
    FVector3d AngularVelocity = FVector3d::ZeroVector;
    /** 对象所有组件在世界坐标系中的轴对齐包围盒。 */
    FBox3d WorldBounds = FBox3d(EForceInit::ForceInit);
};

/** 聚合同一帧所有传感器模态的最终数据包。 */
struct FFramePacket
{
    /** 所属同步帧的公共标识、时间戳和自车位姿。 */
    FFrameHeader Header;
    /** 该请求或帧在发布前必须产生的数据模态位集合。 */
    EPayloadType ExpectedPayloads = EPayloadType::None;
    /** 当前已经到达帧聚合器的数据模态位集合。 */
    EPayloadType CompletedPayloads = EPayloadType::None;
    /** 同一同步帧内由各相机通道生成的图像集合。 */
    TArray<FImagePayload> Images;
    /** 同一同步帧内由各激光雷达生成的点云集合。 */
    TArray<FLidarScanPayload> LidarScans;
    /** 采集时刻场景内语义对象的真值集合。 */
    TArray<FObjectGroundTruth> Objects;

/** 判断已完成模态是否覆盖该帧要求的全部模态。 */
    bool IsComplete() const
    {
        return EnumHasAllFlags(CompletedPayloads, ExpectedPayloads);
    }
};

