#include "CameraRigComponent.h"
#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::RenderOutputMatrixPhase2Tests
{
static const FName CameraTag(TEXT("RendererPhase2Camera"));
static const FName MovingTag(TEXT("RendererPhase2Moving"));
static const FName BackingTag(TEXT("RendererPhase2Backing"));
static const FName ProxyTag(TEXT("RendererPhase2Proxy"));

static constexpr uint8 MovingSemanticId = 61;
static constexpr uint8 BackingSemanticId = 62;
static constexpr uint8 ProxySemanticId = 70;
static constexpr uint32 MovingInstanceId = 0x01051000u;
static constexpr uint32 BackingInstanceId = 0x01052000u;
static constexpr uint32 ProxyInstanceId = 0x01053000u;
static constexpr int32 ExpectedChannelCount = 8;
static constexpr int32 ExpectedAcceptedCount = 7;

enum class ESceneExpectation : uint8
{
    MovingForeground,
    BackingVisible,
    CameraMovedForward,
    ProxyLabelsBackingVisuals,
    ProxyIgnoredBackingVisuals
};

/** 创建 BasicShape 动态材质，让 RGB 中的三个对象具有可区分颜色。 */
UMaterialInstanceDynamic* CreateColoredMaterial(UObject* Outer, const FLinearColor& Color)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(
        nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Outer);
    if (Material)
    {
        Material->SetVectorParameterValue(TEXT("Color"), Color);
    }
    return Material;
}

/** 创建真实透明源；标签路径由 Ignore/OpaqueProxy 策略决定是否使用其代理。 */
UMaterial* CreateTranslucentMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(), TEXT("RendererPhase2Translucent"), RF_Transient);
    Material->BlendMode = BLEND_Translucent;
    Material->TwoSided = true;
    Material->PostEditChange();
    return Material;
}

EImagePixelFormat ExpectedPixelFormat(const EPayloadType Type)
{
    switch (Type)
    {
    case EPayloadType::Rgb:
    case EPayloadType::Semantic:
        return EImagePixelFormat::Rgba8;
    case EPayloadType::Depth:
        return EImagePixelFormat::R32Float;
    case EPayloadType::Instance:
        return EImagePixelFormat::R32Uint;
    default:
        return EImagePixelFormat::Unknown;
    }
}

EImageColorSpace ExpectedColorSpace(const EPayloadType Type)
{
    switch (Type)
    {
    case EPayloadType::Rgb:
        return EImageColorSpace::SRgb;
    case EPayloadType::Semantic:
    case EPayloadType::Depth:
    case EPayloadType::Instance:
        return EImageColorSpace::Data;
    default:
        return EImageColorSpace::Unknown;
    }
}

EImageValueUnit ExpectedValueUnit(const EPayloadType Type)
{
    switch (Type)
    {
    case EPayloadType::Semantic:
    case EPayloadType::Instance:
        return EImageValueUnit::Identifier;
    case EPayloadType::Depth:
        return EImageValueUnit::Meters;
    default:
        return EImageValueUnit::None;
    }
}

int32 CenterOffset(const FImagePayload& Payload)
{
    return ((Payload.ImageSize.Y / 2) * Payload.ImageSize.X + Payload.ImageSize.X / 2) * 4;
}

/**
 * 连续执行高分辨率静态、运动、遮挡和 OpaqueProxy 热切换捕获。
 *
 * 每一帧必须完全消费 8 个 ChannelGuid 后才能改变场景，保证捕获时刻与断言状态一一对应。
 */
class FCapturePhase2MatrixCommand final : public IAutomationLatentCommand
{
public:
    explicit FCapturePhase2MatrixCommand(FAutomationTestBase* InTest)
        : Test(InTest), StartSeconds(FPlatformTime::Seconds())
    {
    }

    virtual bool Update() override
    {
        UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
        if (!PieWorld)
        {
            return WaitOrFail(TEXT("Phase 2 PIE World was not created within 120 seconds."));
        }

        LocateObjects(*PieWorld);
        if (!Rig || !MovingActor || !MovingSemantic || !BackingSemantic || !ProxyActor || !ProxySemantic)
        {
            Test->AddError(TEXT("Phase 2 test objects are missing in PIE."));
            return true;
        }

        if (!bInitialized)
        {
            if (WarmupFrames++ < 8)
            {
                return false;
            }
            MovingSemantic->SetAssignedInstanceId(MovingInstanceId, 1);
            BackingSemantic->SetAssignedInstanceId(BackingInstanceId, 1);
            ProxySemantic->SetAssignedInstanceId(ProxyInstanceId, 1);
            for (const FCameraChannelConfig& Channel : Rig->Channels)
            {
                ExpectedChannels.Add(Channel.ChannelGuid, Channel);
            }
            Test->TestEqual(TEXT("Phase 2 has eight high-resolution channels"),
                ExpectedChannels.Num(), ExpectedChannelCount);
            bInitialized = true;
        }

        // ID 分配、Primitive 注册和 GPU Scene 上传不属于同一时刻；正式首帧前保留稳定窗口。
        if (InitialSettleFrames++ < 12)
        {
            return false;
        }

        switch (Phase)
        {
        case 0:
            SubmitFrame(5201);
            InjectBackpressureResults();
            Phase = 1;
            return false;
        case 1:
            if (CollectFrame(5201, ESceneExpectation::MovingForeground, true, false))
            {
                MovingActor->SetActorLocation(FVector(300.0, 900.0, 0.0));
                Phase = 2;
                SettleFrames = 0;
            }
            return false;
        case 2:
            if (SettleFrames++ < 12) return false;
            SubmitFrame(5202);
            Phase = 3;
            return false;
        case 3:
            if (CollectFrame(5202, ESceneExpectation::BackingVisible, false, true))
            {
                MovingActor->SetActorLocation(FVector(300.0, 0.0, 0.0));
                Phase = 4;
                SettleFrames = 0;
            }
            return false;
        case 4:
            if (SettleFrames++ < 12) return false;
            SubmitFrame(5203);
            Phase = 5;
            return false;
        case 5:
            if (CollectFrame(5203, ESceneExpectation::MovingForeground, false, false))
            {
                MovingActor->SetActorLocation(FVector(300.0, 900.0, 0.0));
                ProxyActor->SetActorLocation(FVector(280.0, 0.0, 0.0));
                Phase = 6;
                SettleFrames = 0;
            }
            return false;
        case 6:
            if (SettleFrames++ < 12) return false;
            FlushRenderingCommands();
            SubmitFrame(5204);
            Phase = 7;
            return false;
        case 7:
            if (CollectFrame(5204, ESceneExpectation::ProxyLabelsBackingVisuals, false, false))
            {
                ProxySemantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::Ignore;
                ProxySemantic->ApplyCaptureConfiguration();
                Phase = 8;
                SettleFrames = 0;
            }
            return false;
        case 8:
            // D3D12 GPU Scene/SceneProxy 状态跨帧传播；严格中心像素断言前等待稳定窗口，
            // 避免把配置变更当帧与正式采集帧混为同一时刻。
            if (SettleFrames++ < 12) return false;
            FlushRenderingCommands();
            SubmitFrame(5205);
            Phase = 9;
            return false;
        case 9:
            if (CollectFrame(5205, ESceneExpectation::ProxyIgnoredBackingVisuals, false, true))
            {
                // 沿相机视线前移 1 米。中心遮挡关系不变，但深度应减小、标签投影面积应增大。
                CameraActor->SetActorLocation(InitialCameraTransform.GetLocation() + FVector(100.0, 0.0, 0.0));
                Phase = 10;
                SettleFrames = 0;
            }
            return false;
        case 10:
            if (SettleFrames++ < 12) return false;
            SubmitFrame(5206);
            Phase = 11;
            return false;
        case 11:
            if (CollectFrame(5206, ESceneExpectation::CameraMovedForward, false, true))
            {
                // 回到初始位姿后再次拍摄；标签覆盖面积应恢复，不能保留近距离的大轮廓。
                CameraActor->SetActorTransform(InitialCameraTransform);
                Phase = 12;
                SettleFrames = 0;
            }
            return false;
        case 12:
            if (SettleFrames++ < 12) return false;
            SubmitFrame(5207);
            Phase = 13;
            return false;
        case 13:
            if (CollectFrame(5207, ESceneExpectation::ProxyIgnoredBackingVisuals, false, true))
            {
                ValidateMetrics();
                return true;
            }
            return false;
        default:
            Test->AddError(TEXT("Phase 2 state machine entered an invalid phase."));
            return true;
        }
    }

private:
    void LocateObjects(UWorld& World)
    {
        if (Rig && MovingActor && ProxyActor)
        {
            return;
        }
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            if (It->ActorHasTag(CameraTag))
            {
                CameraActor = *It;
                Rig = It->FindComponentByClass<UCameraRigComponent>();
                InitialCameraTransform = It->GetActorTransform();
            }
            else if (It->ActorHasTag(MovingTag))
            {
                MovingActor = *It;
                MovingSemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
            else if (It->ActorHasTag(BackingTag))
            {
                BackingSemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
            else if (It->ActorHasTag(ProxyTag))
            {
                ProxyActor = *It;
                ProxySemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
        }
    }

    FCaptureRequest MakeRequest(const uint64 FrameId) const
    {
        FCaptureRequest Request;
        Request.Header.FrameId = FrameId;
        Request.Header.SequenceId = 2;
        Request.SensorName = Rig->SensorName;
        Request.SensorGuid = FGuid(0x520, 0x521, 0x522, 0x523);
        Request.ExpectedPayloads = Rig->GetEnabledPayloadTypes();
        Request.ExpectedImageChannels = Rig->GetEnabledImageChannels();
        return Request;
    }

    void SubmitFrame(const uint64 FrameId)
    {
        ReceivedChannels.Reset();
        const ECaptureRequestResult Result = Rig->SubmitCapture(MakeRequest(FrameId));
        Test->TestEqual(*FString::Printf(TEXT("Frame %llu is accepted"), FrameId),
            Result, ECaptureRequestResult::Accepted);
        if (Result == ECaptureRequestResult::Accepted)
        {
            ++AcceptedCount;
        }
    }

    void InjectBackpressureResults()
    {
        const ECaptureRequestResult BusyResult = Rig->SubmitCapture(MakeRequest(5299));
        Test->TestEqual(TEXT("A second eight-channel request is Busy while capacity is occupied"),
            BusyResult, ECaptureRequestResult::Busy);
        BusyCount += BusyResult == ECaptureRequestResult::Busy ? 1 : 0;

        FCaptureRequest RejectedRequest = MakeRequest(5298);
        RejectedRequest.ExpectedPayloads = EPayloadType::None;
        RejectedRequest.ExpectedImageChannels.Reset();
        const ECaptureRequestResult RejectedResult = Rig->SubmitCapture(RejectedRequest);
        Test->TestEqual(TEXT("A request without expected channels is Rejected"),
            RejectedResult, ECaptureRequestResult::Rejected);
        RejectedCount += RejectedResult == ECaptureRequestResult::Rejected ? 1 : 0;
    }

    bool CollectFrame(
        const uint64 FrameId,
        const ESceneExpectation Expectation,
        const bool bFullProtocolValidation,
        const bool bRequireMovingAbsent)
    {
        FImagePayload Payload;
        while (Rig->PollCompletedImage(Payload))
        {
            if (Payload.Header.FrameId != FrameId)
            {
                Test->AddError(FString::Printf(TEXT("Unexpected completed frame %llu while waiting for %llu."),
                    Payload.Header.FrameId, FrameId));
                continue;
            }
            if (ReceivedChannels.Contains(Payload.ChannelGuid))
            {
                Test->AddError(FString::Printf(TEXT("Frame %llu duplicated ChannelGuid %s."),
                    FrameId, *Payload.ChannelGuid.ToString()));
                continue;
            }
            const FCameraChannelConfig* Config = ExpectedChannels.Find(Payload.ChannelGuid);
            if (!Config)
            {
                Test->AddError(TEXT("Phase 2 received an unknown ChannelGuid."));
                continue;
            }
            ReceivedChannels.Add(Payload.ChannelGuid);
            ValidatePayload(Payload, *Config, Expectation, bFullProtocolValidation, bRequireMovingAbsent);
        }

        if (ReceivedChannels.Num() == ExpectedChannels.Num())
        {
            Test->TestEqual(*FString::Printf(TEXT("Frame %llu delivers eight channels exactly once"), FrameId),
                ReceivedChannels.Num(), ExpectedChannelCount);
            return true;
        }
        return WaitOrFail(TEXT("Phase 2 Payloads did not complete within 120 seconds."));
    }

    void ValidatePayload(
        const FImagePayload& Payload,
        const FCameraChannelConfig& Config,
        const ESceneExpectation Expectation,
        const bool bFullProtocolValidation,
        const bool bRequireMovingAbsent)
    {
        const EPayloadType Type = Config.ToPayloadType();
        const FString Prefix = FString::Printf(TEXT("%llu %s %dx%d"), Payload.Header.FrameId,
            *UEnum::GetValueAsString(Config.ChannelType), Config.Resolution.X, Config.Resolution.Y);

        Test->TestEqual(*FString::Printf(TEXT("%s image size"), *Prefix), Payload.ImageSize, Config.Resolution);
        Test->TestEqual(*FString::Printf(TEXT("%s ViewRect"), *Prefix), Payload.ViewRect,
            FIntRect(FIntPoint::ZeroValue, Config.Resolution));
        Test->TestEqual(*FString::Printf(TEXT("%s row stride"), *Prefix),
            Payload.RowStrideBytes, Config.Resolution.X * 4);
        Test->TestEqual(*FString::Printf(TEXT("%s byte count"), *Prefix),
            Payload.Bytes.Num(), Config.Resolution.X * Config.Resolution.Y * 4);

        if (bFullProtocolValidation)
        {
            Test->TestEqual(*FString::Printf(TEXT("%s payload type"), *Prefix), Payload.PayloadType, Type);
            Test->TestEqual(*FString::Printf(TEXT("%s pixel format"), *Prefix),
                Payload.PixelFormat, ExpectedPixelFormat(Type));
            Test->TestEqual(*FString::Printf(TEXT("%s color space"), *Prefix),
                Payload.ColorSpace, ExpectedColorSpace(Type));
            Test->TestEqual(*FString::Printf(TEXT("%s value unit"), *Prefix),
                Payload.ValueUnit, ExpectedValueUnit(Type));
            Test->TestEqual(*FString::Printf(TEXT("%s bytes per pixel"), *Prefix), Payload.BytesPerPixel, 4);
        }

        const int32 Offset = CenterOffset(Payload);
        if (Offset < 0 || Offset + 3 >= Payload.Bytes.Num())
        {
            Test->AddError(FString::Printf(TEXT("%s has no center pixel."), *Prefix));
            return;
        }

        if (Type == EPayloadType::Rgb)
        {
            const uint8 R = Payload.Bytes[Offset];
            const uint8 G = Payload.Bytes[Offset + 1];
            const uint8 B = Payload.Bytes[Offset + 2];
            if (bFullProtocolValidation)
            {
                Test->TestTrue(*FString::Printf(TEXT("%s static center contains RGB color"), *Prefix),
                    R != 0 || G != 0 || B != 0);
            }
            Test->TestEqual(*FString::Printf(TEXT("%s alpha is opaque"), *Prefix),
                Payload.Bytes[Offset + 3], uint8{255});
            if (Expectation == ESceneExpectation::ProxyLabelsBackingVisuals ||
                Expectation == ESceneExpectation::ProxyIgnoredBackingVisuals)
            {
                Test->TestFalse(*FString::Printf(TEXT("%s red label proxy is absent from RGB"), *Prefix),
                    R > 80 && R > static_cast<int32>(G) + 30);
            }
        }
        else if (Type == EPayloadType::Depth)
        {
            float DepthMeters = 0.0f;
            FMemory::Memcpy(&DepthMeters, Payload.Bytes.GetData() + Offset, sizeof(float));
            Test->TestTrue(*FString::Printf(TEXT("%s center depth is finite"), *Prefix),
                FMath::IsFinite(DepthMeters));
            const bool bExpectNearDepth = Expectation == ESceneExpectation::MovingForeground ||
                Expectation == ESceneExpectation::CameraMovedForward;
            Test->TestTrue(*FString::Printf(TEXT("%s center depth follows current camera/object pose"), *Prefix),
                bExpectNearDepth ? DepthMeters > 1.8f && DepthMeters < 3.2f : DepthMeters > 3.2f);
        }
        else if (Type == EPayloadType::Semantic)
        {
            const uint8 ExpectedCenter = Expectation == ESceneExpectation::MovingForeground
                ? MovingSemanticId
                : Expectation == ESceneExpectation::ProxyLabelsBackingVisuals
                    ? ProxySemanticId
                    : BackingSemanticId;
            Test->TestEqual(*FString::Printf(TEXT("%s semantic center"), *Prefix),
                Payload.Bytes[Offset], ExpectedCenter);

            bool bOnlyLegal = true;
            bool bMovingPresent = false;
            int32 ExpectedPixelCount = 0;
            FIntPoint ExpectedMin(MAX_int32, MAX_int32);
            FIntPoint ExpectedMax(MIN_int32, MIN_int32);
            for (int32 PixelOffset = 0; PixelOffset + 3 < Payload.Bytes.Num(); PixelOffset += 4)
            {
                const uint8 Label = Payload.Bytes[PixelOffset];
                ExpectedPixelCount += Label == ExpectedCenter ? 1 : 0;
                bMovingPresent |= Label == MovingSemanticId;
                const bool bLegalLabel = Expectation == ESceneExpectation::MovingForeground
                    ? Label == 0 || Label == MovingSemanticId || Label == BackingSemanticId
                    : Expectation == ESceneExpectation::ProxyLabelsBackingVisuals
                        ? Label == 0 || Label == ProxySemanticId || Label == BackingSemanticId
                        : Label == 0 || Label == BackingSemanticId;
                bOnlyLegal &= bLegalLabel && Payload.Bytes[PixelOffset + 1] == 0 &&
                    Payload.Bytes[PixelOffset + 2] == 0 && Payload.Bytes[PixelOffset + 3] == 255;
            }
            Test->TestTrue(*FString::Printf(TEXT("%s semantic pixels are legal"), *Prefix), bOnlyLegal);
            ValidateCameraMotionCoverage(Payload, Type, ExpectedPixelCount, Prefix);
            if (bRequireMovingAbsent)
            {
                Test->TestFalse(*FString::Printf(TEXT("%s has no moving-label ghost"), *Prefix), bMovingPresent);
            }
        }
        else if (Type == EPayloadType::Instance)
        {
            const uint32 ExpectedCenter = Expectation == ESceneExpectation::MovingForeground
                ? MovingInstanceId
                : Expectation == ESceneExpectation::ProxyLabelsBackingVisuals
                    ? ProxyInstanceId
                    : BackingInstanceId;
            uint32 CenterId = 0;
            FMemory::Memcpy(&CenterId, Payload.Bytes.GetData() + Offset, sizeof(uint32));
            if (CenterId != ExpectedCenter)
            {
                Test->AddInfo(FString::Printf(TEXT("%s instance center diagnostic: expected=%u actual=%u"),
                    *Prefix, ExpectedCenter, CenterId));
            }
            Test->TestEqual(*FString::Printf(TEXT("%s instance center"), *Prefix), CenterId, ExpectedCenter);

            bool bOnlyLegal = true;
            bool bMovingPresent = false;
            int32 ExpectedPixelCount = 0;
            FIntPoint ExpectedMin(MAX_int32, MAX_int32);
            FIntPoint ExpectedMax(MIN_int32, MIN_int32);
            for (int32 PixelOffset = 0; PixelOffset + 3 < Payload.Bytes.Num(); PixelOffset += 4)
            {
                uint32 Id = 0;
                FMemory::Memcpy(&Id, Payload.Bytes.GetData() + PixelOffset, sizeof(uint32));
                if (Id == ExpectedCenter)
                {
                    const int32 PixelIndex = PixelOffset / 4;
                    const FIntPoint Pixel(PixelIndex % Payload.ImageSize.X, PixelIndex / Payload.ImageSize.X);
                    ++ExpectedPixelCount;
                    ExpectedMin.X = FMath::Min(ExpectedMin.X, Pixel.X);
                    ExpectedMin.Y = FMath::Min(ExpectedMin.Y, Pixel.Y);
                    ExpectedMax.X = FMath::Max(ExpectedMax.X, Pixel.X);
                    ExpectedMax.Y = FMath::Max(ExpectedMax.Y, Pixel.Y);
                }
                bMovingPresent |= Id == MovingInstanceId;
                bOnlyLegal &= Expectation == ESceneExpectation::MovingForeground
                    ? Id == 0 || Id == MovingInstanceId || Id == BackingInstanceId
                    : Expectation == ESceneExpectation::ProxyLabelsBackingVisuals
                        ? Id == 0 || Id == ProxyInstanceId || Id == BackingInstanceId
                        : Id == 0 || Id == BackingInstanceId;
            }
            if (CenterId != ExpectedCenter)
            {
                Test->AddInfo(FString::Printf(
                    TEXT("%s expected instance coverage: pixels=%d bounds=[(%d,%d)-(%d,%d)]"),
                    *Prefix, ExpectedPixelCount,
                    ExpectedMin.X, ExpectedMin.Y, ExpectedMax.X, ExpectedMax.Y));
            }
            Test->TestTrue(*FString::Printf(TEXT("%s instance pixels are legal"), *Prefix), bOnlyLegal);
            ValidateCameraMotionCoverage(Payload, Type, ExpectedPixelCount, Prefix);
            if (bRequireMovingAbsent)
            {
                Test->TestFalse(*FString::Printf(TEXT("%s has no moving-instance ghost"), *Prefix), bMovingPresent);
            }
        }
    }

    /**
     * 记录相机移动前的 Backing 标签面积，并验证前移后放大、复位后恢复。
     * 标签面积同时覆盖 Semantic 与 Instance，可发现相机 View 没有更新或旧轮廓残留。
     */
    void ValidateCameraMotionCoverage(
        const FImagePayload& Payload,
        const EPayloadType Type,
        const int32 ExpectedPixelCount,
        const FString& Prefix)
    {
        const FString Key = FString::Printf(TEXT("%u:%dx%d"), static_cast<uint8>(Type),
            Payload.ImageSize.X, Payload.ImageSize.Y);
        if (Payload.Header.FrameId == 5205)
        {
            CameraBaselineCoverage.Add(Key, ExpectedPixelCount);
            Test->TestTrue(*FString::Printf(TEXT("%s camera baseline has label coverage"), *Prefix),
                ExpectedPixelCount > 0);
            return;
        }

        const int32* Baseline = CameraBaselineCoverage.Find(Key);
        if (!Baseline)
        {
            return;
        }
        if (Payload.Header.FrameId == 5206)
        {
            Test->TestTrue(*FString::Printf(TEXT("%s camera forward motion enlarges label coverage"), *Prefix),
                ExpectedPixelCount > *Baseline);
        }
        else if (Payload.Header.FrameId == 5207)
        {
            const int32 Tolerance = FMath::Max(8, *Baseline / 100);
            Test->TestTrue(*FString::Printf(TEXT("%s camera reset removes enlarged-label history"), *Prefix),
                FMath::Abs(ExpectedPixelCount - *Baseline) <= Tolerance);
        }
    }

    void ValidateMetrics()
    {
        const FImageReadbackStats Stats = Rig->GetImageReadbackStats();
        const TArray<FImageReadbackChannelStats> ChannelStats = Rig->GetImageReadbackChannelStats();
        int64 DeliveredCount = 0;
        double AverageGpuLatencyMs = 0.0;
        double MaxGpuLatencyMs = 0.0;
        double AverageDeliveryLatencyMs = 0.0;
        double MaxDeliveryLatencyMs = 0.0;

        Test->TestEqual(TEXT("Phase 2 accepted capture count"), AcceptedCount, ExpectedAcceptedCount);
        Test->TestEqual(TEXT("Phase 2 Busy count"), BusyCount, 1);
        Test->TestEqual(TEXT("Phase 2 Rejected count"), RejectedCount, 1);
        Test->TestEqual(TEXT("Readback capacity equals one complete eight-channel frame"),
            Stats.Capacity, ExpectedChannelCount);
        Test->TestEqual(TEXT("All Readbacks are consumed"), Stats.PendingCount, 0);
        Test->TestEqual(TEXT("Seven frames enqueue fifty-six channel Readbacks"),
            Stats.EnqueuedCount, int64{ExpectedAcceptedCount * ExpectedChannelCount});
        Test->TestEqual(TEXT("Every enqueued Readback completes"), Stats.CompletedCount, Stats.EnqueuedCount);
        Test->TestEqual(TEXT("No manager-level Readback rejection"), Stats.RejectedCount, int64{0});
        Test->TestEqual(TEXT("No Readback conversion failure"), Stats.FailedCount, int64{0});
        Test->TestTrue(TEXT("Readback resources are reused across frames"), Stats.ReusedReadbackResources > 0);
        Test->TestTrue(TEXT("Readback peak never exceeds capacity"),
            Stats.PeakPendingCount > 0 && Stats.PeakPendingCount <= Stats.Capacity);
        Test->TestEqual(TEXT("Metrics contain all eight ChannelGuids"),
            ChannelStats.Num(), ExpectedChannelCount);

        for (const FImageReadbackChannelStats& Channel : ChannelStats)
        {
            Test->TestEqual(TEXT("Each channel is enqueued seven times"),
                Channel.EnqueuedCount, int64{ExpectedAcceptedCount});
            Test->TestEqual(TEXT("Each channel completes seven times"),
                Channel.CompletedCount, int64{ExpectedAcceptedCount});
            Test->TestEqual(TEXT("Each channel is delivered seven times"),
                Channel.DeliveredCount, int64{ExpectedAcceptedCount});
            Test->TestEqual(TEXT("Each channel has no failures"), Channel.FailedCount, int64{0});
            Test->TestTrue(TEXT("GPU latency maximum is not below average"),
                Channel.MaxGpuLatencyMs >= Channel.AverageGpuLatencyMs && Channel.AverageGpuLatencyMs >= 0.0);
            Test->TestTrue(TEXT("Delivery latency maximum is not below average"),
                Channel.MaxDeliveryLatencyMs >= Channel.AverageDeliveryLatencyMs &&
                Channel.AverageDeliveryLatencyMs >= 0.0);
            DeliveredCount += Channel.DeliveredCount;
            AverageGpuLatencyMs += Channel.AverageGpuLatencyMs;
            MaxGpuLatencyMs = FMath::Max(MaxGpuLatencyMs, Channel.MaxGpuLatencyMs);
            AverageDeliveryLatencyMs += Channel.AverageDeliveryLatencyMs;
            MaxDeliveryLatencyMs = FMath::Max(MaxDeliveryLatencyMs, Channel.MaxDeliveryLatencyMs);
        }
        if (ChannelStats.Num() > 0)
        {
            AverageGpuLatencyMs /= ChannelStats.Num();
            AverageDeliveryLatencyMs /= ChannelStats.Num();
        }

        UE_LOG(LogTemp, Display,
            TEXT("PHASE2_METRICS Accepted=%d Busy=%d Rejected=%d Enqueued=%lld Completed=%lld Delivered=%lld PeakPending=%d Capacity=%d Created=%lld Reused=%lld AvgGpuMs=%.3f MaxGpuMs=%.3f AvgDeliveryMs=%.3f MaxDeliveryMs=%.3f"),
            AcceptedCount, BusyCount, RejectedCount, Stats.EnqueuedCount, Stats.CompletedCount,
            DeliveredCount, Stats.PeakPendingCount, Stats.Capacity, Stats.CreatedReadbackResources,
            Stats.ReusedReadbackResources, AverageGpuLatencyMs, MaxGpuLatencyMs,
            AverageDeliveryLatencyMs, MaxDeliveryLatencyMs);
    }

    bool WaitOrFail(const TCHAR* Error)
    {
        if (FPlatformTime::Seconds() - StartSeconds < 120.0)
        {
            return false;
        }
        Test->AddError(Error);
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double StartSeconds = 0.0;
    int32 WarmupFrames = 0;
    int32 InitialSettleFrames = 0;
    int32 SettleFrames = 0;
    int32 Phase = 0;
    int32 AcceptedCount = 0;
    int32 BusyCount = 0;
    int32 RejectedCount = 0;
    bool bInitialized = false;
    TObjectPtr<AActor> CameraActor = nullptr;
    FTransform InitialCameraTransform = FTransform::Identity;
    TObjectPtr<UCameraRigComponent> Rig = nullptr;
    TObjectPtr<AActor> MovingActor = nullptr;
    TObjectPtr<USemanticObjectComponent> MovingSemantic = nullptr;
    TObjectPtr<USemanticObjectComponent> BackingSemantic = nullptr;
    TObjectPtr<AActor> ProxyActor = nullptr;
    TObjectPtr<USemanticObjectComponent> ProxySemantic = nullptr;
    TMap<FGuid, FCameraChannelConfig> ExpectedChannels;
    TSet<FGuid> ReceivedChannels;
    TMap<FString, int32> CameraBaselineCoverage;
};

class FFlushPhase2Command final : public IAutomationLatentCommand
{
public:
    virtual bool Update() override
    {
        FlushRenderingCommands();
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRenderOutputMatrixPhase2Test,
    "SensorSimulation.Rendering.OutputMatrix.Phase2.HighResolutionMotionOcclusion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderOutputMatrixPhase2Test::RunTest(const FString& Parameters)
{
    using namespace UE::SensorSimulation::RenderOutputMatrixPhase2Tests;

    AddExpectedError(TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A phase 2 output-matrix World is created"), World))
    {
        return false;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

    AActor* CameraActor = World->SpawnActor<AActor>();
    CameraActor->Tags.Add(CameraTag);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(CameraActor, TEXT("RendererPhase2Rig"));
    CameraActor->SetRootComponent(Rig);
    CameraActor->AddInstanceComponent(Rig);
    Rig->SensorName = TEXT("RendererPhase2Camera");
    Rig->HorizontalFovDegrees = 90.0f;
    Rig->MaxPendingReadbacks = ExpectedChannelCount;
    Rig->Channels.Empty();
    for (const FIntPoint Resolution : { FIntPoint(640, 480), FIntPoint(1280, 720) })
    {
        for (const ECameraChannelType Type : { ECameraChannelType::Rgb, ECameraChannelType::Semantic,
            ECameraChannelType::Depth, ECameraChannelType::Instance })
        {
            FCameraChannelConfig& Channel = Rig->Channels.AddDefaulted_GetRef();
            Channel.ChannelType = Type;
            Channel.Resolution = Resolution;
            Channel.bForceLinearGamma = Type == ECameraChannelType::Semantic || Type == ECameraChannelType::Instance;
        }
    }
    Rig->RegisterComponent();
    TestEqual(TEXT("Eight high-resolution runtime channels are active"),
        Rig->GetEnabledImageChannels().Num(), ExpectedChannelCount);

    AActor* LightActor = World->SpawnActor<AActor>();
    UPointLightComponent* Light = NewObject<UPointLightComponent>(LightActor, TEXT("RendererPhase2Light"));
    LightActor->SetRootComponent(Light);
    LightActor->AddInstanceComponent(Light);
    Light->SetIntensity(6000.0f);
    Light->SetAttenuationRadius(1200.0f);
    Light->RegisterComponent();

    AActor* BackingActor = World->SpawnActor<AActor>();
    BackingActor->Tags.Add(BackingTag);
    UStaticMeshComponent* BackingMesh = NewObject<UStaticMeshComponent>(BackingActor, TEXT("RendererPhase2BackingMesh"));
    BackingActor->SetRootComponent(BackingMesh);
    BackingActor->AddInstanceComponent(BackingMesh);
    BackingMesh->SetStaticMesh(Cube);
    BackingMesh->SetMaterial(0, CreateColoredMaterial(BackingActor, FLinearColor::Green));
    BackingMesh->SetWorldScale3D(FVector(2.0));
    BackingMesh->RegisterComponent();
    BackingActor->SetActorLocation(FVector(450.0, 0.0, 0.0));
    USemanticObjectComponent* BackingSemantic =
        NewObject<USemanticObjectComponent>(BackingActor, TEXT("RendererPhase2BackingSemantic"));
    BackingSemantic->SemanticId = BackingSemanticId;
    BackingActor->AddInstanceComponent(BackingSemantic);
    BackingSemantic->RegisterComponent();

    AActor* MovingActor = World->SpawnActor<AActor>();
    MovingActor->Tags.Add(MovingTag);
    UStaticMeshComponent* MovingMesh = NewObject<UStaticMeshComponent>(MovingActor, TEXT("RendererPhase2MovingMesh"));
    MovingActor->SetRootComponent(MovingMesh);
    MovingActor->AddInstanceComponent(MovingMesh);
    MovingMesh->SetStaticMesh(Cube);
    MovingMesh->SetMaterial(0, CreateColoredMaterial(MovingActor, FLinearColor::Blue));
    MovingMesh->SetWorldScale3D(FVector(1.5));
    MovingMesh->RegisterComponent();
    MovingActor->SetActorLocation(FVector(300.0, 0.0, 0.0));
    USemanticObjectComponent* MovingSemantic =
        NewObject<USemanticObjectComponent>(MovingActor, TEXT("RendererPhase2MovingSemantic"));
    MovingSemantic->SemanticId = MovingSemanticId;
    MovingActor->AddInstanceComponent(MovingSemantic);
    MovingSemantic->RegisterComponent();

    UMaterial* Translucent = CreateTranslucentMaterial();
    AActor* ProxyActor = World->SpawnActor<AActor>();
    ProxyActor->Tags.Add(ProxyTag);
    UStaticMeshComponent* SourceMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("RendererPhase2Glass"));
    ProxyActor->SetRootComponent(SourceMesh);
    ProxyActor->AddInstanceComponent(SourceMesh);
    SourceMesh->SetStaticMesh(Cube);
    SourceMesh->SetMaterial(0, Translucent);
    SourceMesh->SetWorldScale3D(FVector(2.0));
    SourceMesh->RegisterComponent();
    UStaticMeshComponent* ProxyMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("RendererPhase2LabelProxy"));
    ProxyMesh->SetupAttachment(SourceMesh);
    ProxyActor->AddInstanceComponent(ProxyMesh);
    ProxyMesh->SetStaticMesh(Cube);
    ProxyMesh->SetMaterial(0, CreateColoredMaterial(ProxyActor, FLinearColor::Red));
    ProxyMesh->SetWorldScale3D(FVector(1.0));
    ProxyMesh->RegisterComponent();
    ProxyActor->SetActorLocation(FVector(280.0, 900.0, 0.0));
    USemanticObjectComponent* ProxySemantic =
        NewObject<USemanticObjectComponent>(ProxyActor, TEXT("RendererPhase2ProxySemantic"));
    ProxySemantic->SemanticId = ProxySemanticId;
    ProxySemantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::OpaqueProxy;
    ProxySemantic->OpaqueLabelProxy = ProxyMesh;
    ProxyActor->AddInstanceComponent(ProxySemantic);
    ProxySemantic->RegisterComponent();

    ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FCapturePhase2MatrixCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFlushPhase2Command());
    return true;
}

#endif
