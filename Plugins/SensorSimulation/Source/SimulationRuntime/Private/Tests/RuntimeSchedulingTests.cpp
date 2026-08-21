#include "SimulationScheduler.h"
#include "SimulationSettings.h"
#include "FrameAssembler.h"
#include "SensorCapturePlanner.h"
#include "SimulationSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMixedSensorRateCapturePlannerTest,
    "SensorSimulation.Runtime.Scheduling.MixedSensorRates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMixedSensorRateCapturePlannerTest::RunTest(const FString& Parameters)
{
    FSensorCapturePlanner Planner;
    const FGuid CameraGuid(1, 2, 3, 4);
    const FGuid LidarGuid(5, 6, 7, 8);
    int32 CameraCaptures = 0;
    int32 LidarCaptures = 0;

    // 20 Hz 主时钟运行 1 秒；Camera=20 Hz，LiDAR=10 Hz，首次主步两者都立即采样。
    for (int32 Step = 1; Step <= 20; ++Step)
    {
        const double Timestamp = Step * 0.05;
        if (Planner.IsDue(CameraGuid, Timestamp))
        {
            ++CameraCaptures;
            Planner.MarkAttempt(CameraGuid, 20.0f, Timestamp);
        }
        if (Planner.IsDue(LidarGuid, Timestamp))
        {
            ++LidarCaptures;
            Planner.MarkAttempt(LidarGuid, 10.0f, Timestamp);
        }
    }

    TestEqual(TEXT("20 Hz camera participates in every master step"), CameraCaptures, 20);
    TestEqual(TEXT("10 Hz LiDAR participates in every second master step"), LidarCaptures, 10);

    // 大幅跨过若干周期后只允许一次到期，不追赶制造请求风暴。
    TestTrue(TEXT("A delayed sensor is due once"), Planner.IsDue(LidarGuid, 5.0));
    Planner.MarkAttempt(LidarGuid, 10.0f, 5.0);
    TestFalse(TEXT("A delayed sensor advances beyond the current timestamp"), Planner.IsDue(LidarGuid, 5.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSimulationWorldEligibilityTest,
    "SensorSimulation.Runtime.Session.WorldEligibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSimulationWorldEligibilityTest::RunTest(const FString& Parameters)
{
    const USimulationSubsystem* Subsystem = GetDefault<USimulationSubsystem>();
    UWorld* World = NewObject<UWorld>();

    World->WorldType = EWorldType::Editor;
    TestFalse(TEXT("Editor asset world does not create a dataset session"),
        Subsystem->ShouldCreateSubsystem(World));
    World->WorldType = EWorldType::EditorPreview;
    TestFalse(TEXT("Editor preview world does not create a dataset session"),
        Subsystem->ShouldCreateSubsystem(World));
    World->WorldType = EWorldType::Game;
    TestTrue(TEXT("Game world creates the simulation subsystem"),
        Subsystem->ShouldCreateSubsystem(World));
    World->WorldType = EWorldType::PIE;
    TestTrue(TEXT("PIE world creates the simulation subsystem"),
        Subsystem->ShouldCreateSubsystem(World));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FDeterministicSimulationSchedulerTest,
    "SensorSimulation.Runtime.Clock.DeterministicSchedulerAndExplicitPause",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeterministicSimulationSchedulerTest::RunTest(const FString& Parameters)
{
    FSimulationScheduler Scheduler;
    Scheduler.Initialize(ESimulationMode::DeterministicDataset, 0.05);

    const TOptional<double> First = Scheduler.Poll(0.001, true, true);
    TestTrue(TEXT("Deterministic scheduler emits the first step immediately"), First.IsSet());
    if (First.IsSet())
    {
        TestEqual(TEXT("First deterministic timestamp is one fixed step"), First.GetValue(), 0.05);
    }

    TestFalse(TEXT("Pending frame explicitly pauses deterministic scheduling"),
        Scheduler.Poll(10.0, false, true).IsSet());
    TestEqual(TEXT("Pending pause reason is observable"),
        Scheduler.GetPauseReason(), ESimulationSchedulerPauseReason::FramePipelineBusy);
    TestEqual(TEXT("Large Tick DeltaTime cannot advance a paused deterministic clock"),
        Scheduler.GetSimulationSeconds(), 0.05);

    TestFalse(TEXT("Full Export Queue explicitly pauses deterministic scheduling"),
        Scheduler.Poll(20.0, true, false).IsSet());
    TestEqual(TEXT("Export backpressure pause reason is observable"),
        Scheduler.GetPauseReason(), ESimulationSchedulerPauseReason::ExportBackpressure);
    TestEqual(TEXT("Backpressure does not advance deterministic time"),
        Scheduler.GetSimulationSeconds(), 0.05);

    const TOptional<double> Second = Scheduler.Poll(0.0, true, true);
    TestTrue(TEXT("Scheduler resumes when pipeline and Export are ready"), Second.IsSet());
    if (Second.IsSet())
    {
        TestEqual(TEXT("Resumed timestamp advances exactly one fixed step"), Second.GetValue(), 0.10);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRealtimeBackpressureSchedulerTest,
    "SensorSimulation.Runtime.Clock.RealtimeExportBackpressure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRealtimeBackpressureSchedulerTest::RunTest(const FString& Parameters)
{
    FSimulationScheduler Scheduler;
    Scheduler.Initialize(ESimulationMode::Realtime, 0.05);

    TestFalse(TEXT("Realtime clock pauses before creating work when Export is full"),
        Scheduler.Poll(0.25, true, false).IsSet());
    TestEqual(TEXT("Realtime backpressure reason is explicit"),
        Scheduler.GetPauseReason(), ESimulationSchedulerPauseReason::ExportBackpressure);
    TestEqual(TEXT("Paused realtime clock does not accumulate an unbounded catch-up burst"),
        Scheduler.GetSimulationSeconds(), 0.0);

    const TOptional<double> Resumed = Scheduler.Poll(0.05, true, true);
    TestTrue(TEXT("Realtime clock resumes when Export has capacity"), Resumed.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FDeterministicThousandStepScheduleTest,
    "SensorSimulation.Runtime.Clock.ThousandStepDeterminism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeterministicThousandStepScheduleTest::RunTest(const FString& Parameters)
{
    auto BuildScheduleHash = [](const double TickDelta)
    {
        FSimulationScheduler Scheduler;
        FSensorCapturePlanner Planner;
        Scheduler.Initialize(ESimulationMode::DeterministicDataset, 0.05);
        const FGuid CameraGuid(1, 2, 3, 4);
        const FGuid LidarGuid(5, 6, 7, 8);
        uint32 Hash = 0;
        for (int32 Step = 0; Step < 1000; ++Step)
        {
            const TOptional<double> Timestamp = Scheduler.Poll(TickDelta, true, true);
            check(Timestamp.IsSet());
            const bool bCameraDue = Planner.IsDue(CameraGuid, Timestamp.GetValue());
            const bool bLidarDue = Planner.IsDue(LidarGuid, Timestamp.GetValue());
            Hash = HashCombineFast(Hash, GetTypeHash(Timestamp.GetValue()));
            Hash = HashCombineFast(Hash, GetTypeHash(bCameraDue));
            Hash = HashCombineFast(Hash, GetTypeHash(bLidarDue));
            if (bCameraDue) Planner.MarkAttempt(CameraGuid, 20.0f, Timestamp.GetValue());
            if (bLidarDue) Planner.MarkAttempt(LidarGuid, 10.0f, Timestamp.GetValue());
        }
        return Hash;
    };

    TestEqual(TEXT("Deterministic schedule is independent of game-thread frame delta"),
        BuildScheduleHash(1.0 / 30.0), BuildScheduleHash(1.0 / 144.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRuntimeSettingsSnapshotTest,
    "SensorSimulation.Runtime.Settings.DatasetRootAndSessionSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeSettingsSnapshotTest::RunTest(const FString& Parameters)
{
    USimulationSettings* Settings = NewObject<USimulationSettings>();
    Settings->SimulationMode = ESimulationMode::DeterministicDataset;
    Settings->FixedStepSeconds = 0.025;
    Settings->MaxPendingFrames = 5;
    Settings->MaxPendingAssemblyFrames = 6;
    Settings->TerminalFrameHistoryCapacity = 77;
    Settings->DatasetRoot.Path = TEXT("Automation/RuntimeSnapshot");
    Settings->FrameTimeoutSeconds = 3.5;
    Settings->RandomSeed = 1234;

    const FSimulationRuntimeSettingsSnapshot Snapshot =
        FSimulationRuntimeSettingsSnapshot::Capture(*Settings);
    FString SavedRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
    FPaths::NormalizeDirectoryName(SavedRoot);
    TestTrue(TEXT("Relative DatasetRoot resolves under Project/Saved"),
        FPaths::IsUnderDirectory(Snapshot.DatasetRoot, SavedRoot));
    TestTrue(TEXT("Resolved DatasetRoot is absolute"), !FPaths::IsRelative(Snapshot.DatasetRoot));

    // 模拟采集中途修改 Settings CDO；已经捕获的会话快照必须保持启动时语义。
    Settings->SimulationMode = ESimulationMode::Realtime;
    Settings->FixedStepSeconds = 1.0;
    Settings->MaxPendingFrames = 99;
    Settings->MaxPendingAssemblyFrames = 100;
    Settings->TerminalFrameHistoryCapacity = 1000;
    Settings->DatasetRoot.Path = TEXT("ChangedDuringSession");
    Settings->FrameTimeoutSeconds = 99.0;
    Settings->RandomSeed = 9999;

    TestEqual(TEXT("Snapshot keeps the original mode"),
        Snapshot.SimulationMode, ESimulationMode::DeterministicDataset);
    TestEqual(TEXT("Snapshot keeps the original fixed step"), Snapshot.FixedStepSeconds, 0.025);
    TestEqual(TEXT("Snapshot keeps the original Export capacity"), Snapshot.MaxPendingFrames, 5);
    TestEqual(TEXT("Snapshot keeps the original assembler capacity"), Snapshot.MaxPendingAssemblyFrames, 6);
    TestEqual(TEXT("Snapshot keeps the original terminal history capacity"),
        Snapshot.TerminalFrameHistoryCapacity, 77);
    TestEqual(TEXT("Snapshot keeps the original timeout"), Snapshot.FrameTimeoutSeconds, 3.5);
    TestEqual(TEXT("Snapshot keeps the original seed"), Snapshot.RandomSeed, 1234);
    TestTrue(TEXT("Snapshot root is unaffected by later Settings edits"),
        Snapshot.DatasetRoot.Contains(TEXT("Automation/RuntimeSnapshot")) ||
        Snapshot.DatasetRoot.Contains(TEXT("Automation\\RuntimeSnapshot")));

    FDirectoryPath EmptyRoot;
    const FString DefaultRoot = USimulationSettings::ResolveDatasetRoot(EmptyRoot);
    TestEqual(TEXT("Empty DatasetRoot resolves to Saved/SensorSimulation"),
        DefaultRoot, FPaths::Combine(SavedRoot, TEXT("SensorSimulation")));

    FDirectoryPath EscapingRoot;
    EscapingRoot.Path = TEXT("../../OutsideSaved");
    AddExpectedError(TEXT("escapes Project/Saved"), EAutomationExpectedErrorFlags::Contains, 1);
    TestEqual(TEXT("Relative DatasetRoot cannot escape Project/Saved"),
        USimulationSettings::ResolveDatasetRoot(EscapingRoot), DefaultRoot);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCompletedFrameBackpressureTimeoutTest,
    "SensorSimulation.Runtime.Clock.CompletedFrameSurvivesExportPause",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompletedFrameBackpressureTimeoutTest::RunTest(const FString& Parameters)
{
    FFrameAssembler Assembler;
    FFrameHeader Header;
    Header.SequenceId = 1;
    Header.FrameId = 77;
    Header.SimulationTimestampSeconds = 0.05;
    Assembler.BeginFrame(Header, EPayloadType::GroundTruth, 100.0);
    TestTrue(TEXT("Ground truth completes the frame"),
        Assembler.AddGroundTruth(Header.FrameId, {}));

    // 完整帧可能因 Export Queue 满而暂留聚合器；它不能被当成传感器超时帧清除。
    TestEqual(TEXT("Completed frame is not purged while Export is backpressured"),
        Assembler.PurgeTimedOutFrames(1000.0, 2.0), 0);
    FFramePacket Packet;
    TestTrue(TEXT("Completed frame remains available after a long Export pause"),
        Assembler.PopCompleteFrame(Packet));
    TestEqual(TEXT("Preserved frame keeps its identity"), Packet.Header.FrameId, uint64{77});
    return true;
}

#endif
