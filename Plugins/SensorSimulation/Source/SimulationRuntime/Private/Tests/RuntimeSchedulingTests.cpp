#include "SimulationScheduler.h"
#include "SimulationSettings.h"
#include "FrameAssembler.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

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
    FRuntimeSettingsSnapshotTest,
    "SensorSimulation.Runtime.Settings.DatasetRootAndSessionSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeSettingsSnapshotTest::RunTest(const FString& Parameters)
{
    USimulationSettings* Settings = NewObject<USimulationSettings>();
    Settings->SimulationMode = ESimulationMode::DeterministicDataset;
    Settings->FixedStepSeconds = 0.025;
    Settings->MaxPendingFrames = 5;
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
    Settings->DatasetRoot.Path = TEXT("ChangedDuringSession");
    Settings->FrameTimeoutSeconds = 99.0;
    Settings->RandomSeed = 9999;

    TestEqual(TEXT("Snapshot keeps the original mode"),
        Snapshot.SimulationMode, ESimulationMode::DeterministicDataset);
    TestEqual(TEXT("Snapshot keeps the original fixed step"), Snapshot.FixedStepSeconds, 0.025);
    TestEqual(TEXT("Snapshot keeps the original Export capacity"), Snapshot.MaxPendingFrames, 5);
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
