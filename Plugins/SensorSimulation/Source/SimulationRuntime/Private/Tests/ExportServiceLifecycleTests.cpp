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

    Service.Stop();
    Service.Stop();
    TestEqual(TEXT("Repeated final Stop is idempotent"), Service.GetPendingCount(), 0);
    TestEqual(TEXT("Both sessions exported their frames"), Service.GetExportedFrameCount(), int64{2});
    TestEqual(TEXT("No frame failed during lifecycle transitions"), Service.GetFailedFrameCount(), int64{0});
    TestTrue(
        TEXT("The restarted worker wrote the second frame"),
        IFileManager::Get().FileExists(*(OutputRoot / TEXT("frame_000002") / TEXT("frame_info.json"))));

    return true;
}

#endif
