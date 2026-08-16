#include "CameraRigComponent.h"
#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

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

namespace UE::SensorSimulation::OpaqueProxyIsolationTests
{
static const FName CameraTag(TEXT("OpaqueProxyIsolationCamera"));
static const FName ProxyActorTag(TEXT("OpaqueProxyIsolationProxy"));
static const FName BackingActorTag(TEXT("OpaqueProxyIsolationBacking"));
static constexpr uint8 ProxySemanticId = 50;
static constexpr uint8 BackingSemanticId = 51;
static constexpr uint32 ProxyInstanceId = 0x01041000u;
static constexpr uint32 BackingInstanceId = 0x01042000u;

/** 创建颜色可区分的 BasicShape 材质实例，供 RGB 中检测代理是否泄漏。 */
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

/** 创建真实透明源；标签路径必须忽略它，视觉路径仍按普通透明物体处理。 */
UMaterial* CreateTranslucentSourceMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(), TEXT("OpaqueProxyIsolationTranslucent"), RF_Transient);
    Material->BlendMode = BLEND_Translucent;
    Material->TwoSided = true;
    Material->PostEditChange();
    return Material;
}

/** 从紧密 Payload 中取得中心像素的字节偏移。 */
int32 GetCenterOffset(const FImagePayload& Payload)
{
    return ((Payload.ImageSize.Y / 2) * Payload.ImageSize.X + Payload.ImageSize.X / 2) * 4;
}

/** 执行 OpaqueProxy 帧和 Ignore 帧，并验证四模态中心像素。 */
class FCaptureHotSwitchCommand final : public IAutomationLatentCommand
{
public:
    explicit FCaptureHotSwitchCommand(FAutomationTestBase* InTest)
        : Test(InTest), StartSeconds(FPlatformTime::Seconds())
    {
    }

    virtual bool Update() override
    {
        UWorld* PieWorld = GEditor ? GEditor->PlayWorld : nullptr;
        if (!PieWorld)
        {
            return WaitOrFail(TEXT("PIE World was not created within 45 seconds."));
        }

        UCameraRigComponent* Rig = nullptr;
        USemanticObjectComponent* ProxySemantic = nullptr;
        USemanticObjectComponent* BackingSemantic = nullptr;
        for (TActorIterator<AActor> It(PieWorld); It; ++It)
        {
            if (It->ActorHasTag(CameraTag))
            {
                Rig = It->FindComponentByClass<UCameraRigComponent>();
            }
            else if (It->ActorHasTag(ProxyActorTag))
            {
                ProxySemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
            else if (It->ActorHasTag(BackingActorTag))
            {
                BackingSemantic = It->FindComponentByClass<USemanticObjectComponent>();
            }
        }
        if (!Rig || !ProxySemantic || !BackingSemantic)
        {
            Test->AddError(TEXT("OpaqueProxy isolation PIE objects are missing."));
            return true;
        }

        if (!bIdsAssigned)
        {
            if (WarmupFrames++ < 5)
            {
                return false;
            }
            ProxySemantic->SetAssignedInstanceId(ProxyInstanceId, 1);
            BackingSemantic->SetAssignedInstanceId(BackingInstanceId, 1);
            bIdsAssigned = true;
        }

        if (Phase == 0)
        {
            Submit(*Rig, 4101);
            Phase = 1;
            return false;
        }
        if (Phase == 1 && CollectAndValidate(*Rig, 4101, true))
        {
            ProxySemantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::Ignore;
            ProxySemantic->ApplyCaptureConfiguration();
            // 给 CustomDepth、Instance Registry 和代理隐藏列表一个 Tick 完成热切换传播。
            Phase = 2;
            SwitchWarmupFrames = 0;
            return false;
        }
        if (Phase == 2)
        {
            if (SwitchWarmupFrames++ < 2)
            {
                return false;
            }
            Submit(*Rig, 4102);
            Phase = 3;
            return false;
        }
        if (Phase == 3 && CollectAndValidate(*Rig, 4102, false))
        {
            Test->TestTrue(TEXT("Configured proxy remains capture-only after switching to Ignore"),
                ProxySemantic->OpaqueLabelProxy &&
                ProxySemantic->OpaqueLabelProxy->bVisibleInSceneCaptureOnly != 0);
            return true;
        }
        return WaitOrFail(TEXT("OpaqueProxy hot-switch Payloads did not complete within 45 seconds."));
    }

private:
    void Submit(UCameraRigComponent& Rig, const uint64 FrameId)
    {
        FCaptureRequest Request;
        Request.Header.FrameId = FrameId;
        Request.Header.SequenceId = 1;
        Request.SensorName = Rig.SensorName;
        Request.SensorGuid = FGuid(0x410, 0x420, 0x430, 0x440);
        Request.ExpectedPayloads = Rig.GetEnabledPayloadTypes();
        Request.ExpectedImageChannels = Rig.GetEnabledImageChannels();
        Test->TestEqual(TEXT("Hot-switch capture is accepted"),
            Rig.SubmitCapture(Request), ECaptureRequestResult::Accepted);
        Received.Reset();
    }

    bool CollectAndValidate(UCameraRigComponent& Rig, const uint64 FrameId, const bool bExpectProxyLabel)
    {
        FImagePayload Payload;
        while (Rig.PollCompletedImage(Payload))
        {
            if (Payload.Header.FrameId != FrameId || Received.Contains(Payload.PayloadType))
            {
                continue;
            }
            Received.Add(Payload.PayloadType);
            const int32 Offset = GetCenterOffset(Payload);
            if (Offset + 3 >= Payload.Bytes.Num())
            {
                Test->AddError(TEXT("Hot-switch Payload has no center pixel."));
                continue;
            }

            if (Payload.PayloadType == EPayloadType::Rgb)
            {
                const uint8 R = Payload.Bytes[Offset];
                const uint8 G = Payload.Bytes[Offset + 1];
                // 代理使用红色材质；只要红色没有压倒其他通道，就证明它没有进入 RGB Capture。
                Test->TestFalse(TEXT("Opaque label proxy does not contaminate RGB"),
                    R > 80 && R > static_cast<int32>(G) + 30);
            }
            else if (Payload.PayloadType == EPayloadType::Depth)
            {
                float DepthMeters = 0.0f;
                FMemory::Memcpy(&DepthMeters, Payload.Bytes.GetData() + Offset, sizeof(float));
                // 代理前表面约 1.8 m，后方物体前表面约 3.0 m。
                Test->TestTrue(TEXT("Depth sees the backing object instead of the label proxy"),
                    FMath::IsFinite(DepthMeters) && DepthMeters > 2.5f);
            }
            else if (Payload.PayloadType == EPayloadType::Semantic)
            {
                Test->TestEqual(TEXT("Semantic center follows the active translucent policy"),
                    Payload.Bytes[Offset], bExpectProxyLabel ? ProxySemanticId : BackingSemanticId);
            }
            else if (Payload.PayloadType == EPayloadType::Instance)
            {
                uint32 InstanceId = 0;
                FMemory::Memcpy(&InstanceId, Payload.Bytes.GetData() + Offset, sizeof(uint32));
                Test->TestEqual(TEXT("Instance center follows the active translucent policy"),
                    InstanceId, bExpectProxyLabel ? ProxyInstanceId : BackingInstanceId);
            }
        }
        return Received.Num() == 4;
    }

    bool WaitOrFail(const TCHAR* Error)
    {
        if (FPlatformTime::Seconds() - StartSeconds < 45.0)
        {
            return false;
        }
        Test->AddError(Error);
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double StartSeconds = 0.0;
    int32 WarmupFrames = 0;
    int32 SwitchWarmupFrames = 0;
    int32 Phase = 0;
    bool bIdsAssigned = false;
    TSet<EPayloadType> Received;
};

class FFlushIsolationCommand final : public IAutomationLatentCommand
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
    FOpaqueProxyHotSwitchIsolationTest,
    "SensorSimulation.Rendering.OpaqueProxy.HotSwitchIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpaqueProxyHotSwitchIsolationTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains, 1);
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("OpaqueProxy isolation World is created"), World))
    {
        return false;
    }
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

    AActor* CameraActor = World->SpawnActor<AActor>();
    CameraActor->Tags.Add(UE::SensorSimulation::OpaqueProxyIsolationTests::CameraTag);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(CameraActor, TEXT("OpaqueProxyIsolationRig"));
    CameraActor->AddInstanceComponent(Rig);
    Rig->SensorName = TEXT("OpaqueProxyIsolationCamera");
    Rig->MaxPendingReadbacks = 8;
    Rig->Channels.Empty();
    for (const ECameraChannelType Type : { ECameraChannelType::Rgb, ECameraChannelType::Semantic,
        ECameraChannelType::Depth, ECameraChannelType::Instance })
    {
        FCameraChannelConfig& Channel = Rig->Channels.AddDefaulted_GetRef();
        Channel.ChannelType = Type;
        Channel.Resolution = FIntPoint(32, 24);
        Channel.bForceLinearGamma = Type == ECameraChannelType::Semantic || Type == ECameraChannelType::Instance;
    }
    Rig->RegisterComponent();

    UMaterial* Translucent =
        UE::SensorSimulation::OpaqueProxyIsolationTests::CreateTranslucentSourceMaterial();
    AActor* ProxyActor = World->SpawnActor<AActor>();
    ProxyActor->Tags.Add(UE::SensorSimulation::OpaqueProxyIsolationTests::ProxyActorTag);
    UStaticMeshComponent* SourceMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("GlassSource"));
    ProxyActor->SetRootComponent(SourceMesh);
    ProxyActor->AddInstanceComponent(SourceMesh);
    SourceMesh->SetStaticMesh(Cube);
    SourceMesh->SetMaterial(0, Translucent);
    SourceMesh->SetWorldScale3D(FVector(2.0));
    SourceMesh->RegisterComponent();
    UStaticMeshComponent* ProxyMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("GlassLabelProxy"));
    ProxyMesh->SetupAttachment(SourceMesh);
    ProxyActor->AddInstanceComponent(ProxyMesh);
    ProxyMesh->SetStaticMesh(Cube);
    ProxyMesh->SetMaterial(0, UE::SensorSimulation::OpaqueProxyIsolationTests::CreateColoredMaterial(
        ProxyActor, FLinearColor::Red));
    ProxyMesh->SetWorldScale3D(FVector(1.0));
    ProxyMesh->RegisterComponent();
    ProxyActor->SetActorLocation(FVector(280.0, 0.0, 0.0));
    USemanticObjectComponent* ProxySemantic = NewObject<USemanticObjectComponent>(ProxyActor, TEXT("GlassSemantic"));
    ProxySemantic->SemanticId = UE::SensorSimulation::OpaqueProxyIsolationTests::ProxySemanticId;
    ProxySemantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::OpaqueProxy;
    ProxySemantic->OpaqueLabelProxy = ProxyMesh;
    ProxyActor->AddInstanceComponent(ProxySemantic);
    ProxySemantic->RegisterComponent();

    AActor* BackingActor = World->SpawnActor<AActor>();
    BackingActor->Tags.Add(UE::SensorSimulation::OpaqueProxyIsolationTests::BackingActorTag);
    UStaticMeshComponent* BackingMesh = NewObject<UStaticMeshComponent>(BackingActor, TEXT("BackingMesh"));
    BackingActor->SetRootComponent(BackingMesh);
    BackingActor->AddInstanceComponent(BackingMesh);
    BackingMesh->SetStaticMesh(Cube);
    BackingMesh->SetMaterial(0, UE::SensorSimulation::OpaqueProxyIsolationTests::CreateColoredMaterial(
        BackingActor, FLinearColor::Green));
    BackingMesh->SetWorldScale3D(FVector(1.2));
    BackingMesh->RegisterComponent();
    BackingActor->SetActorLocation(FVector(360.0, 0.0, 0.0));
    USemanticObjectComponent* BackingSemantic = NewObject<USemanticObjectComponent>(BackingActor, TEXT("BackingSemantic"));
    BackingSemantic->SemanticId = UE::SensorSimulation::OpaqueProxyIsolationTests::BackingSemanticId;
    BackingActor->AddInstanceComponent(BackingSemantic);
    BackingSemantic->RegisterComponent();

    ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::OpaqueProxyIsolationTests::FCaptureHotSwitchCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::OpaqueProxyIsolationTests::FFlushIsolationCommand());
    return true;
}

#endif
