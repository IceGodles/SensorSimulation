#include "CameraRigComponent.h"
#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::Tests
{
static const FName LifecycleActorTag(TEXT("SensorSimulationLifecycleTest"));
static const FName InstanceActorTag(TEXT("SensorSimulationInstanceTest"));
static const FName NaniteActorTag(TEXT("SensorSimulationNaniteTest"));
static constexpr uint32 ExpectedLargeInstanceId = 0x01020304u;
static constexpr uint32 ExpectedNaniteInstanceId = 0x01020400u;

/** 在 PIE 中先完成一帧验证指标，再提交第二帧并让后续命令立即停止 PIE。 */
class FSubmitCaptureInPIECommand final : public IAutomationLatentCommand
{
public:
    explicit FSubmitCaptureInPIECommand(FAutomationTestBase* InTest)
        : Test(InTest)
        , StartSeconds(FPlatformTime::Seconds())
    {
    }

    virtual bool Update() override
    {
        UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
        if (!PieWorld)
        {
            // PIE 创建是异步的；给编辑器少量时间完成 World duplication。
            if (FPlatformTime::Seconds() - StartSeconds < 30.0)
            {
                return false;
            }

            Test->AddError(TEXT("PIE World was not created within 30 seconds."));
            return true;
        }

        for (TActorIterator<AActor> It(PieWorld); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor->ActorHasTag(LifecycleActorTag))
            {
                continue;
            }

            UCameraRigComponent* Rig = Actor->FindComponentByClass<UCameraRigComponent>();
            if (!Rig)
            {
                Test->AddError(TEXT("Lifecycle test Actor was duplicated without its Camera Rig."));
                return true;
            }

            if (!bFirstCaptureSubmitted)
            {
                // ISM/HISM 的 SceneProxy 和 GPU Scene 实例数据在 PIE World 建立后异步上传；
                // 预热若干帧，确保像素回归验证稳定渲染结果而不是初始化竞态。
                if (WarmupFrameCount++ < 5)
                {
                    return false;
                }

                // 覆盖 8 位边界：若实现错误复用 CustomStencil，这个值只能剩下低 8 位 4。
                for (TActorIterator<AActor> InstanceIt(PieWorld); InstanceIt; ++InstanceIt)
                {
                    if (USemanticObjectComponent* Semantic =
                        InstanceIt->FindComponentByClass<USemanticObjectComponent>())
                    {
                        if (InstanceIt->ActorHasTag(InstanceActorTag))
                        {
                            Semantic->SetAssignedInstanceId(ExpectedLargeInstanceId, 3);
                        }
                        else if (InstanceIt->ActorHasTag(NaniteActorTag))
                        {
                            Semantic->SetAssignedInstanceId(ExpectedNaniteInstanceId, 1);
                        }
                    }
                }

                FCaptureRequest FirstRequest;
                FirstRequest.Header.FrameId = 1;
                FirstRequest.Header.SequenceId = 1;
                FirstRequest.SensorName = Rig->SensorName;
                FirstRequest.SensorGuid = FGuid(1, 2, 3, 4);
                FirstRequest.ExpectedPayloads = Rig->GetEnabledPayloadTypes();
                FirstRequest.ExpectedImageChannels = Rig->GetEnabledImageChannels();
                Test->TestTrue(
                    TEXT("PIE Camera Rig exposes at least one supported payload"),
                    FirstRequest.ExpectedPayloads != EPayloadType::None);

                Rig->SubmitCapture(FirstRequest);
                bFirstCaptureSubmitted = true;

                const TArray<FImageReadbackChannelStats> InitialStats = Rig->GetImageReadbackChannelStats();
                ExpectedPayloadCount = InitialStats.Num();
                Test->TestEqual(TEXT("RGB, Semantic and Instance have separate metric keys"), ExpectedPayloadCount, 3);
                for (const FImageReadbackChannelStats& Stats : InitialStats)
                {
                    Test->TestEqual(TEXT("Metric key preserves the Camera SensorName"), Stats.SensorName, Rig->SensorName);
                    Test->TestEqual(TEXT("Each enabled channel accepted one capture"), Stats.EnqueuedCount, int64{1});
                    Test->TestEqual(TEXT("Each accepted channel starts pending"), Stats.PendingCount, 1);
                }

                FGuid RgbChannelGuid;
                for (const FExpectedImageChannel& Channel : FirstRequest.ExpectedImageChannels)
                {
                    if (Channel.PayloadType == EPayloadType::Rgb)
                    {
                        RgbChannelGuid = Channel.ChannelGuid;
                        break;
                    }
                }
                UTextureRenderTarget2D* PendingRgbTarget = Rig->GetChannelRenderTarget(RgbChannelGuid);
                for (FCameraChannelConfig& Channel : Rig->Channels)
                {
                    if (Channel.ChannelGuid == RgbChannelGuid)
                    {
                        Channel.Resolution = FIntPoint(19, 11);
                        break;
                    }
                }
                // 故意在首帧 GPU Copy 尚未交付时热更新，验证旧 Target 会保活到 Readback 排空。
                Test->TestTrue(TEXT("Pending RGB target can be hot-replaced safely"), Rig->ApplyConfiguration());
                Test->TestTrue(TEXT("Pending update switches future captures to a new target"),
                    Rig->GetChannelRenderTarget(RgbChannelGuid) != PendingRgbTarget);
                return false;
            }

            FImagePayload Payload;
            while (Rig->PollCompletedImage(Payload))
            {
                if (Payload.Header.FrameId == 1)
                {
                    ++DeliveredPayloadCount;
                    if (Payload.PayloadType == EPayloadType::Instance)
                    {
                        bSawInstancePayload = true;
                        Test->TestEqual(
                            TEXT("Instance payload declares R32Uint"),
                            Payload.PixelFormat,
                            EImagePixelFormat::R32Uint);
                        Test->TestEqual(
                            TEXT("Instance payload uses identifier units"),
                            Payload.ValueUnit,
                            EImageValueUnit::Identifier);
                        for (int32 Offset = 0;
                            Offset + static_cast<int32>(sizeof(uint32)) <= Payload.Bytes.Num();
                            Offset += sizeof(uint32))
                        {
                            uint32 InstanceId = 0;
                            FMemory::Memcpy(
                                &InstanceId,
                                Payload.Bytes.GetData() + Offset,
                                sizeof(uint32));
                            if (InstanceId != 0)
                            {
                                ObservedInstancePixelCounts.FindOrAdd(InstanceId)++;
                            }
                            bSawFirstInternalInstanceId |=
                                InstanceId == ExpectedLargeInstanceId + 1u;
                            bSawSecondInternalInstanceId |=
                                InstanceId == ExpectedLargeInstanceId + 2u;
                            bSawNaniteInstanceId |=
                                InstanceId == ExpectedNaniteInstanceId;
                        }
                    }
                }
            }

            if (DeliveredPayloadCount < ExpectedPayloadCount)
            {
                if (FPlatformTime::Seconds() - StartSeconds < 30.0)
                {
                    return false;
                }

                Test->AddError(FString::Printf(
                    TEXT("Only %d/%d image payloads completed within 30 seconds."),
                    DeliveredPayloadCount,
                    ExpectedPayloadCount));
                return true;
            }

            Test->TestTrue(TEXT("A formal Instance payload was delivered"), bSawInstancePayload);
            FString ObservedIds;
            for (const TPair<uint32, int32>& Pair : ObservedInstancePixelCounts)
            {
                ObservedIds += FString::Printf(TEXT("%u:%d "), Pair.Key, Pair.Value);
            }
            Test->AddInfo(FString::Printf(
                TEXT("Observed non-zero Instance pixels (ID:count): %s"),
                *ObservedIds));
            Test->TestTrue(
                TEXT("First ISM internal instance writes BaseInstanceId + 0"),
                bSawFirstInternalInstanceId);
            Test->TestTrue(
                TEXT("Second ISM internal instance writes BaseInstanceId + 1"),
                bSawSecondInternalInstanceId);
            Test->TestTrue(
                TEXT("Nanite VisBuffer export writes the assigned 32-bit InstanceId"),
                bSawNaniteInstanceId);

            const TArray<FImageReadbackChannelStats> CompletedStats = Rig->GetImageReadbackChannelStats();
            for (const FImageReadbackChannelStats& Stats : CompletedStats)
            {
                Test->TestEqual(TEXT("Each channel completed one GPU Readback"), Stats.CompletedCount, int64{1});
                Test->TestEqual(TEXT("Each channel delivered one CPU Payload"), Stats.DeliveredCount, int64{1});
                Test->TestEqual(TEXT("Delivered first frame releases channel capacity"), Stats.PendingCount, 0);
                Test->TestTrue(TEXT("GPU latency is recorded"), Stats.AverageGpuLatencyMs > 0.0);
                Test->TestTrue(TEXT("End-to-end delivery latency is recorded"), Stats.AverageDeliveryLatencyMs > 0.0);
            }

            FCaptureRequest PendingRequest;
            PendingRequest.Header.FrameId = 2;
            PendingRequest.Header.SequenceId = 1;
            PendingRequest.SensorName = Rig->SensorName;
            PendingRequest.SensorGuid = FGuid(1, 2, 3, 4);
            PendingRequest.ExpectedPayloads = Rig->GetEnabledPayloadTypes();
            PendingRequest.ExpectedImageChannels = Rig->GetEnabledImageChannels();
            // 第二帧不等待完成：下一条命令直接 Stop PIE，继续覆盖 Pending 命令的退出安全性。
            Rig->SubmitCapture(PendingRequest);

            const TArray<FImageReadbackChannelStats> PendingStats = Rig->GetImageReadbackChannelStats();
            for (const FImageReadbackChannelStats& Stats : PendingStats)
            {
                Test->TestEqual(TEXT("Second capture increments the channel enqueue count"), Stats.EnqueuedCount, int64{2});
                Test->TestEqual(TEXT("Second capture remains pending before PIE Stop"), Stats.PendingCount, 1);
            }
            return true;
        }

        Test->AddError(TEXT("Lifecycle test Actor was not found in the PIE World."));
        return true;
    }

private:
    FAutomationTestBase* Test = nullptr;
    double StartSeconds = 0.0;
    int32 WarmupFrameCount = 0;
    bool bFirstCaptureSubmitted = false;
    int32 ExpectedPayloadCount = 0;
    int32 DeliveredPayloadCount = 0;
    bool bSawInstancePayload = false;
    TMap<uint32, int32> ObservedInstancePixelCounts;
    bool bSawFirstInternalInstanceId = false;
    bool bSawSecondInternalInstanceId = false;
    bool bSawNaniteInstanceId = false;
};
/** PIE 结束后切换到新关卡并同步渲染线程，模拟旧 World/RenderTarget 被整体释放。 */
class FSwitchMapAndFlushRenderingCommand final : public IAutomationLatentCommand
{
public:
    explicit FSwitchMapAndFlushRenderingCommand(FAutomationTestBase* InTest)
        : Test(InTest)
    {
    }

    virtual bool Update() override
    {
        UWorld* NewWorld = FAutomationEditorCommonUtils::CreateNewMap();
        Test->TestNotNull(TEXT("A replacement editor World is created"), NewWorld);

        // Flush 不是正常采集路径的一部分；测试中使用它来确保所有引用旧 World 的命令已经执行，
        // 从而把潜在的 use-after-free 稳定地暴露在本用例内，而不是泄漏到后续测试。
        FlushRenderingCommands();

        bool bFoundOldActor = false;
        if (NewWorld)
        {
            for (TActorIterator<AActor> It(NewWorld); It; ++It)
            {
                bFoundOldActor |= It->ActorHasTag(LifecycleActorTag);
            }
        }
        Test->TestFalse(TEXT("The replacement World does not retain the old sensor Actor"), bFoundOldActor);
        return true;
    }

private:
    FAutomationTestBase* Test = nullptr;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSensorCameraLifecycleTest,
    "SensorSimulation.Lifecycle.CameraRig.PIEStopAndMapChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSensorCameraLifecycleTest::RunTest(const FString& Parameters)
{
    // 验收项目的 Clean GameMode 不创建默认 Pawn；PIE 会记录一次预期的空 PawnClass Spawn 警告。
    // 将其声明为预期日志，避免与本测试真正关注的资源生命周期告警混在一起。
    AddExpectedError(
        TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains,
        1);

    UWorld* EditorWorld = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A temporary editor World is created"), EditorWorld))
    {
        return false;
    }

    AActor* CameraActor = EditorWorld->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("A lifecycle-test Camera Actor is spawned"), CameraActor))
    {
        return false;
    }
    CameraActor->Tags.Add(UE::SensorSimulation::Tests::LifecycleActorTag);

    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(
        CameraActor,
        TEXT("LifecycleCameraRig"),
        RF_Transactional);
    CameraActor->AddInstanceComponent(Rig);

    // 小分辨率保留真实 Capture/RenderTarget 生命周期，同时降低自动化测试的 GPU 成本。
    for (FCameraChannelConfig& Channel : Rig->Channels)
    {
        Channel.Resolution = FIntPoint(16, 16);
    }
    FCameraChannelConfig InstanceChannel;
    InstanceChannel.ChannelType = ECameraChannelType::Instance;
    InstanceChannel.Resolution = FIntPoint(16, 16);
    InstanceChannel.bForceLinearGamma = true;
    Rig->Channels.Add(InstanceChannel);
    Rig->RegisterComponent();

    // Cube 位于相机 +X 正前方；PIE 中会把它的实例号改成大于 255 的验收值。
    AActor* InstanceActor = EditorWorld->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("An Instance-test Actor is spawned"), InstanceActor))
    {
        return false;
    }
    InstanceActor->Tags.Add(UE::SensorSimulation::Tests::InstanceActorTag);
    UInstancedStaticMeshComponent* InstanceMesh =
        NewObject<UInstancedStaticMeshComponent>(InstanceActor, TEXT("InstanceMesh"));
    InstanceActor->SetRootComponent(InstanceMesh);
    InstanceActor->AddInstanceComponent(InstanceMesh);
    InstanceMesh->SetStaticMesh(
        LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    // 两个内部实例在 16x16 图像中左右分离，像素回读必须同时出现两个不同 uint32 ID。
    InstanceMesh->AddInstance(FTransform(FVector(0.0, -65.0, 0.0)));
    InstanceMesh->AddInstance(FTransform(FVector(0.0, 65.0, 0.0)));
    InstanceMesh->RegisterComponent();
    InstanceActor->SetActorLocation(FVector(300.0, 0.0, 0.0));

    USemanticObjectComponent* InstanceSemantic =
        NewObject<USemanticObjectComponent>(InstanceActor, TEXT("InstanceSemantic"));
    // Semantic 与 Instance 独立；设置合法类别只为避免本用例产生无关的 8 位标签错误日志。
    InstanceSemantic->SemanticId = 1;
    InstanceActor->AddInstanceComponent(InstanceSemantic);
    InstanceSemantic->RegisterComponent();
    TestEqual(
        TEXT("Actor ID plus two ISM instances reserve three collision-free IDs"),
        InstanceSemantic->GetRequiredInstanceIdCount(),
        3u);

    // Build a transient Nanite copy so the regression is self-contained and owns no content asset.
    UStaticMesh* SourceCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* NaniteCube = DuplicateObject<UStaticMesh>(
        SourceCube,
        GetTransientPackage(),
        TEXT("SensorSimulationNaniteRegressionCube"));
    if (!TestNotNull(TEXT("A transient Nanite regression mesh is duplicated"), NaniteCube))
    {
        return false;
    }
    FMeshNaniteSettings NaniteSettings = NaniteCube->GetNaniteSettings();
    NaniteSettings.bEnabled = true;
    NaniteCube->SetNaniteSettings(NaniteSettings);
    NaniteCube->Build(true);
    if (!TestTrue(TEXT("The transient regression mesh has valid Nanite data"), NaniteCube->HasValidNaniteData()))
    {
        return false;
    }

    AActor* NaniteActor = EditorWorld->SpawnActor<AActor>();
    if (!TestNotNull(TEXT("A Nanite-test Actor is spawned"), NaniteActor))
    {
        return false;
    }
    NaniteActor->Tags.Add(UE::SensorSimulation::Tests::NaniteActorTag);
    UStaticMeshComponent* NaniteMesh =
        NewObject<UStaticMeshComponent>(NaniteActor, TEXT("NaniteMesh"));
    NaniteActor->SetRootComponent(NaniteMesh);
    NaniteActor->AddInstanceComponent(NaniteMesh);
    NaniteMesh->SetStaticMesh(NaniteCube);
    NaniteMesh->RegisterComponent();
    // Keep Nanite pixels above the two ISM cubes so each render path is diagnosed independently.
    NaniteActor->SetActorLocation(FVector(300.0, 0.0, 150.0));

    USemanticObjectComponent* NaniteSemantic =
        NewObject<USemanticObjectComponent>(NaniteActor, TEXT("NaniteSemantic"));
    NaniteSemantic->SemanticId = 2;
    NaniteActor->AddInstanceComponent(NaniteSemantic);
    NaniteSemantic->RegisterComponent();

    TestTrue(TEXT("Camera Rig is registered"), Rig->IsRegistered());
    const FGuid RgbChannelGuid = Rig->Channels[0].ChannelGuid;
    const FGuid SemanticChannelGuid = Rig->Channels[1].ChannelGuid;
    const FGuid InstanceChannelGuid = Rig->Channels[2].ChannelGuid;
    TestNotNull(
        TEXT("RGB RenderTarget is created on registration"),
        Rig->GetChannelRenderTarget(RgbChannelGuid));
    TestNotNull(
        TEXT("Semantic RenderTarget is created on registration"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid));
    TestEqual(
        TEXT("Instance output uses native PF_R32_UINT"),
        Rig->GetChannelPixelFormat(InstanceChannelGuid),
        PF_R32_UINT);

    UTextureRenderTarget2D* InitialRgbTarget = Rig->GetChannelRenderTarget(RgbChannelGuid);
    UTextureRenderTarget2D* InitialSemanticTarget = Rig->GetChannelRenderTarget(SemanticChannelGuid);
    const FCalibration InitialCalibration = Rig->BuildCalibration(Rig->Channels[0]);
    const FCameraRigResourceStats InitialResourceStats = Rig->GetResourceStats();
    TestEqual(TEXT("Initial build creates three Capture components"), InitialResourceStats.CreatedCaptureComponents, int64{3});
    TestEqual(
        TEXT("Initial build creates four RenderTargets including Instance work target"),
        InitialResourceStats.CreatedRenderTargets,
        int64{4});
    TestTrue(TEXT("RGB channel receives a stable GUID"), Rig->Channels[0].ChannelGuid.IsValid());
    TestTrue(TEXT("Semantic channel receives a stable GUID"), Rig->Channels[1].ChannelGuid.IsValid());
    TestTrue(TEXT("Instance channel receives a stable GUID"), Rig->Channels[2].ChannelGuid.IsValid());
    TestTrue(TEXT("Each channel has a distinct GUID"),
        Rig->Channels[0].ChannelGuid != Rig->Channels[1].ChannelGuid &&
        Rig->Channels[0].ChannelGuid != Rig->Channels[2].ChannelGuid &&
        Rig->Channels[1].ChannelGuid != Rig->Channels[2].ChannelGuid);
    TestEqual(TEXT("Each active channel exposes independent calibration"),
        Rig->BuildActiveCalibrations().Num(), 3);

    Rig->MaxPendingReadbacks = 3;
    TestTrue(TEXT("Readback capacity change is detected"), Rig->ApplyConfiguration());
    TestEqual(TEXT("Readback capacity updates without replacing Manager"),
        Rig->GetImageReadbackStats().Capacity, 3);
    TestTrue(TEXT("Capacity-only update reuses RGB target"),
        Rig->GetChannelRenderTarget(RgbChannelGuid) == InitialRgbTarget);
    TestTrue(TEXT("Capacity-only update reuses Semantic target"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid) == InitialSemanticTarget);

    // 同一 ChannelType 的第二条配置必须获得独立资源，并能通过自己的 ChannelGuid 查询。
    FCameraChannelConfig DuplicateRgb = Rig->Channels[0];
    DuplicateRgb.ChannelGuid.Invalidate();
    DuplicateRgb.Resolution = FIntPoint(23, 13);
    Rig->Channels.Add(DuplicateRgb);
    TestTrue(TEXT("Second RGB ChannelGuid creates an independent runtime channel"), Rig->ApplyConfiguration());
    const FGuid SecondRgbChannelGuid = Rig->Channels.Last().ChannelGuid;
    TestTrue(TEXT("Duplicate-type config receives its own stable GUID"), SecondRgbChannelGuid.IsValid());
    TestTrue(TEXT("Two RGB configurations resolve to different RenderTargets"),
        Rig->GetChannelRenderTarget(SecondRgbChannelGuid) != nullptr &&
        Rig->GetChannelRenderTarget(SecondRgbChannelGuid) != Rig->GetChannelRenderTarget(RgbChannelGuid));
    TestEqual(TEXT("Both RGB ChannelGuids expose independent calibration"),
        Rig->BuildActiveCalibrations().Num(), 4);
    Rig->Channels.Pop();
    TestTrue(TEXT("Removing the second RGB ChannelGuid removes its runtime resource"), Rig->ApplyConfiguration());
    TestNull(TEXT("Removed RGB ChannelGuid no longer resolves a RenderTarget"),
        Rig->GetChannelRenderTarget(SecondRgbChannelGuid));

    Rig->HorizontalFovDegrees = 75.0f;
    TestTrue(TEXT("FOV change is detected as a configuration change"), Rig->ApplyConfiguration());
    TestTrue(TEXT("FOV-only update reuses the RGB RenderTarget"),
        Rig->GetChannelRenderTarget(RgbChannelGuid) == InitialRgbTarget);
    TestTrue(TEXT("FOV-only update reuses the Semantic RenderTarget"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid) == InitialSemanticTarget);
    TestTrue(TEXT("FOV update changes calibration focal length"),
        !FMath::IsNearlyEqual(InitialCalibration.Fx, Rig->BuildCalibration(Rig->Channels[0]).Fx));

    TestFalse(TEXT("Applying an unchanged configuration is a no-op"), Rig->ApplyConfiguration());
    const FCameraRigResourceStats NoOpStats = Rig->GetResourceStats();
    TestEqual(TEXT("No-op update does not rebuild RenderTargets"), NoOpStats.RebuiltRenderTargets, int64{0});
    TestTrue(TEXT("No-op update is observable in resource metrics"), NoOpStats.NoOpConfigurationApplyCount > 0);

    Rig->Channels[0].Resolution = FIntPoint(17, 9);
    TestTrue(TEXT("Odd RGB resolution change is applied"), Rig->ApplyConfiguration());
    UTextureRenderTarget2D* ResizedRgbTarget = Rig->GetChannelRenderTarget(RgbChannelGuid);
    TestTrue(TEXT("Resolution update replaces only the affected RGB RenderTarget"),
        ResizedRgbTarget != nullptr && ResizedRgbTarget != InitialRgbTarget);
    TestTrue(TEXT("Unchanged Semantic channel remains on its original RenderTarget"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid) == InitialSemanticTarget);
    TestEqual(TEXT("Only the affected RGB GPU target is rebuilt"), Rig->GetResourceStats().RebuiltRenderTargets, int64{1});

    Rig->Channels[0].bForceLinearGamma = true;
    TestTrue(TEXT("RGB Gamma change is applied"), Rig->ApplyConfiguration());
    TestTrue(TEXT("Gamma update replaces RGB but still reuses its Capture component"),
        Rig->GetChannelRenderTarget(RgbChannelGuid) != ResizedRgbTarget);
    TestTrue(TEXT("Gamma update does not rebuild Semantic"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid) == InitialSemanticTarget);
    TestEqual(TEXT("Resolution and Gamma each rebuild one affected target"),
        Rig->GetResourceStats().RebuiltRenderTargets, int64{2});

    UTextureRenderTarget2D* HotReloadedRgbTarget = Rig->GetChannelRenderTarget(RgbChannelGuid);
    Rig->Channels[1].bEnabled = false;
    TestTrue(TEXT("Disabling Semantic removes only that channel"), Rig->ApplyConfiguration());
    TestNull(TEXT("Disabled Semantic no longer exposes a RenderTarget"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid));
    TestTrue(TEXT("Disabling Semantic keeps the hot-reloaded RGB target alive"),
        Rig->GetChannelRenderTarget(RgbChannelGuid) == HotReloadedRgbTarget);

    Rig->Channels[1].bEnabled = true;
    TestTrue(TEXT("Re-enabling Semantic creates the missing channel"), Rig->ApplyConfiguration());
    TestNotNull(TEXT("Re-enabled Semantic receives a new RenderTarget"),
        Rig->GetChannelRenderTarget(SemanticChannelGuid));
    TestTrue(TEXT("Re-enabling Semantic still reuses the hot-reloaded RGB target"),
        Rig->GetChannelRenderTarget(RgbChannelGuid) == HotReloadedRgbTarget);
    TestEqual(TEXT("Second RGB removal and Semantic disable destroy two Captures"),
        Rig->GetResourceStats().DestroyedCaptureComponents, int64{2});

    // 覆盖组件禁用/启用或 Actor 重建时会发生的 OnUnregister -> OnRegister 循环。
    Rig->UnregisterComponent();
    TestFalse(TEXT("Camera Rig can unregister cleanly"), Rig->IsRegistered());
    TestNull(
        TEXT("Runtime channels are released on unregister"),
        Rig->GetChannelRenderTarget(RgbChannelGuid));

    Rig->RegisterComponent();
    TestTrue(TEXT("Camera Rig can register again"), Rig->IsRegistered());
    TestNotNull(
        TEXT("Runtime channels are rebuilt after re-registration"),
        Rig->GetChannelRenderTarget(RgbChannelGuid));

    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::Tests::FSubmitCaptureInPIECommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::Tests::FSwitchMapAndFlushRenderingCommand(this));
    return true;
}

#endif
