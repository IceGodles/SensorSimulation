#include "SemanticImageLabel.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSemanticImageLabelRangeTest,
    "SensorSimulation.Runtime.Semantic.ImageLabelRange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSemanticImageLabelRangeTest::RunTest(const FString& Parameters)
{
    uint8 ImageId = 99;
    TestFalse(TEXT("0 is reserved for background"), UE::SensorSimulation::SemanticLabels::TryConvertToImageId(0, ImageId));
    TestEqual(TEXT("Invalid ID maps to background only as an error sentinel"), ImageId, static_cast<uint8>(0));
    TestTrue(TEXT("1 is the first valid object label"), UE::SensorSimulation::SemanticLabels::TryConvertToImageId(1, ImageId));
    TestEqual(TEXT("ID 1 is preserved"), ImageId, static_cast<uint8>(1));
    TestTrue(TEXT("255 is the last valid image label"), UE::SensorSimulation::SemanticLabels::TryConvertToImageId(255, ImageId));
    TestEqual(TEXT("ID 255 is preserved"), ImageId, static_cast<uint8>(255));
    TestFalse(TEXT("256 is rejected instead of clamped"), UE::SensorSimulation::SemanticLabels::TryConvertToImageId(256, ImageId));
    return true;
}

#endif
