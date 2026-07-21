#pragma once

#include "CoreMinimal.h"

/** Unreal、FLU 与 OpenCV 坐标约定之间的无状态转换器。 */
class SIMULATIONCORE_API FCoordinateConverter
{
public:
/** 把 Unreal 的前-右-上厘米坐标转换为前-左-上米坐标。 */
    static FVector3d UnrealCentimetersToFrontLeftUpMeters(const FVector& UnrealPositionCm);
/** 把前-左-上米坐标转换回 Unreal 厘米坐标。 */
    static FVector FrontLeftUpMetersToUnrealCentimeters(const FVector3d& PositionMeters);
/** 通过坐标轴反射转换旋转的坐标系约定。 */
    static FQuat UnrealToFrontLeftUpRotation(const FQuat& UnrealRotation);
/** 转换位姿中的平移和旋转并保留缩放。 */
    static FTransform UnrealToFrontLeftUpTransform(const FTransform& UnrealTransform);
/** 把 Unreal 相机局部点转换为 OpenCV 的右-下-前米坐标。 */
    static FVector3d UnrealCameraPointToOpenCV(const FVector& UnrealCameraPointCm);
};
