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
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::RenderOutputMatrixPhase5Tests
{
static constexpr int32 RigCount = 4;
static constexpr int32 ChannelCount = 4;
static constexpr int32 RearRigIndex = 1;
static constexpr int32 SemanticChannelIndex = 1;
static constexpr uint8 TargetSemanticId = 93;
static constexpr uint32 TargetInstanceId = 0x01081000u;
static constexpr uint64 InitialFrameId = 5500;
static constexpr uint64 ReducedFrameId = 5501;
static constexpr uint64 RestoredFrameId = 5502;
static const FName TargetTag(TEXT("RendererPhase5Target"));

static const FName RigNames[RigCount] = {
    TEXT("FrontCamera"), TEXT("RearCamera"), TEXT("LeftCamera"), TEXT("RightCamera")
};

static const FName RigTags[RigCount] = {
    TEXT("RendererPhase5Front"), TEXT("RendererPhase5Rear"),
    TEXT("RendererPhase5Left"), TEXT("RendererPhase5Right")
};

static const FVector RigLocations[RigCount] = {
    FVector(-600.0, 0.0, 0.0), FVector(600.0, 0.0, 0.0),
    FVector(0.0, -600.0, 0.0), FVector(0.0, 600.0, 0.0)
};

static const FRotator RigRotations[RigCount] = {
    FRotator(0.0, 0.0, 0.0), FRotator(0.0, 180.0, 0.0),
    FRotator(0.0, 90.0, 0.0), FRotator(0.0, -90.0, 0.0)
};

FGuid SensorGuidForRig(const int32 RigIndex)
{
    return FGuid(0x55000000u + static_cast<uint32>(RigIndex), 0x21u, 0x22u, 0x23u);
}

FGuid ChannelGuidFor(const int32 RigIndex, const int32 ChannelIndex)
{
    return FGuid(
        0x55100000u + static_cast<uint32>(RigIndex),
        0x300u + static_cast<uint32>(ChannelIndex),
        0x24u,
        0x25u);
}

/** 创建具有稳定传感器身份、四模态配置和七任务容量的方向相机。 */
UCameraRigComponent* CreateDirectionalRig(UWorld& World, const int32 RigIndex)
{
    AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass());
    Actor->Tags.Add(RigTags[RigIndex]);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(
        Actor, *FString::Printf(TEXT("RendererPhase5%sRig"), *RigNames[RigIndex].ToString()));
    Actor->SetRootComponent(Rig);
    Actor->AddInstanceComponent(Rig);
    Rig->SensorName = RigNames[RigIndex];
    Rig->HorizontalFovDegrees = 90.0f;
    // Rear 在旧四通道仍 Pending 时还要接收三通道帧，因此容量精确设置为 4 + 3。
    Rig->MaxPendingReadbacks = 7;
    Rig->Channels.Empty();

    const ECameraChannelType Types[ChannelCount] = {
        ECameraChannelType::Rgb, ECameraChannelType::Semantic,
        ECameraChannelType::Depth, ECameraChannelType::Instance
    };
    for (int32 ChannelIndex = 0; ChannelIndex < ChannelCount; ++ChannelIndex)
    {
        FCameraChannelConfig& Config = Rig->Channels.AddDefaulted_GetRef();
        Config.ChannelType = Types[ChannelIndex];
        Config.ChannelGuid = ChannelGuidFor(RigIndex, ChannelIndex);
        Config.Resolution = FIntPoint(160, 120);
        Config.bForceLinearGamma = Types[ChannelIndex] == ECameraChannelType::Semantic ||
            Types[ChannelIndex] == ECameraChannelType::Instance;
    }
    Rig->RegisterComponent();
    Actor->SetActorLocation(RigLocations[RigIndex]);
    Actor->SetActorRotation(RigRotations[RigIndex]);
    return Rig;
}

UMaterialInstanceDynamic* CreateTargetMaterial(UObject* Outer)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(
        nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Outer);
    if (Material)
    {
        Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.8f, 0.25f, 0.1f, 1.0f));
    }
    return Material;
}

/** 保存一个 Rig 在一个 FrameId 内已经交付的通道与模态。 */
struct FDeliveryState
{
    TSet<FGuid> Channels;
    TSet<EPayloadType> Modalities;
};

/**
 * 在 Rear 仍有四个 Pending Readback 时禁用 Semantic，再提交三通道帧；排空后恢复
 * 同一 Semantic ChannelGuid，并证明其他 Rig、旧 Payload 与新资源互不污染。
 */
class FCapturePhase5ChannelHotReloadCommand final : public IAutomationLatentCommand
{
public:
    explicit FCapturePhase5ChannelHotReloadCommand(FAutomationTestBase* InTest)
        : Test(InTest), StartSeconds(FPlatformTime::Seconds())
    {
        Rigs.SetNumZeroed(RigCount);
        ExpectedChannels.SetNum(RigCount);
        InitialDeliveries.SetNum(RigCount);
        RestoredDeliveries.SetNum(RigCount);
    }

    virtual bool Update() override
    {
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        if (!World)
        {
            return WaitOrFail(TEXT("Phase 5 PIE World was not created within 180 seconds."));
        }

        if (!bInitialized)
        {
            LocateObjects(*World);
            if (!TargetSemantic || Rigs.Contains(nullptr))
            {
                Test->AddError(TEXT("Phase 5 target or one of the four Camera Rigs is missing."));
                return true;
            }
            if (WarmupFrames++ < 12)
            {
                return false;
            }
            TargetSemantic->SetAssignedInstanceId(TargetInstanceId, 1);
            BuildRoutingExpectations();
            RegisteredManagerCount =
                FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount;
            bInitialized = true;
        }

        switch (Phase)
        {
        case 0:
            SubmitAllRigs(InitialFrameId);
            DisableRearSemanticWhilePending();
            SubmitRearReducedFrame();
            Phase = 1;
            return false;

        case 1:
            PollAllRigs();
            if (!InitialAndReducedFramesComplete())
            {
                return WaitOrFail(TEXT("Phase 5 initial and reduced-channel frames did not complete."));
            }
            RestoreRearSemantic();
            SettleFrames = 0;
            Phase = 2;
            return false;

        case 2:
            if (SettleFrames++ < 12)
            {
                return false;
            }
            SubmitAllRigs(RestoredFrameId);
            Phase = 3;
            return false;

        case 3:
            PollAllRigs();
            if (!RestoredFrameComplete())
            {
                return WaitOrFail(TEXT("Phase 5 restored four-modal frame did not complete."));
            }
            ValidateFinalMetrics();
            return true;

        default:
            Test->AddError(TEXT("Phase 5 entered an invalid lifecycle phase."));
            return true;
        }
    }

private:
    void LocateObjects(UWorld& World)
    {
        for (TActorIterator<AActor> It(&World); It; ++It)
        {
            if (It->ActorHasTag(TargetTag))
            {
                TargetSemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
            for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
            {
                if (It->ActorHasTag(RigTags[RigIndex]))
                {
                    Rigs[RigIndex] = It->FindComponentByClass<UCameraRigComponent>();
                }
            }
        }
    }

    void BuildRoutingExpectations()
    {
        TSet<FGuid> AllGuids;
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            for (const FCameraChannelConfig& Config : Rigs[RigIndex]->Channels)
            {
                ExpectedChannels[RigIndex].Add(Config.ChannelGuid, Config.ToPayloadType());
                Test->TestFalse(TEXT("Phase 5 ChannelGuid is globally unique"),
                    AllGuids.Contains(Config.ChannelGuid));
                AllGuids.Add(Config.ChannelGuid);
            }
        }
        RearSemanticGuid = ChannelGuidFor(RearRigIndex, SemanticChannelIndex);
        Test->TestEqual(TEXT("Phase 5 has sixteen stable ChannelGuids"),
            AllGuids.Num(), RigCount * ChannelCount);
    }

    FCaptureRequest MakeRequest(const int32 RigIndex, const uint64 FrameId) const
    {
        FCaptureRequest Request;
        Request.Header.SequenceId = 5;
        Request.Header.FrameId = FrameId;
        Request.Header.SimulationTimestampSeconds = (FrameId - InitialFrameId) * 0.05;
        Request.SensorName = RigNames[RigIndex];
        Request.SensorGuid = SensorGuidForRig(RigIndex);
        Request.ExpectedPayloads = Rigs[RigIndex]->GetEnabledPayloadTypes();
        Request.ExpectedImageChannels = Rigs[RigIndex]->GetEnabledImageChannels();
        return Request;
    }

    void SubmitAllRigs(const uint64 FrameId)
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const ECaptureRequestResult Result = Rigs[RigIndex]->SubmitCapture(
                MakeRequest(RigIndex, FrameId));
            Test->TestEqual(*FString::Printf(TEXT("Frame %llu %s is Accepted"),
                FrameId, *RigNames[RigIndex].ToString()), Result, ECaptureRequestResult::Accepted);
            AcceptedCount += Result == ECaptureRequestResult::Accepted ? 1 : 0;
        }
    }

    /** 禁用 Rear Semantic；其他三个 Capture/Target 必须保持原对象，旧目标由退休数组保活。 */
    void DisableRearSemanticWhilePending()
    {
        UCameraRigComponent* Rear = Rigs[RearRigIndex];
        const FImageReadbackStats BeforeReadback = Rear->GetImageReadbackStats();
        Test->TestEqual(TEXT("Rear has four Pending Readbacks before Semantic disable"),
            BeforeReadback.PendingCount, ChannelCount);
        BeforeDisableResources = Rear->GetResourceStats();
        RegisteredBeforeDisable =
            FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount;

        Rear->Channels[SemanticChannelIndex].bEnabled = false;
        Test->TestTrue(TEXT("Disabling Rear Semantic changes runtime configuration"),
            Rear->ApplyConfiguration());
        AfterDisableResources = Rear->GetResourceStats();

        Test->TestEqual(TEXT("Rear exposes three active channels while Semantic is disabled"),
            Rear->GetEnabledImageChannels().Num(), ChannelCount - 1);
        Test->TestNull(TEXT("Disabled Semantic no longer has an active RenderTarget"),
            Rear->GetChannelRenderTarget(RearSemanticGuid));
        Test->TestEqual(TEXT("Channel disable keeps the same Readback Manager registered"),
            FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount,
            RegisteredBeforeDisable);
        Test->TestEqual(TEXT("Disable reuses three unaffected Capture components"),
            AfterDisableResources.ReusedCaptureComponents - BeforeDisableResources.ReusedCaptureComponents,
            int64{3});
        Test->TestEqual(TEXT("Disable reuses three unaffected RenderTarget groups"),
            AfterDisableResources.ReusedRenderTargets - BeforeDisableResources.ReusedRenderTargets,
            int64{3});
        Test->TestEqual(TEXT("Disable destroys only the Semantic Capture component"),
            AfterDisableResources.DestroyedCaptureComponents - BeforeDisableResources.DestroyedCaptureComponents,
            int64{1});
        Test->TestEqual(TEXT("Disable retires only the Semantic RenderTarget"),
            AfterDisableResources.DestroyedRenderTargets - BeforeDisableResources.DestroyedRenderTargets,
            int64{1});
    }

    /** 旧四通道仍在途时，Rear 以当前三通道配置继续提交，容量恰好达到七。 */
    void SubmitRearReducedFrame()
    {
        UCameraRigComponent* Rear = Rigs[RearRigIndex];
        const FCaptureRequest Request = MakeRequest(RearRigIndex, ReducedFrameId);
        Test->TestEqual(TEXT("Reduced Rear request contains exactly three channels"),
            Request.ExpectedImageChannels.Num(), ChannelCount - 1);
        Test->TestFalse(TEXT("Reduced Rear request excludes the disabled Semantic ChannelGuid"),
            Request.ExpectedImageChannels.ContainsByPredicate(
                [this](const FExpectedImageChannel& Channel)
                {
                    return Channel.ChannelGuid == RearSemanticGuid;
                }));
        const ECaptureRequestResult Result = Rear->SubmitCapture(Request);
        Test->TestEqual(TEXT("Rear accepts a three-channel frame while four old tasks are Pending"),
            Result, ECaptureRequestResult::Accepted);
        AcceptedCount += Result == ECaptureRequestResult::Accepted ? 1 : 0;
        Test->TestEqual(TEXT("Rear reaches the exact seven-task capacity"),
            Rear->GetImageReadbackStats().PendingCount, 7);
    }

    void PollAllRigs()
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            FImagePayload Payload;
            while (Rigs[RigIndex]->PollCompletedImage(Payload))
            {
                ValidateAndRoutePayload(RigIndex, MoveTemp(Payload));
                Payload = FImagePayload();
            }
        }
    }

    void ValidateAndRoutePayload(const int32 RigIndex, FImagePayload&& Payload)
    {
        Test->TestEqual(TEXT("Phase 5 Payload keeps SensorGuid"),
            Payload.SensorGuid, SensorGuidForRig(RigIndex));
        Test->TestEqual(TEXT("Phase 5 Payload keeps SensorName"),
            Payload.SensorName, RigNames[RigIndex]);
        const EPayloadType* ExpectedType = ExpectedChannels[RigIndex].Find(Payload.ChannelGuid);
        if (!ExpectedType)
        {
            Test->AddError(TEXT("Phase 5 received an unknown ChannelGuid."));
            return;
        }
        Test->TestEqual(TEXT("Phase 5 ChannelGuid keeps its modality"),
            Payload.PayloadType, *ExpectedType);

        FDeliveryState* State = nullptr;
        if (Payload.Header.FrameId == InitialFrameId)
        {
            State = &InitialDeliveries[RigIndex];
        }
        else if (Payload.Header.FrameId == ReducedFrameId && RigIndex == RearRigIndex)
        {
            State = &ReducedRearDelivery;
            Test->TestNotEqual(TEXT("Reduced frame never delivers disabled Semantic"),
                Payload.ChannelGuid, RearSemanticGuid);
        }
        else if (Payload.Header.FrameId == RestoredFrameId)
        {
            State = &RestoredDeliveries[RigIndex];
        }
        else
        {
            Test->AddError(FString::Printf(TEXT("Unexpected Phase 5 FrameId %llu for Rig %d."),
                Payload.Header.FrameId, RigIndex));
            return;
        }

        Test->TestFalse(TEXT("Phase 5 frame has no duplicate ChannelGuid"),
            State->Channels.Contains(Payload.ChannelGuid));
        Test->TestFalse(TEXT("Phase 5 frame has no duplicate modality"),
            State->Modalities.Contains(Payload.PayloadType));

        const int32 CenterOffset =
            ((Payload.ImageSize.Y / 2) * Payload.ImageSize.X + Payload.ImageSize.X / 2) * 4;
        if (Payload.Bytes.Num() < CenterOffset + 4)
        {
            Test->AddError(TEXT("Phase 5 Payload is too small for its center pixel."));
            return;
        }
        if (Payload.PayloadType == EPayloadType::Semantic)
        {
            Test->TestEqual(TEXT("Phase 5 Semantic center keeps target label"),
                Payload.Bytes[CenterOffset], TargetSemanticId);
        }
        else if (Payload.PayloadType == EPayloadType::Instance)
        {
            uint32 CenterId = 0;
            FMemory::Memcpy(&CenterId, Payload.Bytes.GetData() + CenterOffset, sizeof(uint32));
            Test->TestEqual(TEXT("Phase 5 Instance center keeps target identity"),
                CenterId, TargetInstanceId);
        }
        else if (Payload.PayloadType == EPayloadType::Depth)
        {
            float DepthMeters = 0.0f;
            FMemory::Memcpy(&DepthMeters, Payload.Bytes.GetData() + CenterOffset, sizeof(float));
            Test->TestTrue(TEXT("Phase 5 Depth center remains valid"),
                FMath::IsFinite(DepthMeters) && DepthMeters > 4.0f && DepthMeters < 7.0f);
        }

        State->Channels.Add(Payload.ChannelGuid);
        State->Modalities.Add(Payload.PayloadType);
        ++DeliveredCount;
    }

    bool InitialAndReducedFramesComplete() const
    {
        for (const FDeliveryState& State : InitialDeliveries)
        {
            if (State.Channels.Num() != ChannelCount || State.Modalities.Num() != ChannelCount)
            {
                return false;
            }
        }
        return ReducedRearDelivery.Channels.Num() == ChannelCount - 1 &&
            ReducedRearDelivery.Modalities.Num() == ChannelCount - 1;
    }

    /** 恢复原配置项而不是新增替代项，确保通道身份跨开关保持不变。 */
    void RestoreRearSemantic()
    {
        UCameraRigComponent* Rear = Rigs[RearRigIndex];
        const FImageReadbackStats DrainedStats = Rear->GetImageReadbackStats();
        Test->TestEqual(TEXT("Rear Pending is drained before Semantic restore"),
            DrainedStats.PendingCount, 0);
        Test->TestEqual(TEXT("Rear delivered old four-modal plus reduced three-modal work"),
            DrainedStats.CompletedCount, int64{7});

        Rear->Channels[SemanticChannelIndex].bEnabled = true;
        Test->TestEqual(TEXT("Semantic ChannelGuid remains stable before restore"),
            Rear->Channels[SemanticChannelIndex].ChannelGuid, RearSemanticGuid);
        Test->TestTrue(TEXT("Restoring Rear Semantic changes runtime configuration"),
            Rear->ApplyConfiguration());
        AfterRestoreResources = Rear->GetResourceStats();

        Test->TestEqual(TEXT("Rear exposes four active channels after restore"),
            Rear->GetEnabledImageChannels().Num(), ChannelCount);
        Test->TestNotNull(TEXT("Restored Semantic owns a new active RenderTarget"),
            Rear->GetChannelRenderTarget(RearSemanticGuid));
        Test->TestEqual(TEXT("Semantic restore keeps global Manager registration stable"),
            FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount,
            RegisteredManagerCount);
        Test->TestEqual(TEXT("Restore reuses the three unaffected Capture components"),
            AfterRestoreResources.ReusedCaptureComponents - AfterDisableResources.ReusedCaptureComponents,
            int64{3});
        Test->TestEqual(TEXT("Restore reuses the three unaffected RenderTarget groups"),
            AfterRestoreResources.ReusedRenderTargets - AfterDisableResources.ReusedRenderTargets,
            int64{3});
        Test->TestEqual(TEXT("Restore creates one replacement Semantic Capture"),
            AfterRestoreResources.CreatedCaptureComponents - AfterDisableResources.CreatedCaptureComponents,
            int64{1});
        Test->TestEqual(TEXT("Restore creates one replacement Semantic RenderTarget"),
            AfterRestoreResources.CreatedRenderTargets - AfterDisableResources.CreatedRenderTargets,
            int64{1});
    }

    bool RestoredFrameComplete() const
    {
        for (const FDeliveryState& State : RestoredDeliveries)
        {
            if (State.Channels.Num() != ChannelCount || State.Modalities.Num() != ChannelCount)
            {
                return false;
            }
        }
        return true;
    }

    void ValidateFinalMetrics()
    {
        int64 TotalEnqueued = 0;
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const FImageReadbackStats Stats = Rigs[RigIndex]->GetImageReadbackStats();
            const int64 Expected = RigIndex == RearRigIndex ? 11 : 8;
            Test->TestEqual(TEXT("Phase 5 Manager drains all Pending work"), Stats.PendingCount, 0);
            Test->TestEqual(TEXT("Phase 5 Manager enqueued the expected channel count"),
                Stats.EnqueuedCount, Expected);
            Test->TestEqual(TEXT("Phase 5 Manager completed all retained work"),
                Stats.CompletedCount, Expected);
            TotalEnqueued += Stats.EnqueuedCount;
        }

        const TArray<FImageReadbackChannelStats> RearChannels =
            Rigs[RearRigIndex]->GetImageReadbackChannelStats();
        Test->TestEqual(TEXT("Rear metrics retain all four stable ChannelGuid keys"),
            RearChannels.Num(), ChannelCount);
        for (const FImageReadbackChannelStats& Channel : RearChannels)
        {
            const int64 ExpectedDeliveries = Channel.ChannelGuid == RearSemanticGuid ? 2 : 3;
            Test->TestEqual(TEXT("Rear per-channel delivery count reflects disabled interval"),
                Channel.DeliveredCount, ExpectedDeliveries);
        }

        Test->TestEqual(TEXT("Phase 5 accepts nine capture requests"), AcceptedCount, 9);
        Test->TestEqual(TEXT("Phase 5 delivers thirty-five Payloads"), DeliveredCount, 35);
        Test->TestEqual(TEXT("Phase 5 enqueues thirty-five Readbacks"), TotalEnqueued, int64{35});

        UE_LOG(LogTemp, Display,
            TEXT("PHASE5_METRICS Accepted=%d Delivered=%d Enqueued=%lld RearSemanticDelivered=2 RearOtherDelivered=3 RegisteredManagers=%d ReusedCaptures=%lld ReusedTargets=%lld DestroyedCaptures=%lld DestroyedTargets=%lld"),
            AcceptedCount, DeliveredCount, TotalEnqueued, RegisteredManagerCount,
            AfterRestoreResources.ReusedCaptureComponents - BeforeDisableResources.ReusedCaptureComponents,
            AfterRestoreResources.ReusedRenderTargets - BeforeDisableResources.ReusedRenderTargets,
            AfterRestoreResources.DestroyedCaptureComponents - BeforeDisableResources.DestroyedCaptureComponents,
            AfterRestoreResources.DestroyedRenderTargets - BeforeDisableResources.DestroyedRenderTargets);
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
    int32 SettleFrames = 0;
    int32 Phase = 0;
    int32 AcceptedCount = 0;
    int32 DeliveredCount = 0;
    int32 RegisteredManagerCount = 0;
    int32 RegisteredBeforeDisable = 0;
    bool bInitialized = false;
    FGuid RearSemanticGuid;
    USemanticObjectComponent* TargetSemantic = nullptr;
    TArray<UCameraRigComponent*> Rigs;
    TArray<TMap<FGuid, EPayloadType>> ExpectedChannels;
    TArray<FDeliveryState> InitialDeliveries;
    FDeliveryState ReducedRearDelivery;
    TArray<FDeliveryState> RestoredDeliveries;
    FCameraRigResourceStats BeforeDisableResources;
    FCameraRigResourceStats AfterDisableResources;
    FCameraRigResourceStats AfterRestoreResources;
};

class FFlushPhase5Command final : public IAutomationLatentCommand
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
    FRenderOutputMatrixPhase5ChannelHotReloadTest,
    "SensorSimulation.Rendering.OutputMatrix.Phase5.ChannelDisableRestore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderOutputMatrixPhase5ChannelHotReloadTest::RunTest(const FString& Parameters)
{
    using namespace UE::SensorSimulation::RenderOutputMatrixPhase5Tests;

    // CreateNewMap/PIE 的编辑器辅助路径会尝试一次无类 Spawn；与本测试显式创建的 Actor 无关。
    AddExpectedError(TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A Phase 5 hot-reload World is created"), World))
    {
        return false;
    }

    for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
    {
        CreateDirectionalRig(*World, RigIndex);
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    AActor* TargetActor = World->SpawnActor<AActor>(AActor::StaticClass());
    TargetActor->Tags.Add(TargetTag);
    UStaticMeshComponent* TargetMesh = NewObject<UStaticMeshComponent>(
        TargetActor, TEXT("RendererPhase5TargetMesh"));
    TargetActor->SetRootComponent(TargetMesh);
    TargetActor->AddInstanceComponent(TargetMesh);
    TargetMesh->SetStaticMesh(Cube);
    TargetMesh->SetMaterial(0, CreateTargetMaterial(TargetActor));
    TargetMesh->SetWorldScale3D(FVector(1.5));
    TargetMesh->RegisterComponent();
    USemanticObjectComponent* Semantic = NewObject<USemanticObjectComponent>(
        TargetActor, TEXT("RendererPhase5TargetSemantic"));
    Semantic->SemanticId = TargetSemanticId;
    TargetActor->AddInstanceComponent(Semantic);
    Semantic->RegisterComponent();

    for (int32 LightIndex = 0; LightIndex < RigCount; ++LightIndex)
    {
        AActor* LightActor = World->SpawnActor<AActor>(AActor::StaticClass());
        UPointLightComponent* Light = NewObject<UPointLightComponent>(LightActor);
        LightActor->SetRootComponent(Light);
        LightActor->AddInstanceComponent(Light);
        Light->SetIntensity(8000.0f);
        Light->SetAttenuationRadius(1200.0f);
        Light->RegisterComponent();
        LightActor->SetActorLocation(RigLocations[LightIndex] * 0.5 + FVector(0.0, 0.0, 150.0));
    }

    ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FCapturePhase5ChannelHotReloadCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFlushPhase5Command());
    return true;
}

#endif
