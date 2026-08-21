#include "LidarSerialization.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLidarExtendedV2SerializationTest,
    "SensorSimulation.Core.Protocol.LidarExtendedV2",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLidarExtendedV2SerializationTest::RunTest(const FString& Parameters)
{
    FLidarScanPayload Scan;
    FLidarPoint& Point = Scan.Points.AddDefaulted_GetRef();
    Point.PositionMeters = FVector3f(1.0f, -2.0f, 3.0f);
    Point.Intensity = 0.75f;
    Point.SemanticId = 42;
    Point.InstanceId = 0x01020304u;
    Point.RelativeTimeSeconds = 0.025f;
    TArray<uint8> Bytes;
    UE::SensorSimulation::LidarFormat::SerializeExtendedV2(Scan, Bytes);
    TestEqual(TEXT("One v2 point has a 32-byte header plus 28-byte record"), Bytes.Num(), 60);
    uint32 PointCount = 0;
    TestTrue(TEXT("Core validates its stable v2 wire format"),
        UE::SensorSimulation::LidarFormat::ValidateExtendedV2(Bytes, PointCount));
    TestEqual(TEXT("Header preserves point count"), PointCount, uint32{1});
    Bytes[0] ^= 0xff;
    TestFalse(TEXT("Corrupt magic is rejected"),
        UE::SensorSimulation::LidarFormat::ValidateExtendedV2(Bytes, PointCount));
    return true;
}
#endif
