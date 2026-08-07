#include "ImageReadbackManager.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "RenderingThread.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackChannelStatsTest,
    "SensorSimulation.Renderer.Readback.ChannelStatsAreSeparated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackChannelStatsTest::RunTest(const FString& Parameters)
{
    AddExpectedError(
        TEXT("Readback rejected: target is null or payload type is unsupported."),
        EAutomationExpectedErrorFlags::Contains,
        2);

    FImageReadbackManager Manager(2);
    FCaptureRequest FrontRequest;
    FrontRequest.SensorName = TEXT("FrontCamera");
    FrontRequest.SensorGuid = FGuid(1, 0, 0, 0);
    FCaptureRequest RearRequest;
    RearRequest.SensorName = TEXT("RearCamera");
    RearRequest.SensorGuid = FGuid(2, 0, 0, 0);

    const FGuid FrontChannelGuid(10, 0, 0, 0);
    const FGuid RearChannelGuid(20, 0, 0, 0);
    TestFalse(
        TEXT("Front RGB null target is rejected"),
        Manager.Enqueue(nullptr, FrontRequest, FrontChannelGuid, EPayloadType::Rgb));
    TestFalse(
        TEXT("Rear Semantic null target is rejected"),
        Manager.Enqueue(nullptr, RearRequest, RearChannelGuid, EPayloadType::Semantic));

    const TArray<FImageReadbackChannelStats> Stats = Manager.GetChannelStats();
    TestEqual(TEXT("Two sensor/modality keys are reported separately"), Stats.Num(), 2);
    if (Stats.Num() == 2)
    {
        TestEqual(TEXT("Stats are sorted by SensorName"), Stats[0].SensorName, FName(TEXT("FrontCamera")));
        TestEqual(TEXT("Front key preserves ChannelGuid"), Stats[0].ChannelGuid, FrontChannelGuid);
        TestEqual(TEXT("Front key preserves RGB modality"), Stats[0].PayloadType, EPayloadType::Rgb);
        TestEqual(TEXT("Front rejection is attributed once"), Stats[0].RejectedCount, int64{1});
        TestEqual(TEXT("Rear key preserves Semantic modality"), Stats[1].PayloadType, EPayloadType::Semantic);
        TestEqual(TEXT("Rear rejection is attributed once"), Stats[1].RejectedCount, int64{1});
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackGlobalPumpTest,
    "SensorSimulation.Renderer.Readback.GlobalPumpBatchesManagers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FImageReadbackGlobalPumpTest::RunTest(const FString& Parameters)
{
    // 清理前一用例析构时排队的 Release 命令，使本用例的命令差值保持确定。
    FlushRenderingCommands();
    const FImageReadbackGlobalPumpStats Before = FImageReadbackManager::GetGlobalPumpStatsForTesting();

    {
        FImageReadbackManager FirstManager(1);
        FImageReadbackManager SecondManager(1);
        const FImageReadbackGlobalPumpStats Registered = FImageReadbackManager::GetGlobalPumpStatsForTesting();
        TestEqual(
            TEXT("Both managers register with the global coordinator"),
            Registered.RegisteredManagerCount,
            Before.RegisteredManagerCount + 2);

        FImagePayload Unused;
        TestFalse(TEXT("First empty manager has no payload"), FirstManager.PollCompleted(Unused));
        TestFalse(TEXT("Second empty manager has no payload"), SecondManager.PollCompleted(Unused));
        FlushRenderingCommands();

        const FImageReadbackGlobalPumpStats Pumped = FImageReadbackManager::GetGlobalPumpStatsForTesting();
        TestEqual(
            TEXT("Two Poll calls enqueue one global render command"),
            Pumped.PumpCommandCount - Before.PumpCommandCount,
            int64{1});
        TestEqual(
            TEXT("The one command visits both managers"),
            Pumped.PumpedManagerCount - Before.PumpedManagerCount,
            int64{2});
        TestTrue(TEXT("Peak batch size records both managers"), Pumped.PeakManagersPerPump >= 2);
    }

    TestEqual(
        TEXT("Destroyed managers unregister synchronously"),
        FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount,
        Before.RegisteredManagerCount);
    FlushRenderingCommands();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FImageReadbackCapacityHotUpdateTest,
    "SensorSimulation.Renderer.Readback.CapacityHotUpdate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 验证容量可以原地更新，并且非法的 0/负数设置会被安全限制为至少一个任务。 */
bool FImageReadbackCapacityHotUpdateTest::RunTest(const FString& Parameters)
{
    FImageReadbackManager Manager(4);
    TestEqual(TEXT("Constructor applies initial capacity"), Manager.GetCapacity(), 4);
    TestEqual(TEXT("Stats expose initial capacity"), Manager.GetStats().Capacity, 4);

    Manager.SetCapacity(2);
    TestEqual(TEXT("Capacity can shrink without rebuilding manager"), Manager.GetCapacity(), 2);
    TestEqual(TEXT("Stats expose updated capacity"), Manager.GetStats().Capacity, 2);

    Manager.SetCapacity(0);
    TestEqual(TEXT("Capacity is clamped to at least one"), Manager.GetCapacity(), 1);
    return true;
}
#endif
