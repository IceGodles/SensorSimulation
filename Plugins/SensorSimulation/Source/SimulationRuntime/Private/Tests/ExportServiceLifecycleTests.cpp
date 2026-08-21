#include "ExportService.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FExportServiceLifecycleTest,
    "SensorSimulation.Lifecycle.ExportService.StopRestartAndDrain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExportServiceLifecycleTest::RunTest(const FString& Parameters)
{
    const FString OutputRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("Automation"),
        TEXT("SensorSimulation"),
        TEXT("ExportLifecycle"));

    // 测试目录只保存本次生成的最小 frame_info.json，避免旧结果掩盖 Worker 未真正写出的错误。
    IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

    FExportService Service(2);

    // Stop 必须可在 Start 前和已停止状态下重复调用，便于 World/Subsystem 多路径清理。
    Service.Stop();
    Service.Stop();
    TestEqual(TEXT("Stop-before-Start leaves no pending frames"), Service.GetPendingCount(), 0);

    TestTrue(TEXT("Export worker starts"), Service.Start(OutputRoot));

    FFramePacket FirstPacket;
    FirstPacket.Header.FrameId = 1;
    FirstPacket.Header.SequenceId = 7;
    // 用两个跨越 8 位边界的原生 uint32 验证 Instance Writer 不执行 PNG/颜色转换。
    FirstPacket.ExpectedPayloads = EPayloadType::Instance;
    FirstPacket.CompletedPayloads = EPayloadType::Instance;
    const FGuid InstanceChannelGuid(10, 20, 30, 40);
    FImagePayload& InstanceImage = FirstPacket.Images.AddDefaulted_GetRef();
    InstanceImage.Header = FirstPacket.Header;
    InstanceImage.SensorName = TEXT("LifecycleCamera");
    InstanceImage.ChannelGuid = InstanceChannelGuid;
    InstanceImage.PayloadType = EPayloadType::Instance;
    InstanceImage.ImageSize = FIntPoint(2, 1);
    InstanceImage.ViewRect = FIntRect(0, 0, 2, 1);
    InstanceImage.PixelFormat = EImagePixelFormat::R32Uint;
    InstanceImage.ColorSpace = EImageColorSpace::Data;
    InstanceImage.ValueUnit = EImageValueUnit::Identifier;
    InstanceImage.BytesPerPixel = sizeof(uint32);
    InstanceImage.RowStrideBytes = 2 * sizeof(uint32);
    const uint32 ExpectedInstanceIds[] = { 1u, 0x01020304u };
    InstanceImage.Bytes.Append(
        reinterpret_cast<const uint8*>(ExpectedInstanceIds),
        sizeof(ExpectedInstanceIds));
    TestTrue(
        TEXT("A complete frame is accepted before Stop"),
        Service.Enqueue(MoveTemp(FirstPacket), EExportBackpressurePolicy::RejectNewest));

    // Stop 必须等待 Worker 排空队列，否则关卡退出可能静默丢失最后几帧。
    Service.Stop();
    TestEqual(TEXT("Stop drains the first session queue"), Service.GetPendingCount(), 0);
    TestEqual(TEXT("The first frame is exported before Stop returns"), Service.GetExportedFrameCount(), int64{1});
    TestTrue(
        TEXT("The first frame metadata exists"),
        IFileManager::Get().FileExists(*(OutputRoot / TEXT("frame_000001") / TEXT("frame_info.json"))));

    const FString InstancePath = OutputRoot / TEXT("frame_000001") /
        (TEXT("instance_u32_") + InstanceChannelGuid.ToString(EGuidFormats::Digits) + TEXT(".bin"));
    TArray<uint8> WrittenInstanceBytes;
    TestTrue(
        TEXT("Instance Writer creates ChannelGuid-addressed instance file"),
        FFileHelper::LoadFileToArray(WrittenInstanceBytes, *InstancePath));
    TestEqual(
        TEXT("Instance Writer preserves the uint32 byte count"),
        WrittenInstanceBytes.Num(),
        static_cast<int32>(sizeof(ExpectedInstanceIds)));
    if (WrittenInstanceBytes.Num() == sizeof(ExpectedInstanceIds))
    {
        TestTrue(
            TEXT("Instance Writer preserves all 32 identifier bits"),
            FMemory::Memcmp(
                WrittenInstanceBytes.GetData(),
                ExpectedInstanceIds,
                sizeof(ExpectedInstanceIds)) == 0);
    }

    // 同一个服务对象允许随 Subsystem/PIE 生命周期重新启动，而不遗留旧 Worker。
    TestTrue(TEXT("Export worker restarts after Stop"), Service.Start(OutputRoot));

    FFramePacket SecondPacket;
    SecondPacket.Header.FrameId = 2;
    SecondPacket.Header.SequenceId = 7;
    SecondPacket.ExpectedPayloads = EPayloadType::None;
    SecondPacket.CompletedPayloads = EPayloadType::None;
    TestTrue(
        TEXT("A complete frame is accepted after restart"),
        Service.Enqueue(MoveTemp(SecondPacket), EExportBackpressurePolicy::RejectNewest));

    FFramePacket EmptyLidarPacket;
    EmptyLidarPacket.Header.FrameId = 3;
    EmptyLidarPacket.Header.SequenceId = 7;
    EmptyLidarPacket.ExpectedPayloads = EPayloadType::Lidar;
    EmptyLidarPacket.CompletedPayloads = EPayloadType::Lidar;
    FLidarScanPayload& EmptyScan = EmptyLidarPacket.LidarScans.AddDefaulted_GetRef();
    EmptyScan.Header = EmptyLidarPacket.Header;
    EmptyScan.SensorGuid = FGuid(11, 22, 33, 44);
    EmptyScan.SensorName = TEXT("EmptyLidar");
    EmptyScan.ExpectedRayCount = 32;
    EmptyScan.CompletedRayCount = 32;
    EmptyScan.bCompleteRevolution = true;
    TestTrue(TEXT("A zero-hit LiDAR frame is accepted"),
        Service.Enqueue(MoveTemp(EmptyLidarPacket), EExportBackpressurePolicy::RejectNewest));

    Service.Stop();
    Service.Stop();
    TestEqual(TEXT("Repeated final Stop is idempotent"), Service.GetPendingCount(), 0);
    TestEqual(TEXT("Both sessions exported all three frames"), Service.GetExportedFrameCount(), int64{3});
    TestEqual(TEXT("No frame failed during lifecycle transitions"), Service.GetFailedFrameCount(), int64{0});
    TestTrue(
        TEXT("The restarted worker wrote the second frame"),
        IFileManager::Get().FileExists(*(OutputRoot / TEXT("frame_000002") / TEXT("frame_info.json"))));
    const FString EmptyLidarPath = OutputRoot / TEXT("frame_000003") / TEXT("lidar.bin");
    TestTrue(TEXT("Zero-hit LiDAR still creates lidar.bin"),
        IFileManager::Get().FileExists(*EmptyLidarPath));
    TestEqual(TEXT("Zero-hit lidar.bin is exactly empty"),
        IFileManager::Get().FileSize(*EmptyLidarPath), int64{0});
    const FString EmptyExtendedLidarPath =
        OutputRoot / TEXT("frame_000003") / TEXT("lidar_extended.bin");
    TArray<uint8> EmptyExtendedBytes;
    TestTrue(TEXT("Zero-hit LiDAR creates a versioned extended payload"),
        FFileHelper::LoadFileToArray(EmptyExtendedBytes, *EmptyExtendedLidarPath));
    TestEqual(TEXT("Zero-hit extended payload contains only its 32-byte header"),
        EmptyExtendedBytes.Num(), 32);
    if (EmptyExtendedBytes.Num() == 32)
    {
        uint32 Magic = 0;
        uint16 Version = 0;
        uint32 PointCount = 1;
        FMemory::Memcpy(&Magic, EmptyExtendedBytes.GetData(), sizeof(Magic));
        FMemory::Memcpy(&Version, EmptyExtendedBytes.GetData() + 4, sizeof(Version));
        FMemory::Memcpy(&PointCount, EmptyExtendedBytes.GetData() + 12, sizeof(PointCount));
        TestEqual(TEXT("Extended LiDAR magic is stable"), Magic, uint32{0x52444C53});
        TestEqual(TEXT("Extended LiDAR schema version is 2"), Version, uint16{2});
        TestEqual(TEXT("Zero-hit extended LiDAR reports zero points"), PointCount, uint32{0});
    }
    TestFalse(TEXT("Committed frames leave no temporary directory"),
        IFileManager::Get().DirectoryExists(*(OutputRoot / TEXT("frame_000003.tmp"))));

    // 任一 Writer 失败时不得暴露最终 frame 目录；临时目录保留为可诊断状态。
    TestTrue(TEXT("Export worker starts for atomic failure injection"), Service.Start(OutputRoot));
    FFramePacket InvalidPacket;
    InvalidPacket.Header.FrameId = 4;
    InvalidPacket.ExpectedPayloads = EPayloadType::Rgb;
    InvalidPacket.CompletedPayloads = EPayloadType::Rgb;
    FImagePayload& InvalidImage = InvalidPacket.Images.AddDefaulted_GetRef();
    InvalidImage.PayloadType = EPayloadType::Rgb;
    InvalidImage.ImageSize = FIntPoint(1, 1);
    InvalidImage.Bytes = { 255, 0, 0, 255 };
    AddExpectedError(TEXT("Cannot export image without ChannelGuid"),
        EAutomationExpectedErrorFlags::Contains, 1);
    TestTrue(TEXT("Complete invalid frame reaches Writer fault injection"),
        Service.Enqueue(MoveTemp(InvalidPacket), EExportBackpressurePolicy::RejectNewest));
    Service.Stop();
    TestFalse(TEXT("Failed transaction never exposes a final frame directory"),
        IFileManager::Get().DirectoryExists(*(OutputRoot / TEXT("frame_000004"))));
    TestTrue(TEXT("Failed transaction remains explicitly marked as temporary"),
        IFileManager::Get().DirectoryExists(*(OutputRoot / TEXT("frame_000004.tmp"))));
    TestEqual(TEXT("Writer failure is counted"), Service.GetFailedFrameCount(), int64{1});

    const FExportServiceStats ExportStats = Service.GetStats();
    TestEqual(TEXT("Committed count equals final frame count"), ExportStats.CommittedFrames, int64{3});
    TestEqual(TEXT("All accepted frames are counted as enqueued"), ExportStats.EnqueuedFrames, int64{4});

    return true;
}

#endif
