#include "DatasetSession.h"
#include "FrameAssembler.h"
#include "ExportService.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FDatasetSessionCalibrationUpsertTest,
    "SensorSimulation.Lifecycle.DatasetSession.CalibrationUpsert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 验证同一 Rig 的不同 ChannelGuid 独立保存，而热更新只覆盖目标通道。 */
bool FDatasetSessionCalibrationUpsertTest::RunTest(const FString& Parameters)
{
    const FString OutputRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SensorSimulation"), TEXT("CalibrationUpsert"));
    IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

    FDatasetSession Session;
    if (!TestTrue(TEXT("Dataset session starts"), Session.Start(OutputRoot, TEXT("HotUpdate"))))
    {
        return false;
    }

    FCalibration Rgb;
    Rgb.SensorName = TEXT("FrontCamera");
    Rgb.SensorGuid = FGuid(1, 2, 3, 4);
    Rgb.ChannelGuid = FGuid(1, 2, 3, 4);
    Rgb.PayloadType = EPayloadType::Rgb;
    Rgb.ImageSize = FIntPoint(320, 180);
    Rgb.Fx = 160.0;
    Rgb.Fy = 160.0;
    Session.RegisterCalibration(Rgb);

    FCalibration Semantic = Rgb;
    Semantic.ChannelGuid = FGuid(5, 6, 7, 8);
    Semantic.PayloadType = EPayloadType::Semantic;
    Semantic.ImageSize = FIntPoint(800, 600);
    Semantic.Fx = 400.0;
    Semantic.Fy = 400.0;
    Session.RegisterCalibration(Semantic);

    FCalibration UpdatedRgb = Rgb;
    UpdatedRgb.ImageSize = FIntPoint(640, 360);
    UpdatedRgb.Fx = 320.0;
    UpdatedRgb.Fy = 320.0;
    // 同 GUID 注册只覆盖 RGB；同名 Semantic 必须继续保留自己的分辨率。
    Session.RegisterCalibration(UpdatedRgb);

    const FString SessionDirectory = Session.GetSessionDirectory();
    Session.Stop();

    FString JsonText;
    if (!TestTrue(TEXT("Calibration file is written"),
        FFileHelper::LoadFileToString(JsonText, *(SessionDirectory / TEXT("calibration.json")))))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!TestTrue(TEXT("Calibration JSON parses"), FJsonSerializer::Deserialize(Reader, RootObject)) ||
        !TestTrue(TEXT("Calibration JSON has a root object"), RootObject.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>& Sensors = RootObject->GetArrayField(TEXT("sensors"));
    TestEqual(TEXT("Two channels on one Rig remain independent"), Sensors.Num(), 2);
    if (Sensors.Num() == 2)
    {
        const TSharedPtr<FJsonObject> First = Sensors[0]->AsObject();
        const TSharedPtr<FJsonObject> Second = Sensors[1]->AsObject();
        TestEqual(TEXT("RGB hot update replaces only RGB width"),
            First->GetIntegerField(TEXT("image_width")), 640);
        TestEqual(TEXT("RGB payload type is explicit"),
            First->GetStringField(TEXT("payload_type")), FString(TEXT("rgb")));
        TestEqual(TEXT("Semantic width remains independent"),
            Second->GetIntegerField(TEXT("image_width")), 800);
        TestEqual(TEXT("Semantic payload type is explicit"),
            Second->GetStringField(TEXT("payload_type")), FString(TEXT("semantic")));
        TestFalse(TEXT("Each channel writes a non-empty GUID"),
            First->GetStringField(TEXT("channel_guid")).IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FDatasetSessionRendererMetricsTest,
    "SensorSimulation.Lifecycle.DatasetSession.RendererMetrics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 验证 Camera Rig 资源、Readback 容量和按通道延迟进入机器可读会话 metadata。 */
bool FDatasetSessionRendererMetricsTest::RunTest(const FString& Parameters)
{
    const FString OutputRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("SensorSimulation"), TEXT("RendererMetrics"));
    IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);

    FDatasetSession Session;
    if (!TestTrue(TEXT("Dataset session starts"), Session.Start(OutputRoot, TEXT("Metrics"))))
    {
        return false;
    }

    FCameraRendererMetricsSnapshot Metrics;
    Metrics.SensorName = TEXT("FrontCamera");
    Metrics.SensorGuid = FGuid(1, 2, 3, 4);
    Metrics.ResourceStats.CreatedRenderTargets = 3;
    Metrics.ResourceStats.ReusedRenderTargets = 7;
    Metrics.ReadbackStats.Capacity = 5;
    Metrics.ReadbackStats.PeakPendingCount = 4;
    FImageReadbackChannelStats& RgbStats = Metrics.ChannelStats.AddDefaulted_GetRef();
    RgbStats.SensorGuid = Metrics.SensorGuid;
    RgbStats.SensorName = Metrics.SensorName;
    RgbStats.ChannelGuid = FGuid(9, 8, 7, 6);
    RgbStats.PayloadType = EPayloadType::Rgb;
    RgbStats.DeliveredCount = 12;
    RgbStats.AverageGpuLatencyMs = 1.25;
    Session.RegisterRendererMetrics(Metrics);

    const FString SessionDirectory = Session.GetSessionDirectory();
    FFrameAssemblerStats FrameStats;
    FrameStats.TotalFrames = 12;
    FrameStats.CompletedFrames = 10;
    FrameStats.FailedFrames = 1;
    FrameStats.TimeoutFrames = 1;
    FrameStats.BusyFrames = 1;
    FrameStats.DuplicatePayloads = 2;
    FrameStats.LatePayloads = 3;
    FrameStats.PeakPendingFrames = 6;
    FrameStats.CapacityRejectedFrames = 4;
    FExportServiceStats ExportStats;
    ExportStats.EnqueuedFrames = 11;
    ExportStats.RejectedFrames = 2;
    ExportStats.DroppedFrames = 1;
    ExportStats.CommittedFrames = 9;
    ExportStats.FailedFrames = 1;
    ExportStats.PeakPendingFrames = 7;
    Session.WriteMetadata(FrameStats, ExportStats, 42, TEXT("DeterministicDataset"));
    Session.Stop();

    FString JsonText;
    if (!TestTrue(TEXT("Metadata file is written"),
        FFileHelper::LoadFileToString(JsonText, *(SessionDirectory / TEXT("metadata.json")))))
    {
        return false;
    }

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!TestTrue(TEXT("Metadata JSON parses"), FJsonSerializer::Deserialize(Reader, RootObject)) ||
        !TestTrue(TEXT("Metadata JSON has a root object"), RootObject.IsValid()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Statistics = RootObject->GetObjectField(TEXT("statistics"));
    TestEqual(TEXT("Timeout frame count is written"), Statistics->GetIntegerField(TEXT("timeout_frames")), 1);
    TestEqual(TEXT("Busy frame count is written"), Statistics->GetIntegerField(TEXT("busy_frames")), 1);
    TestEqual(TEXT("Duplicate payload count is written"), Statistics->GetIntegerField(TEXT("duplicate_payloads")), 2);
    TestEqual(TEXT("Late payload count is written"), Statistics->GetIntegerField(TEXT("late_payloads")), 3);
    TestEqual(TEXT("FrameAssembler peak is written"),
        Statistics->GetIntegerField(TEXT("frame_assembler_peak_pending")), 6);
    TestEqual(TEXT("Frame capacity rejection count is written"),
        Statistics->GetIntegerField(TEXT("capacity_rejected_frames")), 4);
    TestEqual(TEXT("Export queue peak is written"),
        Statistics->GetIntegerField(TEXT("export_peak_pending")), 7);
    TestEqual(TEXT("Export committed count is written"),
        Statistics->GetIntegerField(TEXT("export_committed_frames")), 9);
    TestEqual(TEXT("Export rejected count is written"),
        Statistics->GetIntegerField(TEXT("export_rejected_frames")), 2);

    const TSharedPtr<FJsonObject> Renderer = RootObject->GetObjectField(TEXT("renderer"));
    const TArray<TSharedPtr<FJsonValue>>& Rigs = Renderer->GetArrayField(TEXT("camera_rigs"));
    TestEqual(TEXT("One Camera Rig snapshot is written"), Rigs.Num(), 1);
    if (Rigs.Num() == 1)
    {
        const TSharedPtr<FJsonObject> Rig = Rigs[0]->AsObject();
        TestEqual(TEXT("Readback capacity is written"),
            Rig->GetObjectField(TEXT("readback"))->GetIntegerField(TEXT("capacity")), 5);
        TestEqual(TEXT("RenderTarget reuse count is written"),
            Rig->GetObjectField(TEXT("resources"))->GetIntegerField(TEXT("reused_render_targets")), 7);
        const TArray<TSharedPtr<FJsonValue>>& Channels = Rig->GetArrayField(TEXT("channels"));
        TestEqual(TEXT("Per-channel metrics are written"), Channels.Num(), 1);
        if (Channels.Num() == 1)
        {
            TestEqual(TEXT("Readback metrics preserve ChannelGuid"),
                Channels[0]->AsObject()->GetStringField(TEXT("channel_guid")),
                RgbStats.ChannelGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
        }
        if (Channels.Num() == 1)
        {
            TestEqual(TEXT("Channel modality is stable text"),
                Channels[0]->AsObject()->GetStringField(TEXT("payload_type")), FString(TEXT("rgb")));
            TestEqual(TEXT("Channel delivered count is written"),
                Channels[0]->AsObject()->GetIntegerField(TEXT("delivered")), 12);
        }
    }
    return true;
}

#endif
