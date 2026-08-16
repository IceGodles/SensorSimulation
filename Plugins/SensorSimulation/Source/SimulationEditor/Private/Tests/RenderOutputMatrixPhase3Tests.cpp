#include "CameraRigComponent.h"
#include "ImageReadbackManager.h"
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
#include "Math/RotationMatrix.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::RenderOutputMatrixPhase3Tests
{
static constexpr int32 RigCount = 4;
static constexpr int32 ModalitiesPerRig = 4;
static constexpr int32 MotionFrameCount = 6;
static constexpr double SampleStepSeconds = 0.05;
static constexpr uint8 MovingSemanticId = 81;
static constexpr uint32 MovingInstanceId = 0x01061000u;
static const FName MovingActorTag(TEXT("RendererPhase3MovingActor"));

/** 四个相机分别从目标的前、后、左、右侧观察，名称同时作为路由断言的可读身份。 */
static const FName RigNames[RigCount] = {
    TEXT("FrontCamera"), TEXT("RearCamera"), TEXT("LeftCamera"), TEXT("RightCamera")
};

static const FName RigTags[RigCount] = {
    TEXT("RendererPhase3Front"), TEXT("RendererPhase3Rear"),
    TEXT("RendererPhase3Left"), TEXT("RendererPhase3Right")
};

static const FVector BaseCameraLocations[RigCount] = {
    FVector(-600.0, 0.0, 0.0), FVector(600.0, 0.0, 0.0),
    FVector(0.0, -600.0, 0.0), FVector(0.0, 600.0, 0.0)
};

/** 返回测试中固定且互不重复的传感器身份。 */
FGuid SensorGuidForRig(const int32 RigIndex)
{
    return FGuid(0x53000000u + static_cast<uint32>(RigIndex), 0x01u, 0x02u, 0x03u);
}

/** 返回跨 Rig、跨模态都唯一的稳定通道身份。 */
FGuid ChannelGuidFor(const int32 RigIndex, const int32 ModalityIndex)
{
    return FGuid(
        0x53100000u + static_cast<uint32>(RigIndex),
        0x100u + static_cast<uint32>(ModalityIndex),
        0x04u,
        0x05u);
}

/** 创建用于 RGB 对齐探针的高亮材质。 */
UMaterialInstanceDynamic* CreateMovingMaterial(UObject* Outer)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(
        nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Outer);
    if (Material)
    {
        Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.75f, 0.1f, 1.0f));
    }
    return Material;
}

/** 单个 Rig 在当前 FrameId 下收集到的四模态结果。 */
struct FRigFramePayloads
{
    TMap<EPayloadType, FImagePayload> ByType;
    TSet<FGuid> ChannelGuids;
};

/**
 * 四方向多 Rig 并发与相机/Actor 联合连续运动验收。
 *
 * 每个 0.05 秒逻辑采样步同时改变相机组和目标位姿，不等待额外 Scene 稳定帧；随后四个
 * Rig 在同一 FrameId 下提交四模态捕获。Semantic/Instance 掩码必须逐像素一致，标签
 * 质心处的 RGB 与 Depth 必须有效，从而验证同一 Rig 内四模态使用同一运动快照。
 */
class FCapturePhase3Command final : public IAutomationLatentCommand
{
public:
    explicit FCapturePhase3Command(FAutomationTestBase* InTest)
        : Test(InTest), StartSeconds(FPlatformTime::Seconds())
    {
        RigPayloads.SetNum(RigCount);
        ExpectedChannels.SetNum(RigCount);
        CentroidHistory.SetNum(RigCount);
    }

    virtual bool Update() override
    {
        UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
        if (!PieWorld)
        {
            return WaitOrFail(TEXT("Phase 3 PIE World was not created within 180 seconds."));
        }

        LocateObjects(*PieWorld);
        if (!MovingActor || !MovingSemantic || Rigs.Num() != RigCount || CameraActors.Num() != RigCount)
        {
            Test->AddError(TEXT("Phase 3 moving Actor or one of the four Camera Rigs is missing in PIE."));
            return true;
        }

        if (!bInitialized)
        {
            if (WarmupFrames++ < 12)
            {
                return false;
            }
            MovingSemantic->SetAssignedInstanceId(MovingInstanceId, 1);
            InitializeRoutingExpectations();
            bInitialized = true;
            InitialSettleFrames = 0;
        }

        // 仅首帧为 InstanceId 注册和 GPU Scene 上传保留稳定窗口；后续运动采样不再等待。
        if (InitialSettleFrames++ < 12)
        {
            return false;
        }

        if (!bCaptureSubmitted)
        {
            ApplyMotionAndSubmitFrame();
            bCaptureSubmitted = true;
            return false;
        }

        if (!bGlobalPumpProbeComplete)
        {
            ProbeGlobalPumpBatching();
            bGlobalPumpProbeComplete = true;
        }

        PollAllRigs();
        if (!IsCurrentFrameComplete())
        {
            return WaitOrFail(TEXT("Phase 3 Payloads did not complete within 180 seconds."));
        }

        ValidateCurrentFrame();
        if (MotionStep + 1 < MotionFrameCount)
        {
            ++MotionStep;
            ResetCurrentFrame();
            // 不插入稳定帧：下一次 Update 立即改变相机与 Actor，并提交下一逻辑采样。
            ApplyMotionAndSubmitFrame();
            return false;
        }

        ValidateFinalMetrics();
        return true;
    }

private:
    /** 从 PIE 世界按稳定 Tag 找回四个相机和运动目标。 */
    void LocateObjects(UWorld& World)
    {
        if (MovingActor && Rigs.Num() == RigCount)
        {
            return;
        }

        TArray<UCameraRigComponent*> LocatedRigs;
        TArray<AActor*> LocatedActors;
        LocatedRigs.SetNumZeroed(RigCount);
        LocatedActors.SetNumZeroed(RigCount);
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            if (It->ActorHasTag(MovingActorTag))
            {
                MovingActor = *It;
                MovingSemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
            for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
            {
                if (It->ActorHasTag(RigTags[RigIndex]))
                {
                    LocatedActors[RigIndex] = *It;
                    LocatedRigs[RigIndex] = It->FindComponentByClass<UCameraRigComponent>();
                }
            }
        }

        if (!LocatedRigs.Contains(nullptr) && !LocatedActors.Contains(nullptr))
        {
            Rigs = MoveTemp(LocatedRigs);
            CameraActors = MoveTemp(LocatedActors);
        }
    }

    /** 验证四 Rig 的 SensorGuid/ChannelGuid 配方互不覆盖。 */
    void InitializeRoutingExpectations()
    {
        TSet<FGuid> AllChannelGuids;
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            Test->TestEqual(*FString::Printf(TEXT("%s has four active modalities"), *RigNames[RigIndex].ToString()),
                Rigs[RigIndex]->GetEnabledImageChannels().Num(), ModalitiesPerRig);
            Test->TestEqual(*FString::Printf(TEXT("%s keeps its stable sensor name"), *RigNames[RigIndex].ToString()),
                Rigs[RigIndex]->SensorName, RigNames[RigIndex]);

            for (const FCameraChannelConfig& Config : Rigs[RigIndex]->Channels)
            {
                Test->TestTrue(TEXT("Every Phase 3 ChannelGuid is valid"), Config.ChannelGuid.IsValid());
                Test->TestFalse(TEXT("ChannelGuid is globally unique across four rigs"),
                    AllChannelGuids.Contains(Config.ChannelGuid));
                AllChannelGuids.Add(Config.ChannelGuid);
                ExpectedChannels[RigIndex].Add(Config.ChannelGuid, Config.ToPayloadType());
            }
        }
        Test->TestEqual(TEXT("Four rigs expose sixteen globally unique channels"),
            AllChannelGuids.Num(), RigCount * ModalitiesPerRig);
    }

    /** 生成四个 Rig 共享 FrameId/时间戳、但保持独立传感器身份的请求。 */
    FCaptureRequest MakeRequest(const int32 RigIndex, const uint64 FrameId) const
    {
        FCaptureRequest Request;
        Request.Header.SequenceId = 3;
        Request.Header.FrameId = FrameId;
        Request.Header.SimulationTimestampSeconds = MotionStep * SampleStepSeconds;
        Request.SensorName = RigNames[RigIndex];
        Request.SensorGuid = SensorGuidForRig(RigIndex);
        Request.ExpectedPayloads = Rigs[RigIndex]->GetEnabledPayloadTypes();
        Request.ExpectedImageChannels = Rigs[RigIndex]->GetEnabledImageChannels();
        return Request;
    }

    /**
     * 以 20 Hz 逻辑时间推进一段高速轨迹：相机组和目标使用不同速度同时移动。
     * 每步约 0.05 秒，CameraDelta=(25,-20) cm、ActorDelta=(-15,30) cm，二者相对速度
     * 足以让四个视角中的标签质心连续改变。
     */
    void ApplyMotionAndSubmitFrame()
    {
        const FVector CameraDelta(
            25.0 * static_cast<double>(MotionStep),
            -20.0 * static_cast<double>(MotionStep),
            0.0);
        const FVector ActorLocation(
            -15.0 * static_cast<double>(MotionStep),
            30.0 * static_cast<double>(MotionStep),
            0.0);
        MovingActor->SetActorLocation(ActorLocation);

        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            CameraActors[RigIndex]->SetActorLocation(BaseCameraLocations[RigIndex] + CameraDelta);
        }

        CurrentFrameId = 5300u + static_cast<uint64>(MotionStep);
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const FCaptureRequest Request = MakeRequest(RigIndex, CurrentFrameId);
            const ECaptureRequestResult Accepted = Rigs[RigIndex]->SubmitCapture(Request);
            Test->TestEqual(*FString::Printf(TEXT("Frame %llu %s is Accepted"),
                CurrentFrameId, *RigNames[RigIndex].ToString()), Accepted, ECaptureRequestResult::Accepted);
            AcceptedCount += Accepted == ECaptureRequestResult::Accepted ? 1 : 0;

            // 每个 Manager 容量恰好等于本 Rig 的四模态；在途帧未消费时重复请求必须原子 Busy，
            // 不允许出现只提交了部分通道的半帧。
            const ECaptureRequestResult Busy = Rigs[RigIndex]->SubmitCapture(
                MakeRequest(RigIndex, 5390u + static_cast<uint64>(MotionStep)));
            Test->TestEqual(*FString::Printf(TEXT("Frame %llu %s duplicate request is Busy"),
                CurrentFrameId, *RigNames[RigIndex].ToString()), Busy, ECaptureRequestResult::Busy);
            BusyCount += Busy == ECaptureRequestResult::Busy ? 1 : 0;
        }
    }

    /** 证明任一 Manager 的一次 Poll 会用一条渲染命令批量推进全部活跃 Manager。 */
    void ProbeGlobalPumpBatching()
    {
        GlobalPumpBefore = FImageReadbackManager::GetGlobalPumpStatsForTesting();
        Test->TestTrue(TEXT("At least four Readback Managers are registered"),
            GlobalPumpBefore.RegisteredManagerCount >= RigCount);

        FImagePayload EarlyPayload;
        if (Rigs[0]->PollCompletedImage(EarlyPayload))
        {
            AcceptPayload(MoveTemp(EarlyPayload));
        }
        FlushRenderingCommands();

        const FImageReadbackGlobalPumpStats After =
            FImageReadbackManager::GetGlobalPumpStatsForTesting();
        Test->TestEqual(TEXT("One Poll submits exactly one global Pump command"),
            After.PumpCommandCount - GlobalPumpBefore.PumpCommandCount, int64{1});
        Test->TestEqual(TEXT("The global Pump visits every registered Manager in one snapshot"),
            After.PumpedManagerCount - GlobalPumpBefore.PumpedManagerCount,
            static_cast<int64>(GlobalPumpBefore.RegisteredManagerCount));
        Test->TestTrue(TEXT("Global Pump batch contains all four Phase 3 Managers"),
            After.PeakManagersPerPump >= RigCount);
    }

    /** 非阻塞轮询四个 Manager，并按 SensorGuid + ChannelGuid 路由 Payload。 */
    void PollAllRigs()
    {
        for (UCameraRigComponent* Rig : Rigs)
        {
            FImagePayload Payload;
            while (Rig->PollCompletedImage(Payload))
            {
                AcceptPayload(MoveTemp(Payload));
                Payload = FImagePayload();
            }
        }
    }

    /** 校验传感器与通道身份后，将载荷放入对应 Rig 的当前帧集合。 */
    void AcceptPayload(FImagePayload&& Payload)
    {
        int32 RigIndex = INDEX_NONE;
        for (int32 Candidate = 0; Candidate < RigCount; ++Candidate)
        {
            if (Payload.SensorGuid == SensorGuidForRig(Candidate))
            {
                RigIndex = Candidate;
                break;
            }
        }
        if (RigIndex == INDEX_NONE)
        {
            Test->AddError(TEXT("Phase 3 received a Payload with an unknown SensorGuid."));
            return;
        }

        Test->TestEqual(TEXT("Payload keeps the submitted FrameId"), Payload.Header.FrameId, CurrentFrameId);
        Test->TestEqual(TEXT("Payload keeps the shared simulation timestamp"),
            Payload.Header.SimulationTimestampSeconds, MotionStep * SampleStepSeconds);
        Test->TestEqual(TEXT("Payload keeps the owning Rig name"), Payload.SensorName, RigNames[RigIndex]);

        const EPayloadType* ExpectedType = ExpectedChannels[RigIndex].Find(Payload.ChannelGuid);
        if (!ExpectedType)
        {
            Test->AddError(FString::Printf(TEXT("%s received an unknown ChannelGuid %s."),
                *RigNames[RigIndex].ToString(), *Payload.ChannelGuid.ToString()));
            return;
        }
        Test->TestEqual(TEXT("ChannelGuid routes to the expected modality"), Payload.PayloadType, *ExpectedType);
        Test->TestFalse(TEXT("A Rig never receives the same ChannelGuid twice in one frame"),
            RigPayloads[RigIndex].ChannelGuids.Contains(Payload.ChannelGuid));
        Test->TestFalse(TEXT("A Rig never receives the same modality twice in one frame"),
            RigPayloads[RigIndex].ByType.Contains(Payload.PayloadType));
        RigPayloads[RigIndex].ChannelGuids.Add(Payload.ChannelGuid);
        RigPayloads[RigIndex].ByType.Add(Payload.PayloadType, MoveTemp(Payload));
    }

    bool IsCurrentFrameComplete() const
    {
        for (const FRigFramePayloads& Payloads : RigPayloads)
        {
            if (Payloads.ByType.Num() != ModalitiesPerRig ||
                Payloads.ChannelGuids.Num() != ModalitiesPerRig)
            {
                return false;
            }
        }
        return true;
    }

    /** 对四个方向分别执行四模态运动快照对齐检查。 */
    void ValidateCurrentFrame()
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const FImagePayload& Rgb = RigPayloads[RigIndex].ByType.FindChecked(EPayloadType::Rgb);
            const FImagePayload& Semantic = RigPayloads[RigIndex].ByType.FindChecked(EPayloadType::Semantic);
            const FImagePayload& Depth = RigPayloads[RigIndex].ByType.FindChecked(EPayloadType::Depth);
            const FImagePayload& Instance = RigPayloads[RigIndex].ByType.FindChecked(EPayloadType::Instance);
            ValidateAlignedModalities(RigIndex, Rgb, Semantic, Depth, Instance);
        }
    }

    /**
     * Semantic 与 Instance 必须形成完全相同的离散掩码；其质心位置同时作为 RGB/Depth
     * 的跨模态探针。该检查对每个方向、每个运动采样独立执行。
     */
    void ValidateAlignedModalities(
        const int32 RigIndex,
        const FImagePayload& Rgb,
        const FImagePayload& Semantic,
        const FImagePayload& Depth,
        const FImagePayload& Instance)
    {
        const FString Prefix = FString::Printf(TEXT("Frame %llu %s"),
            CurrentFrameId, *RigNames[RigIndex].ToString());
        const FIntPoint ExpectedSize(320, 240);
        for (const FImagePayload* Payload : { &Rgb, &Semantic, &Depth, &Instance })
        {
            Test->TestEqual(*FString::Printf(TEXT("%s image size"), *Prefix),
                Payload->ImageSize, ExpectedSize);
            Test->TestEqual(*FString::Printf(TEXT("%s full ViewRect"), *Prefix),
                Payload->ViewRect, FIntRect(FIntPoint::ZeroValue, ExpectedSize));
            Test->TestEqual(*FString::Printf(TEXT("%s tight row stride"), *Prefix),
                Payload->RowStrideBytes, ExpectedSize.X * 4);
        }

        int32 SemanticPixels = 0;
        int32 InstancePixels = 0;
        int32 MaskMismatchPixels = 0;
        int32 RgbVisiblePixels = 0;
        int32 RgbSemanticIntersection = 0;
        int32 NearDepthPixels = 0;
        int32 DepthSemanticIntersection = 0;
        int64 SumX = 0;
        int64 SumY = 0;
        const int32 PixelCount = ExpectedSize.X * ExpectedSize.Y;
        for (int32 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
        {
            const int32 Offset = PixelIndex * 4;
            const uint8 Label = Semantic.Bytes[Offset];
            uint32 InstanceId = 0;
            FMemory::Memcpy(&InstanceId, Instance.Bytes.GetData() + Offset, sizeof(uint32));

            const bool bSemanticForeground = Label == MovingSemanticId;
            const bool bInstanceForeground = InstanceId == MovingInstanceId;
            const bool bSemanticLegal = Label == 0 || bSemanticForeground;
            const bool bInstanceLegal = InstanceId == 0 || bInstanceForeground;
            if (!bSemanticLegal || !bInstanceLegal ||
                Semantic.Bytes[Offset + 1] != 0 || Semantic.Bytes[Offset + 2] != 0 ||
                Semantic.Bytes[Offset + 3] != 255)
            {
                Test->AddError(FString::Printf(TEXT("%s contains an illegal label pixel at %d."),
                    *Prefix, PixelIndex));
                break;
            }

            float PixelDepthMeters = 0.0f;
            FMemory::Memcpy(&PixelDepthMeters, Depth.Bytes.GetData() + Offset, sizeof(float));
            const bool bNearDepth = FMath::IsFinite(PixelDepthMeters) &&
                PixelDepthMeters > 2.0f && PixelDepthMeters < 12.0f;
            const bool bRgbVisible =
                static_cast<int32>(Rgb.Bytes[Offset]) +
                static_cast<int32>(Rgb.Bytes[Offset + 1]) +
                static_cast<int32>(Rgb.Bytes[Offset + 2]) > 12;

            SemanticPixels += bSemanticForeground ? 1 : 0;
            InstancePixels += bInstanceForeground ? 1 : 0;
            MaskMismatchPixels += bSemanticForeground != bInstanceForeground ? 1 : 0;
            RgbVisiblePixels += bRgbVisible ? 1 : 0;
            RgbSemanticIntersection += bRgbVisible && bSemanticForeground ? 1 : 0;
            NearDepthPixels += bNearDepth ? 1 : 0;
            DepthSemanticIntersection += bNearDepth && bSemanticForeground ? 1 : 0;
            if (bSemanticForeground)
            {
                SumX += PixelIndex % ExpectedSize.X;
                SumY += PixelIndex / ExpectedSize.X;
            }
        }

        Test->TestTrue(*FString::Printf(TEXT("%s moving object remains visible"), *Prefix),
            SemanticPixels > 100);
        Test->TestEqual(*FString::Printf(TEXT("%s Semantic/Instance foreground area"), *Prefix),
            SemanticPixels, InstancePixels);
        Test->TestEqual(*FString::Printf(TEXT("%s Semantic/Instance masks align pixel-for-pixel"), *Prefix),
            MaskMismatchPixels, 0);
        const int32 RgbUnion = SemanticPixels + RgbVisiblePixels - RgbSemanticIntersection;
        const int32 DepthUnion = SemanticPixels + NearDepthPixels - DepthSemanticIntersection;
        const double RgbIou = RgbUnion > 0
            ? static_cast<double>(RgbSemanticIntersection) / static_cast<double>(RgbUnion) : 0.0;
        const double DepthIou = DepthUnion > 0
            ? static_cast<double>(DepthSemanticIntersection) / static_cast<double>(DepthUnion) : 0.0;
        // RGB 保留正常颜色边缘，允许少量抗锯齿像素；Depth 与离散标签应更接近同一几何轮廓。
        Test->TestTrue(*FString::Printf(TEXT("%s RGB/Semantic mask IoU >= 0.88 (actual %.4f)"),
            *Prefix, RgbIou), RgbIou >= 0.88);
        Test->TestTrue(*FString::Printf(TEXT("%s Depth/Semantic mask IoU >= 0.98 (actual %.4f)"),
            *Prefix, DepthIou), DepthIou >= 0.98);
        if (SemanticPixels <= 0)
        {
            return;
        }

        const FIntPoint Centroid(
            static_cast<int32>(SumX / SemanticPixels),
            static_cast<int32>(SumY / SemanticPixels));
        CentroidHistory[RigIndex].Add(Centroid);
        const int32 CenterOffset = (Centroid.Y * ExpectedSize.X + Centroid.X) * 4;

        float DepthMeters = 0.0f;
        FMemory::Memcpy(&DepthMeters, Depth.Bytes.GetData() + CenterOffset, sizeof(float));
        Test->TestTrue(*FString::Printf(TEXT("%s Depth aligns at label centroid"), *Prefix),
            FMath::IsFinite(DepthMeters) && DepthMeters > 2.0f && DepthMeters < 12.0f);

        const uint8 R = Rgb.Bytes[CenterOffset];
        const uint8 G = Rgb.Bytes[CenterOffset + 1];
        const uint8 B = Rgb.Bytes[CenterOffset + 2];
        Test->TestTrue(*FString::Printf(TEXT("%s RGB aligns at label centroid"), *Prefix),
            R != 0 || G != 0 || B != 0);
        Test->TestEqual(*FString::Printf(TEXT("%s RGB alpha is opaque"), *Prefix),
            Rgb.Bytes[CenterOffset + 3], uint8{255});
    }

    void ResetCurrentFrame()
    {
        for (FRigFramePayloads& Payloads : RigPayloads)
        {
            Payloads.ByType.Reset();
            Payloads.ChannelGuids.Reset();
        }
    }

    /** 验证联合运动轨迹、每 Manager 容量/复用和全局 Pump 最终指标。 */
    void ValidateFinalMetrics()
    {
        int64 TotalEnqueued = 0;
        int64 TotalCompleted = 0;
        int64 TotalDelivered = 0;
        int64 TotalCreated = 0;
        int64 TotalReused = 0;
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const TArray<FIntPoint>& History = CentroidHistory[RigIndex];
            Test->TestEqual(TEXT("Every Rig records all six motion samples"),
                History.Num(), MotionFrameCount);
            if (History.Num() == MotionFrameCount)
            {
                const FVector2D Delta(History.Last() - History[0]);
                Test->TestTrue(*FString::Printf(TEXT("%s observes continuous relative motion"),
                    *RigNames[RigIndex].ToString()), Delta.Size() >= 5.0);
            }

            const FImageReadbackStats Stats = Rigs[RigIndex]->GetImageReadbackStats();
            const TArray<FImageReadbackChannelStats> Channels =
                Rigs[RigIndex]->GetImageReadbackChannelStats();
            Test->TestEqual(TEXT("Each Manager capacity equals one four-modal frame"),
                Stats.Capacity, ModalitiesPerRig);
            Test->TestEqual(TEXT("Each Manager drains all Pending work"), Stats.PendingCount, 0);
            Test->TestEqual(TEXT("Each Manager enqueues twenty-four Readbacks"),
                Stats.EnqueuedCount, int64{MotionFrameCount * ModalitiesPerRig});
            Test->TestEqual(TEXT("Each Manager completes every Readback"),
                Stats.CompletedCount, Stats.EnqueuedCount);
            Test->TestEqual(TEXT("Each Manager has no post-admission rejection"),
                Stats.RejectedCount, int64{0});
            Test->TestEqual(TEXT("Each Manager has no conversion failure"),
                Stats.FailedCount, int64{0});
            Test->TestEqual(TEXT("Each Manager exposes four routed channel metrics"),
                Channels.Num(), ModalitiesPerRig);
            Test->TestEqual(TEXT("Each Manager creates one resource per modality"),
                Stats.CreatedReadbackResources, int64{ModalitiesPerRig});
            Test->TestEqual(TEXT("Each Manager reuses resources for the remaining frames"),
                Stats.ReusedReadbackResources,
                int64{(MotionFrameCount - 1) * ModalitiesPerRig});

            for (const FImageReadbackChannelStats& Channel : Channels)
            {
                Test->TestEqual(TEXT("Channel metrics keep the owning SensorGuid"),
                    Channel.SensorGuid, SensorGuidForRig(RigIndex));
                Test->TestTrue(TEXT("Channel metrics keep a known ChannelGuid"),
                    ExpectedChannels[RigIndex].Contains(Channel.ChannelGuid));
                Test->TestEqual(TEXT("Each channel is delivered once per motion frame"),
                    Channel.DeliveredCount, int64{MotionFrameCount});
            }

            TotalEnqueued += Stats.EnqueuedCount;
            TotalCompleted += Stats.CompletedCount;
            TotalCreated += Stats.CreatedReadbackResources;
            TotalReused += Stats.ReusedReadbackResources;
            for (const FImageReadbackChannelStats& Channel : Channels)
            {
                TotalDelivered += Channel.DeliveredCount;
            }
        }

        Test->TestEqual(TEXT("All four rigs accept every motion frame"),
            AcceptedCount, RigCount * MotionFrameCount);
        Test->TestEqual(TEXT("Every immediate duplicate request reports Busy"),
            BusyCount, RigCount * MotionFrameCount);
        Test->TestEqual(TEXT("Four rigs enqueue ninety-six routed Readbacks"),
            TotalEnqueued, int64{RigCount * MotionFrameCount * ModalitiesPerRig});
        Test->TestEqual(TEXT("All Phase 3 Readbacks complete"), TotalCompleted, TotalEnqueued);
        Test->TestEqual(TEXT("All Phase 3 Payloads are delivered"), TotalDelivered, TotalEnqueued);

        const FImageReadbackGlobalPumpStats GlobalAfter =
            FImageReadbackManager::GetGlobalPumpStatsForTesting();
        UE_LOG(LogTemp, Display,
            TEXT("PHASE3_METRICS Rigs=%d Frames=%d Accepted=%d Busy=%d Enqueued=%lld Completed=%lld Delivered=%lld Created=%lld Reused=%lld PumpCommands=%lld PumpedManagers=%lld PeakManagersPerPump=%d"),
            RigCount, MotionFrameCount, AcceptedCount, BusyCount,
            TotalEnqueued, TotalCompleted, TotalDelivered, TotalCreated, TotalReused,
            GlobalAfter.PumpCommandCount - GlobalPumpBefore.PumpCommandCount,
            GlobalAfter.PumpedManagerCount - GlobalPumpBefore.PumpedManagerCount,
            GlobalAfter.PeakManagersPerPump);
    }

    bool WaitOrFail(const TCHAR* Error)
    {
        if (FPlatformTime::Seconds() - StartSeconds < 180.0)
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
    int32 MotionStep = 0;
    int32 AcceptedCount = 0;
    int32 BusyCount = 0;
    uint64 CurrentFrameId = 0;
    bool bInitialized = false;
    bool bCaptureSubmitted = false;
    bool bGlobalPumpProbeComplete = false;
    AActor* MovingActor = nullptr;
    USemanticObjectComponent* MovingSemantic = nullptr;
    TArray<AActor*> CameraActors;
    TArray<UCameraRigComponent*> Rigs;
    TArray<TMap<FGuid, EPayloadType>> ExpectedChannels;
    TArray<FRigFramePayloads> RigPayloads;
    TArray<TArray<FIntPoint>> CentroidHistory;
    FImageReadbackGlobalPumpStats GlobalPumpBefore;
};

class FFlushPhase3Command final : public IAutomationLatentCommand
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
    FRenderOutputMatrixPhase3Test,
    "SensorSimulation.Rendering.OutputMatrix.Phase3.FourRigConcurrentContinuousMotion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderOutputMatrixPhase3Test::RunTest(const FString& Parameters)
{
    using namespace UE::SensorSimulation::RenderOutputMatrixPhase3Tests;

    AddExpectedError(TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A Phase 3 multi-rig World is created"), World))
    {
        return false;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    const FRotator CameraRotations[RigCount] = {
        FRotator(0.0, 0.0, 0.0), FRotator(0.0, 180.0, 0.0),
        FRotator(0.0, 90.0, 0.0), FRotator(0.0, -90.0, 0.0)
    };
    const ECameraChannelType Modalities[ModalitiesPerRig] = {
        ECameraChannelType::Rgb, ECameraChannelType::Semantic,
        ECameraChannelType::Depth, ECameraChannelType::Instance
    };

    for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
    {
        AActor* CameraActor = World->SpawnActor<AActor>();
        CameraActor->Tags.Add(RigTags[RigIndex]);
        UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(
            CameraActor, *FString::Printf(TEXT("RendererPhase3%sRig"), *RigNames[RigIndex].ToString()));
        CameraActor->SetRootComponent(Rig);
        CameraActor->AddInstanceComponent(Rig);
        Rig->SensorName = RigNames[RigIndex];
        Rig->HorizontalFovDegrees = 90.0f;
        Rig->MaxPendingReadbacks = ModalitiesPerRig;
        Rig->Channels.Empty();
        for (int32 ModalityIndex = 0; ModalityIndex < ModalitiesPerRig; ++ModalityIndex)
        {
            FCameraChannelConfig& Channel = Rig->Channels.AddDefaulted_GetRef();
            Channel.ChannelType = Modalities[ModalityIndex];
            Channel.ChannelGuid = ChannelGuidFor(RigIndex, ModalityIndex);
            Channel.Resolution = FIntPoint(320, 240);
            Channel.bForceLinearGamma = Modalities[ModalityIndex] == ECameraChannelType::Semantic ||
                Modalities[ModalityIndex] == ECameraChannelType::Instance;
        }
        Rig->RegisterComponent();
        CameraActor->SetActorLocation(BaseCameraLocations[RigIndex]);
        CameraActor->SetActorRotation(CameraRotations[RigIndex]);
    }

    // 四个侧面分别布置点光源，确保 RGB 对齐探针不会把“未受光的黑色表面”误判为缺失对象。
    // 光源只影响 RGB；Semantic/Instance/Depth 仍由各自的独立数据路径生成。
    for (int32 LightIndex = 0; LightIndex < RigCount; ++LightIndex)
    {
        AActor* LightActor = World->SpawnActor<AActor>();
        UPointLightComponent* Light = NewObject<UPointLightComponent>(
            LightActor, *FString::Printf(TEXT("RendererPhase3Light%d"), LightIndex));
        LightActor->SetRootComponent(Light);
        LightActor->AddInstanceComponent(Light);
        Light->SetIntensity(10000.0f);
        Light->SetAttenuationRadius(1200.0f);
        Light->RegisterComponent();
        LightActor->SetActorLocation(BaseCameraLocations[LightIndex] * 0.5 + FVector(0.0, 0.0, 150.0));
    }

    AActor* MovingActor = World->SpawnActor<AActor>();
    MovingActor->Tags.Add(MovingActorTag);
    UStaticMeshComponent* MovingMesh = NewObject<UStaticMeshComponent>(
        MovingActor, TEXT("RendererPhase3MovingMesh"));
    MovingActor->SetRootComponent(MovingMesh);
    MovingActor->AddInstanceComponent(MovingMesh);
    MovingMesh->SetStaticMesh(Cube);
    MovingMesh->SetMaterial(0, CreateMovingMaterial(MovingActor));
    MovingMesh->SetWorldScale3D(FVector(1.5));
    MovingMesh->RegisterComponent();
    USemanticObjectComponent* MovingSemantic = NewObject<USemanticObjectComponent>(
        MovingActor, TEXT("RendererPhase3MovingSemantic"));
    MovingSemantic->SemanticId = MovingSemanticId;
    MovingActor->AddInstanceComponent(MovingSemantic);
    MovingSemantic->RegisterComponent();

    ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FCapturePhase3Command(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFlushPhase3Command());
    return true;
}

#endif
