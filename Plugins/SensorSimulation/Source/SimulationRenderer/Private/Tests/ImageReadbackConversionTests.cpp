#include "ImageReadbackConversion.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackRgbaPitchTest,
    "SensorSimulation.Renderer.Readback.Rgba8PaddedRowPitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackRgbaPitchTest::RunTest(const FString& Parameters)
{
    // 两个有效像素后故意放一个 padding 像素，验证转换不会把下一行读偏。
    const TArray<uint8> Source = {
        1, 2, 3, 4, 5, 6, 7, 8, 201, 202, 203, 204,
        9, 10, 11, 12, 13, 14, 15, 16, 205, 206, 207, 208
    };
    const TArray<uint8> Expected = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16
    };

    TArray<uint8> Actual;
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyRgba8ToCanonical(
        Source.GetData(),
        3,
        PF_R8G8B8A8,
        FIntPoint(2, 2),
        Actual);

    TestTrue(TEXT("RGBA padded-row conversion succeeds"), bCopied);
    TestEqual(TEXT("RGBA output byte count"), Actual.Num(), Expected.Num());
    TestTrue(TEXT("RGBA padding is removed without shifting rows"), Actual == Expected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackBgraSwizzleTest,
    "SensorSimulation.Renderer.Readback.Bgra8ToCanonicalRgba8",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackBgraSwizzleTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Source = {
        30, 20, 10, 40,
        70, 60, 50, 80
    };
    const TArray<uint8> Expected = {
        10, 20, 30, 40,
        50, 60, 70, 80
    };

    TArray<uint8> Actual;
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyRgba8ToCanonical(
        Source.GetData(),
        2,
        PF_B8G8R8A8,
        FIntPoint(2, 1),
        Actual);

    TestTrue(TEXT("BGRA conversion succeeds"), bCopied);
    TestTrue(TEXT("Red and blue channels are normalized to RGBA"), Actual == Expected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackRejectsShortPitchTest,
    "SensorSimulation.Renderer.Readback.RejectsShortRowPitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackRejectsShortPitchTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Source = { 1, 2, 3, 4 };
    TArray<uint8> Actual = { 99 };
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyRgba8ToCanonical(
        Source.GetData(),
        1,
        PF_R8G8B8A8,
        FIntPoint(2, 1),
        Actual);

    TestFalse(TEXT("A pitch shorter than the image width is rejected"), bCopied);
    TestTrue(TEXT("Failed conversion clears stale output"), Actual.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackDepthMetersTest,
    "SensorSimulation.Renderer.Readback.DepthR32FloatPaddedPitchToMeters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackDepthMetersTest::RunTest(const FString& Parameters)
{
    // 每行第三个 float 是 padding；有效输入 100/250/1000/50 cm 应输出 1/2.5/10/0.5 m。
    const TArray<float> SourceFloats = {
        100.0f, 250.0f, -12345.0f,
        1000.0f, 50.0f, -12345.0f
    };

    TArray<uint8> ActualBytes;
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyDepthR32FloatToMeters(
        reinterpret_cast<const uint8*>(SourceFloats.GetData()),
        3,
        PF_R32_FLOAT,
        FIntPoint(2, 2),
        ActualBytes);

    TestTrue(TEXT("R32F depth conversion succeeds"), bCopied);
    TestEqual(TEXT("Depth output is tightly packed"), ActualBytes.Num(), 4 * static_cast<int32>(sizeof(float)));

    TArray<float> Actual;
    Actual.SetNumUninitialized(4);
    if (ActualBytes.Num() == Actual.Num() * static_cast<int32>(sizeof(float)))
    {
        FMemory::Memcpy(Actual.GetData(), ActualBytes.GetData(), ActualBytes.Num());
        TestEqual(TEXT("100 cm becomes 1 m"), Actual[0], 1.0f);
        TestEqual(TEXT("250 cm becomes 2.5 m"), Actual[1], 2.5f);
        TestEqual(TEXT("1000 cm becomes 10 m"), Actual[2], 10.0f);
        TestEqual(TEXT("50 cm becomes 0.5 m"), Actual[3], 0.5f);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackDepthRgba32FloatTest,
    "SensorSimulation.Renderer.Readback.DepthRgba32FloatExtractsRedToMeters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackDepthRgba32FloatTest::RunTest(const FString& Parameters)
{
    // 每个 texel 是 RGBA 四个 float；第三个 texel 是 RowPitch padding，且 G/B/A 故意填入干扰值。
    const TArray<float> SourceFloats = {
        100.0f, -1.0f, -2.0f, -3.0f,
        250.0f, -4.0f, -5.0f, -6.0f,
        -12345.0f, -7.0f, -8.0f, -9.0f,
        1000.0f, -10.0f, -11.0f, -12.0f,
        50.0f, -13.0f, -14.0f, -15.0f,
        -12345.0f, -16.0f, -17.0f, -18.0f
    };

    TArray<uint8> ActualBytes;
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyDepthR32FloatToMeters(
        reinterpret_cast<const uint8*>(SourceFloats.GetData()),
        3,
        PF_A32B32G32R32F,
        FIntPoint(2, 2),
        ActualBytes);

    TestTrue(TEXT("RGBA32F depth conversion succeeds"), bCopied);
    TestEqual(
        TEXT("RGBA32F depth is compressed to tightly packed R32F"),
        ActualBytes.Num(),
        4 * static_cast<int32>(sizeof(float)));

    TArray<float> Actual;
    Actual.SetNumUninitialized(4);
    if (ActualBytes.Num() == Actual.Num() * static_cast<int32>(sizeof(float)))
    {
        FMemory::Memcpy(Actual.GetData(), ActualBytes.GetData(), ActualBytes.Num());
        TestEqual(TEXT("RGBA red 100 cm becomes 1 m"), Actual[0], 1.0f);
        TestEqual(TEXT("RGBA red 250 cm becomes 2.5 m"), Actual[1], 2.5f);
        TestEqual(TEXT("RGBA red 1000 cm becomes 10 m"), Actual[2], 10.0f);
        TestEqual(TEXT("RGBA red 50 cm becomes 0.5 m"), Actual[3], 0.5f);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackInstanceUintTest,
    "SensorSimulation.Renderer.Readback.InstanceR32UintPaddedPitch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackInstanceUintTest::RunTest(const FString& Parameters)
{
    // 每行第三个 uint32 是 padding；有效值覆盖 8 位边界、符号位和 uint32 最大值。
    const TArray<uint32> Source = {
        1u, 0x01020304u, 0xDEADBEEFu,
        0x80000001u, 0xFFFFFFFFu, 0xBAADF00Du
    };
    const TArray<uint32> Expected = {
        1u, 0x01020304u,
        0x80000001u, 0xFFFFFFFFu
    };

    TArray<uint8> ActualBytes;
    const bool bCopied = UE::SensorSimulation::ImageReadback::CopyR32UintToCanonical(
        reinterpret_cast<const uint8*>(Source.GetData()),
        3,
        PF_R32_UINT,
        FIntPoint(2, 2),
        ActualBytes);

    TestTrue(TEXT("R32_UINT instance conversion succeeds"), bCopied);
    TestEqual(
        TEXT("Instance output is tightly packed"),
        ActualBytes.Num(),
        Expected.Num() * static_cast<int32>(sizeof(uint32)));
    if (ActualBytes.Num() == Expected.Num() * static_cast<int32>(sizeof(uint32)))
    {
        TestTrue(
            TEXT("All 32 bits survive and padded texels are excluded"),
            FMemory::Memcmp(ActualBytes.GetData(), Expected.GetData(), ActualBytes.Num()) == 0);
    }
    return true;
}

#endif
