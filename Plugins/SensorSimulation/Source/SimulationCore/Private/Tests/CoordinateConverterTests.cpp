#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CoordinateConverter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCoordinatePositionTest,
    "SensorSimulation.Core.Coordinates.UnrealToFLU",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 验证 Unreal 到 FLU 坐标转换的轴方向和单位缩放。 */
bool FCoordinatePositionTest::RunTest(const FString& Parameters)
{
    const FVector3d Result = FCoordinateConverter::UnrealCentimetersToFrontLeftUpMeters(FVector(100.0, 200.0, 300.0));
    TestEqual(TEXT("X forward"), Result.X, 1.0);
    TestEqual(TEXT("Y flips right to left"), Result.Y, -2.0);
    TestEqual(TEXT("Z up"), Result.Z, 3.0);
    return true;
}

#endif
