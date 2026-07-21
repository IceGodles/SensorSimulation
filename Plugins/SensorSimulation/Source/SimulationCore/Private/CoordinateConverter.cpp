#include "CoordinateConverter.h"

/** 把 Unreal 的前-右-上厘米坐标转换为前-左-上米坐标。 */
FVector3d FCoordinateConverter::UnrealCentimetersToFrontLeftUpMeters(const FVector& UnrealPositionCm)
{
    return FVector3d(UnrealPositionCm.X, -UnrealPositionCm.Y, UnrealPositionCm.Z) / 100.0;
}

/** 把前-左-上米坐标转换回 Unreal 厘米坐标。 */
FVector FCoordinateConverter::FrontLeftUpMetersToUnrealCentimeters(const FVector3d& PositionMeters)
{
    return FVector(PositionMeters.X, -PositionMeters.Y, PositionMeters.Z) * 100.0;
}

/** 通过坐标轴反射转换旋转的坐标系约定。 */
FQuat FCoordinateConverter::UnrealToFrontLeftUpRotation(const FQuat& UnrealRotation)
{
    // 坐标系变换是一次 Y 轴镜像。旋转矩阵需左右各乘一次反射矩阵，
    // 才能同时转换旋转的输入基和输出基，并避免直接修改四元数分量造成符号错误。
    const FMatrix Reflection(
        FPlane(1.0, 0.0, 0.0, 0.0),
        FPlane(0.0, -1.0, 0.0, 0.0),
        FPlane(0.0, 0.0, 1.0, 0.0),
        FPlane(0.0, 0.0, 0.0, 1.0));
    const FMatrix Converted = Reflection * FQuatRotationMatrix(UnrealRotation) * Reflection;
    return FQuat(Converted).GetNormalized();
}

/** 转换位姿中的平移和旋转并保留缩放。 */
FTransform FCoordinateConverter::UnrealToFrontLeftUpTransform(const FTransform& UnrealTransform)
{
    return FTransform(
        UnrealToFrontLeftUpRotation(UnrealTransform.GetRotation()),
        FVector(UnrealCentimetersToFrontLeftUpMeters(UnrealTransform.GetLocation())),
        UnrealTransform.GetScale3D());
}

/** 把 Unreal 相机局部点转换为 OpenCV 的右-下-前米坐标。 */
FVector3d FCoordinateConverter::UnrealCameraPointToOpenCV(const FVector& UnrealCameraPointCm)
{
    return FVector3d(UnrealCameraPointCm.Y, -UnrealCameraPointCm.Z, UnrealCameraPointCm.X) / 100.0;
}
