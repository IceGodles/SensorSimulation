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

namespace UE::SensorSimulation::RenderOutputMatrixPhase4Tests
{
static constexpr int32 RigCount = 4;
static constexpr int32 ChannelCount = 4;
static constexpr int32 RearRigIndex = 1;
static constexpr uint8 TargetSemanticId = 91;
static constexpr uint32 TargetInstanceId = 0x01071000u;
static const FName TargetTag(TEXT("RendererPhase4Target"));

static const FName RigNames[RigCount] = {
    TEXT("FrontCamera"), TEXT("RearCamera"), TEXT("LeftCamera"), TEXT("RightCamera")
};

static const FName RigTags[RigCount] = {
    TEXT("RendererPhase4Front"), TEXT("RendererPhase4Rear"),
    TEXT("RendererPhase4Left"), TEXT("RendererPhase4Right")
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
    return FGuid(0x54000000u + static_cast<uint32>(RigIndex), 0x11u, 0x12u, 0x13u);
}

FGuid ChannelGuidFor(const int32 RigIndex, const int32 ChannelIndex)
{
    return FGuid(
        0x54100000u + static_cast<uint32>(RigIndex),
        0x200u + static_cast<uint32>(ChannelIndex),
        0x14u,
        0x15u);
}

/** 创建一个具有稳定身份和四模态配置的方向相机。 */
UCameraRigComponent* CreateDirectionalRig(UWorld& World, const int32 RigIndex, AActor*& OutActor)
{
    OutActor = World.SpawnActor<AActor>();
    OutActor->Tags.Add(RigTags[RigIndex]);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(
        OutActor, *FString::Printf(TEXT("RendererPhase4%sRig"), *RigNames[RigIndex].ToString()));
    OutActor->SetRootComponent(Rig);
    OutActor->AddInstanceComponent(Rig);
    Rig->SensorName = RigNames[RigIndex];
    Rig->HorizontalFovDegrees = 90.0f;
    Rig->MaxPendingReadbacks = ChannelCount;
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
    OutActor->SetActorLocation(RigLocations[RigIndex]);
    OutActor->SetActorRotation(RigRotations[RigIndex]);
    return Rig;
}

UMaterialInstanceDynamic* CreateTargetMaterial(UObject* Outer)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(
        nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Outer);
    if (Material)
    {
        Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.2f, 0.8f, 1.0f, 1.0f));
    }
    return Material;
}

/** 单帧内按 Rig 收集到的 ChannelGuid，避免重复或跨 Rig 路由。 */
struct FRigDeliveryState
{
    TSet<FGuid> Channels;
    TSet<EPayloadType> Modalities;
};

/**
 * 验证一个 Rig 在四模态 Readback 仍 Pending 时被销毁，其他 Rig 继续交付；随后使用
 * 同一 SensorGuid/ChannelGuid 重建该方向，并再次加入全局 Pump 与正常采集。
 */
class FCapturePhase4LifecycleCommand final : public IAutomationLatentCommand
{
public:
    explicit FCapturePhase4LifecycleCommand(FAutomationTestBase* InTest)
        : Test(InTest), StartSeconds(FPlatformTime::Seconds())
    {
        Rigs.SetNumZeroed(RigCount);
        CameraActors.SetNumZeroed(RigCount);
        ExpectedChannels.SetNum(RigCount);
        DeliveryStates.SetNum(RigCount);
    }

    virtual bool Update() override
    {
        UWorld* World = GEditor ? GEditor->PlayWorld : nullptr;
        if (!World)
        {
            return WaitOrFail(TEXT("Phase 4 PIE World was not created within 180 seconds."));
        }

        if (!bInitialized)
        {
            LocateObjects(*World);
            if (!TargetSemantic || Rigs.Contains(nullptr) || CameraActors.Contains(nullptr))
            {
                Test->AddError(TEXT("Phase 4 target or one of the four initial Camera Rigs is missing."));
                return true;
            }
            if (WarmupFrames++ < 12)
            {
                return false;
            }
            TargetSemantic->SetAssignedInstanceId(TargetInstanceId, 1);
            BuildRoutingExpectations();
            bInitialized = true;
            SettleFrames = 0;
        }
        if (SettleFrames++ < 12)
        {
            return false;
        }

        switch (Phase)
        {
        case 0:
            RegisteredBeforeDestroy =
                FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount;
            Test->TestTrue(TEXT("Phase 4 starts with at least four registered Managers"),
                RegisteredBeforeDestroy >= RigCount);
            SubmitAllRigs(5400);
            DestroyRearWhilePending();
            Phase = 1;
            return false;

        case 1:
            PollSurvivingRigs(5400);
            if (!FirstFrameSurvivorsComplete())
            {
                return WaitOrFail(TEXT("The three surviving Phase 4 Rigs did not complete Frame 5400."));
            }
            RebuildRearRig(*World);
            Phase = 2;
            SettleFrames = 0;
            return false;

        case 2:
            if (SettleFrames++ < 8)
            {
                return false;
            }
            ResetDeliveries();
            SubmitAllRigs(5401);
            Phase = 3;
            return false;

        case 3:
            PollAllRigs(5401);
            if (!AllRigsComplete())
            {
                return WaitOrFail(TEXT("The rebuilt four-Rig set did not complete Frame 5401."));
            }
            ValidateFinalMetrics();
            return true;

        default:
            Test->AddError(TEXT("Phase 4 entered an invalid lifecycle phase."));
            return true;
        }
    }

private:
    void LocateObjects(UWorld& World)
    {
        if (TargetSemantic && !Rigs.Contains(nullptr))
        {
            return;
        }
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
                    CameraActors[RigIndex] = *It;
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
                Test->TestFalse(TEXT("Phase 4 ChannelGuid is globally unique"),
                    AllGuids.Contains(Config.ChannelGuid));
                AllGuids.Add(Config.ChannelGuid);
            }
        }
        Test->TestEqual(TEXT("Phase 4 has sixteen unique routed channels"),
            AllGuids.Num(), RigCount * ChannelCount);
    }

    FCaptureRequest MakeRequest(const int32 RigIndex, const uint64 FrameId) const
    {
        FCaptureRequest Request;
        Request.Header.SequenceId = 4;
        Request.Header.FrameId = FrameId;
        Request.Header.SimulationTimestampSeconds = FrameId == 5400 ? 0.0 : 0.05;
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

    /** 在任何 Poll 之前销毁 Rear Rig，保证四个任务仍占用其 Manager 容量。 */
    void DestroyRearWhilePending()
    {
        OldRearStats = Rigs[RearRigIndex]->GetImageReadbackStats();
        Test->TestEqual(TEXT("Rear Rig has four Pending Readbacks before destruction"),
            OldRearStats.PendingCount, ChannelCount);
        Test->TestEqual(TEXT("Rear Rig enqueued a complete four-modal frame before destruction"),
            OldRearStats.EnqueuedCount, int64{ChannelCount});

        Rigs[RearRigIndex]->DestroyComponent();
        CameraActors[RearRigIndex]->Destroy();
        Rigs[RearRigIndex] = nullptr;
        CameraActors[RearRigIndex] = nullptr;
        FlushRenderingCommands();

        RegisteredAfterDestroy =
            FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount;
        Test->TestEqual(TEXT("Destroyed Rear Manager unregisters immediately from global Pump"),
            RegisteredAfterDestroy, RegisteredBeforeDestroy - 1);
    }

    void PollSurvivingRigs(const uint64 FrameId)
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            if (RigIndex == RearRigIndex)
            {
                continue;
            }
            PollRig(RigIndex, FrameId);
        }
    }

    void PollAllRigs(const uint64 FrameId)
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            PollRig(RigIndex, FrameId);
        }
    }

    void PollRig(const int32 RigIndex, const uint64 FrameId)
    {
        FImagePayload Payload;
        while (Rigs[RigIndex]->PollCompletedImage(Payload))
        {
            ValidateAndRoutePayload(RigIndex, FrameId, MoveTemp(Payload));
            Payload = FImagePayload();
        }
    }

    void ValidateAndRoutePayload(
        const int32 RigIndex,
        const uint64 FrameId,
        FImagePayload&& Payload)
    {
        Test->TestEqual(TEXT("Lifecycle Payload keeps FrameId"), Payload.Header.FrameId, FrameId);
        Test->TestEqual(TEXT("Lifecycle Payload keeps SensorGuid"),
            Payload.SensorGuid, SensorGuidForRig(RigIndex));
        Test->TestEqual(TEXT("Lifecycle Payload keeps SensorName"),
            Payload.SensorName, RigNames[RigIndex]);
        const EPayloadType* ExpectedType = ExpectedChannels[RigIndex].Find(Payload.ChannelGuid);
        if (!ExpectedType)
        {
            Test->AddError(TEXT("Phase 4 received an unknown ChannelGuid."));
            return;
        }
        Test->TestEqual(TEXT("Lifecycle ChannelGuid keeps its modality"),
            Payload.PayloadType, *ExpectedType);
        Test->TestFalse(TEXT("Lifecycle frame has no duplicate ChannelGuid"),
            DeliveryStates[RigIndex].Channels.Contains(Payload.ChannelGuid));
        Test->TestFalse(TEXT("Lifecycle frame has no duplicate modality"),
            DeliveryStates[RigIndex].Modalities.Contains(Payload.PayloadType));

        const int32 CenterOffset =
            ((Payload.ImageSize.Y / 2) * Payload.ImageSize.X + Payload.ImageSize.X / 2) * 4;
        if (Payload.PayloadType == EPayloadType::Semantic)
        {
            Test->TestEqual(TEXT("Lifecycle Semantic center keeps target label"),
                Payload.Bytes[CenterOffset], TargetSemanticId);
        }
        else if (Payload.PayloadType == EPayloadType::Instance)
        {
            uint32 CenterId = 0;
            FMemory::Memcpy(&CenterId, Payload.Bytes.GetData() + CenterOffset, sizeof(uint32));
            Test->TestEqual(TEXT("Lifecycle Instance center keeps target identity"),
                CenterId, TargetInstanceId);
        }
        else if (Payload.PayloadType == EPayloadType::Depth)
        {
            float DepthMeters = 0.0f;
            FMemory::Memcpy(&DepthMeters, Payload.Bytes.GetData() + CenterOffset, sizeof(float));
            Test->TestTrue(TEXT("Lifecycle Depth center remains valid"),
                FMath::IsFinite(DepthMeters) && DepthMeters > 4.0f && DepthMeters < 7.0f);
        }

        DeliveryStates[RigIndex].Channels.Add(Payload.ChannelGuid);
        DeliveryStates[RigIndex].Modalities.Add(Payload.PayloadType);
    }

    bool FirstFrameSurvivorsComplete() const
    {
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            if (RigIndex != RearRigIndex &&
                DeliveryStates[RigIndex].Channels.Num() != ChannelCount)
            {
                return false;
            }
        }
        return DeliveryStates[RearRigIndex].Channels.IsEmpty();
    }

    bool AllRigsComplete() const
    {
        for (const FRigDeliveryState& State : DeliveryStates)
        {
            if (State.Channels.Num() != ChannelCount || State.Modalities.Num() != ChannelCount)
            {
                return false;
            }
        }
        return true;
    }

    void RebuildRearRig(UWorld& World)
    {
        AActor* NewRearActor = nullptr;
        UCameraRigComponent* NewRearRig = CreateDirectionalRig(World, RearRigIndex, NewRearActor);
        CameraActors[RearRigIndex] = NewRearActor;
        Rigs[RearRigIndex] = NewRearRig;
        RegisteredAfterRebuild =
            FImageReadbackManager::GetGlobalPumpStatsForTesting().RegisteredManagerCount;
        Test->TestEqual(TEXT("Rebuilt Rear Manager rejoins the global Pump"),
            RegisteredAfterRebuild, RegisteredBeforeDestroy);

        Test->TestEqual(TEXT("Rebuilt Rear Rig restores four channels"),
            NewRearRig->GetEnabledImageChannels().Num(), ChannelCount);
        for (const FCameraChannelConfig& Config : NewRearRig->Channels)
        {
            Test->TestTrue(TEXT("Rebuilt Rear Rig preserves original ChannelGuid routing"),
                ExpectedChannels[RearRigIndex].Contains(Config.ChannelGuid));
        }
    }

    void ResetDeliveries()
    {
        for (FRigDeliveryState& State : DeliveryStates)
        {
            State.Channels.Reset();
            State.Modalities.Reset();
        }
    }

    void ValidateFinalMetrics()
    {
        int64 SurvivingDelivered = 0;
        for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
        {
            const FImageReadbackStats Stats = Rigs[RigIndex]->GetImageReadbackStats();
            const TArray<FImageReadbackChannelStats> Channels =
                Rigs[RigIndex]->GetImageReadbackChannelStats();
            const int64 ExpectedFrames = RigIndex == RearRigIndex ? 1 : 2;
            Test->TestEqual(TEXT("Lifecycle Manager drains Pending work"), Stats.PendingCount, 0);
            Test->TestEqual(TEXT("Lifecycle Manager enqueued expected frame count"),
                Stats.EnqueuedCount, ExpectedFrames * ChannelCount);
            Test->TestEqual(TEXT("Lifecycle Manager completed every retained request"),
                Stats.CompletedCount, Stats.EnqueuedCount);
            Test->TestEqual(TEXT("Lifecycle Manager keeps four channel metric keys"),
                Channels.Num(), ChannelCount);
            for (const FImageReadbackChannelStats& Channel : Channels)
            {
                Test->TestEqual(TEXT("Lifecycle channel delivery count is exact"),
                    Channel.DeliveredCount, ExpectedFrames);
                SurvivingDelivered += Channel.DeliveredCount;
            }
        }

        Test->TestEqual(TEXT("Phase 4 accepts four initial and four rebuilt requests"),
            AcceptedCount, 8);
        Test->TestEqual(TEXT("Old Rear manager abandoned exactly one four-modal frame"),
            OldRearStats.EnqueuedCount, int64{ChannelCount});
        Test->TestEqual(TEXT("Retained and rebuilt Managers deliver twenty-eight Payloads"),
            SurvivingDelivered, int64{28});

        UE_LOG(LogTemp, Display,
            TEXT("PHASE4_METRICS Accepted=%d OldRearPending=%d OldRearEnqueued=%lld Delivered=%lld RegisteredBefore=%d RegisteredAfterDestroy=%d RegisteredAfterRebuild=%d"),
            AcceptedCount, OldRearStats.PendingCount, OldRearStats.EnqueuedCount, SurvivingDelivered,
            RegisteredBeforeDestroy, RegisteredAfterDestroy, RegisteredAfterRebuild);
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
    int32 RegisteredBeforeDestroy = 0;
    int32 RegisteredAfterDestroy = 0;
    int32 RegisteredAfterRebuild = 0;
    bool bInitialized = false;
    USemanticObjectComponent* TargetSemantic = nullptr;
    TArray<UCameraRigComponent*> Rigs;
    TArray<AActor*> CameraActors;
    TArray<TMap<FGuid, EPayloadType>> ExpectedChannels;
    TArray<FRigDeliveryState> DeliveryStates;
    FImageReadbackStats OldRearStats;
};

class FFlushPhase4Command final : public IAutomationLatentCommand
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
    FRenderOutputMatrixPhase4LifecycleTest,
    "SensorSimulation.Rendering.OutputMatrix.Phase4.PendingRigDestroyRebuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderOutputMatrixPhase4LifecycleTest::RunTest(const FString& Parameters)
{
    using namespace UE::SensorSimulation::RenderOutputMatrixPhase4Tests;

    AddExpectedError(TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A Phase 4 lifecycle World is created"), World))
    {
        return false;
    }

    for (int32 RigIndex = 0; RigIndex < RigCount; ++RigIndex)
    {
        AActor* CameraActor = nullptr;
        CreateDirectionalRig(*World, RigIndex, CameraActor);
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    AActor* TargetActor = World->SpawnActor<AActor>();
    TargetActor->Tags.Add(TargetTag);
    UStaticMeshComponent* TargetMesh = NewObject<UStaticMeshComponent>(
        TargetActor, TEXT("RendererPhase4TargetMesh"));
    TargetActor->SetRootComponent(TargetMesh);
    TargetActor->AddInstanceComponent(TargetMesh);
    TargetMesh->SetStaticMesh(Cube);
    TargetMesh->SetMaterial(0, CreateTargetMaterial(TargetActor));
    TargetMesh->SetWorldScale3D(FVector(1.5));
    TargetMesh->RegisterComponent();
    USemanticObjectComponent* Semantic = NewObject<USemanticObjectComponent>(
        TargetActor, TEXT("RendererPhase4TargetSemantic"));
    Semantic->SemanticId = TargetSemanticId;
    TargetActor->AddInstanceComponent(Semantic);
    Semantic->RegisterComponent();

    for (int32 LightIndex = 0; LightIndex < RigCount; ++LightIndex)
    {
        AActor* LightActor = World->SpawnActor<AActor>();
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
    ADD_LATENT_AUTOMATION_COMMAND(FCapturePhase4LifecycleCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FFlushPhase4Command());
    return true;
}

#endif
