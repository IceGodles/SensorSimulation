#include "CameraRigComponent.h"
#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionStep.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::SpecialObjectTests
{
/** 用于在 PIE World 中重新找到本测试创建的 Camera Rig。 */
static const FName CameraTag(TEXT("SensorSimulationSpecialObjectCamera"));
/** 用于识别独立 HISM Actor。 */
static const FName HismTag(TEXT("SensorSimulationSpecialObjectHISM"));
/** 用于识别使用 Masked 材质的 foliage 风格 HISM Actor。 */
static const FName MaskedFoliageTag(TEXT("SensorSimulationSpecialObjectMaskedFoliage"));
/** 用于识别 Masked 几何孔洞后方的不透明遮挡验证对象。 */
static const FName MaskedBackingTag(TEXT("SensorSimulationSpecialObjectMaskedBacking"));
/** 用于识别 SkeletalMesh Actor。 */
static const FName SkeletalTag(TEXT("SensorSimulationSpecialObjectSkeletal"));
/** 用于识别按产品策略排除的 Translucent Actor。 */
static const FName TranslucentTag(TEXT("SensorSimulationSpecialObjectTranslucent"));
/** 用于识别 Translucent 后方应保持可见的不透明对象。 */
static const FName TranslucentBackingTag(TEXT("SensorSimulationSpecialObjectTranslucentBacking"));
/** 用于识别由 OpaqueProxy 表示的透明对象。 */
static const FName OpaqueProxyTag(TEXT("SensorSimulationSpecialObjectOpaqueProxy"));
/** 用于识别 OpaqueProxy 后方应被代理遮挡的对象。 */
static const FName OpaqueProxyBackingTag(TEXT("SensorSimulationSpecialObjectOpaqueProxyBacking"));

/** HISM Actor 自身占用 Base，两个内部实例依次使用 Base+1 和 Base+2。 */
static constexpr uint32 HismBaseId = 0x01021000u;
/** Masked foliage Actor 自身占用 Base，唯一内部实例使用 Base+1。 */
static constexpr uint32 MaskedFoliageBaseId = 0x01022000u;
/** Masked 孔洞后方对象使用的独立 ID。 */
static constexpr uint32 MaskedBackingId = 0x01023000u;
/** SkeletalMesh 组件级 ID。 */
static constexpr uint32 SkeletalId = 0x01024000u;
/** 按产品规则应从 Instance 输出排除的 Translucent ID。 */
static constexpr uint32 TranslucentId = 0x01025000u;
/** Translucent 后方不透明对象的独立 ID。 */
static constexpr uint32 TranslucentBackingId = 0x01026000u;
/** OpaqueProxy 策略下透明对象自身的 ID。 */
static constexpr uint32 OpaqueProxyId = 0x01027000u;
/** OpaqueProxy 后方验证对象的 ID。 */
static constexpr uint32 OpaqueProxyBackingId = 0x01028000u;


/**
 * 创建只用于本次验收的 Masked 材质。
 *
 * UV.x >= 0.5 的右半区域不透明，左半区域透明。纯解析表达式不依赖临时纹理
 * 在 PIE 复制时的资源状态，因此 D3D11/D3D12 使用完全相同的确定性输入。
 * Instance Pass 必须沿用该材质的 OpacityMask；如果错误地退回实心默认材质，
 * 后方 Cube 的 ID 会完全消失，像素断言会失败。
 */
UMaterial* CreateMaskedFoliageMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(),
        TEXT("SensorSimulationSpecialObjectMaskedMaterial"),
        RF_Transient);
    Material->BlendMode = BLEND_Masked;
    Material->TwoSided = true;
    Material->OpacityMaskClipValue = 0.5f;

    UMaterialExpressionTextureCoordinate* TexCoord =
        NewObject<UMaterialExpressionTextureCoordinate>(Material);
    UMaterialExpressionComponentMask* UChannel =
        NewObject<UMaterialExpressionComponentMask>(Material);
    TexCoord->Material = Material;
    UChannel->Material = Material;
    UChannel->R = true;
    UChannel->G = false;
    UChannel->B = false;
    UChannel->A = false;
    UChannel->Input.Expression = TexCoord;
    Material->GetExpressionCollection().AddExpression(TexCoord);
    Material->GetExpressionCollection().AddExpression(UChannel);
    // UV.x 覆盖 0..1，配合 0.5 ClipValue 产生确定性的保留区与孔洞区。
    Material->GetEditorOnlyData()->OpacityMask.Expression = UChannel;

    Material->PostEditChange();
    Material->ForceRecompileForRendering();
    return Material;
}

/** 创建用于验证“Translucent 不产生单标签”的临时材质。 */
UMaterial* CreateTranslucentMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(),
        TEXT("SensorSimulationSpecialObjectTranslucentMaterial"),
        RF_Transient);
    Material->BlendMode = BLEND_Translucent;
    Material->TwoSided = true;
    Material->PostEditChange();
    return Material;
}
/** 为 Actor 添加语义组件，使其 Primitive 同时登记到 32 位 Instance Registry。 */
USemanticObjectComponent* AddSemanticComponent(AActor& Actor, const TCHAR* Name, int32 SemanticId)
{
    USemanticObjectComponent* Semantic =
        NewObject<USemanticObjectComponent>(&Actor, FName(Name));
    Semantic->SemanticId = SemanticId;
    Actor.AddInstanceComponent(Semantic);
    Semantic->RegisterComponent();
    return Semantic;
}

/** 在 PIE 中触发一次 Instance Capture，并检查全部特殊对象的真实 uint32 像素。 */
class FCaptureSpecialObjectsCommand final : public IAutomationLatentCommand
{
public:
    explicit FCaptureSpecialObjectsCommand(FAutomationTestBase* InTest)
        : Test(InTest)
        , StartSeconds(FPlatformTime::Seconds())
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
        for (TActorIterator<AActor> It(PieWorld); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->ActorHasTag(CameraTag))
            {
                Rig = Actor->FindComponentByClass<UCameraRigComponent>();
            }

            if (!bIdsAssigned)
            {
                if (USemanticObjectComponent* Semantic =
                    Actor->FindComponentByClass<USemanticObjectComponent>())
                {
                    if (Actor->ActorHasTag(HismTag))
                    {
                        Semantic->SetAssignedInstanceId(HismBaseId, 3);
                    }
                    else if (Actor->ActorHasTag(MaskedFoliageTag))
                    {
                        Semantic->SetAssignedInstanceId(MaskedFoliageBaseId, 2);
                    }
                    else if (Actor->ActorHasTag(MaskedBackingTag))
                    {
                        Semantic->SetAssignedInstanceId(MaskedBackingId, 1);
                    }
                    else if (Actor->ActorHasTag(SkeletalTag))
                    {
                        Semantic->SetAssignedInstanceId(SkeletalId, 1);
                    }
                    else if (Actor->ActorHasTag(TranslucentTag))
                    {
                        Semantic->SetAssignedInstanceId(TranslucentId, 1);
                    }
                    else if (Actor->ActorHasTag(TranslucentBackingTag))
                    {
                        Semantic->SetAssignedInstanceId(TranslucentBackingId, 1);
                    }
                    else if (Actor->ActorHasTag(OpaqueProxyTag))
                    {
                        Semantic->SetAssignedInstanceId(OpaqueProxyId, 1);
                    }
                    else if (Actor->ActorHasTag(OpaqueProxyBackingTag))
                    {
                        Semantic->SetAssignedInstanceId(OpaqueProxyBackingId, 1);
                    }
                }
            }
        }
        bIdsAssigned = true;

        if (!Rig)
        {
            Test->AddError(TEXT("Special-object Camera Rig was not duplicated into PIE."));
            return true;
        }

        // HISM、Skeletal dynamic batches、材质 Shader 和 GPU Scene 数据均需要若干帧完成上传。
        if (!bCaptureSubmitted)
        {
            if (WarmupFrames++ < 10)
            {
                return false;
            }

            FCaptureRequest Request;
            Request.Header.FrameId = 1001;
            Request.Header.SequenceId = 15;
            Request.SensorName = Rig->SensorName;
            Request.SensorGuid = FGuid(15, 1, 5, 7);
            Request.ExpectedPayloads = Rig->GetEnabledPayloadTypes();
            Request.ExpectedImageChannels = Rig->GetEnabledImageChannels();
            Test->TestEqual(
                TEXT("Special-object Rig exposes exactly one Instance channel"),
                Request.ExpectedImageChannels.Num(),
                1);
            Test->TestEqual(
                TEXT("Special-object capture request is accepted"),
                Rig->SubmitCapture(Request),
                ECaptureRequestResult::Accepted);
            bCaptureSubmitted = true;
            return false;
        }

        FImagePayload Payload;
        while (Rig->PollCompletedImage(Payload))
        {
            if (Payload.Header.FrameId != 1001 || Payload.PayloadType != EPayloadType::Instance)
            {
                continue;
            }

            Test->TestEqual(TEXT("Special-object payload is R32Uint"), Payload.PixelFormat, EImagePixelFormat::R32Uint);
            Test->TestEqual(TEXT("Special-object payload uses identifier units"), Payload.ValueUnit, EImageValueUnit::Identifier);

            TMap<uint32, int32> PixelCounts;
            for (int32 Offset = 0;
                Offset + static_cast<int32>(sizeof(uint32)) <= Payload.Bytes.Num();
                Offset += sizeof(uint32))
            {
                uint32 InstanceId = 0;
                FMemory::Memcpy(&InstanceId, Payload.Bytes.GetData() + Offset, sizeof(uint32));
                if (InstanceId != 0)
                {
                    PixelCounts.FindOrAdd(InstanceId)++;
                }
            }

            FString ObservedIds;
            for (const TPair<uint32, int32>& Pair : PixelCounts)
            {
                ObservedIds += FString::Printf(TEXT("%u:%d "), Pair.Key, Pair.Value);
            }
            Test->AddInfo(FString::Printf(
                TEXT("Special-object Instance pixels (ID:count): %s"),
                *ObservedIds));

            Test->TestTrue(TEXT("HISM internal instance 0 has its own ID"), PixelCounts.Contains(HismBaseId + 1u));
            Test->TestTrue(TEXT("HISM internal instance 1 has its own ID"), PixelCounts.Contains(HismBaseId + 2u));
            Test->TestTrue(TEXT("Masked foliage keeps opaque alpha regions"), PixelCounts.Contains(MaskedFoliageBaseId + 1u));
            Test->TestTrue(TEXT("Masked foliage holes reveal the backing object's ID"), PixelCounts.Contains(MaskedBackingId));
            Test->TestTrue(TEXT("SkeletalMesh dynamic path writes its component ID"), PixelCounts.Contains(SkeletalId));
            Test->TestFalse(TEXT("Translucent product policy excludes the foreground ID"), PixelCounts.Contains(TranslucentId));
            Test->TestTrue(TEXT("Translucent exclusion reveals the backing object ID"), PixelCounts.Contains(TranslucentBackingId));
            Test->TestTrue(TEXT("OpaqueProxy writes the translucent object's ID"), PixelCounts.Contains(OpaqueProxyId));
            Test->TestFalse(TEXT("OpaqueProxy occludes its backing object"), PixelCounts.Contains(OpaqueProxyBackingId));
            return true;
        }

        return WaitOrFail(TEXT("Special-object Instance payload did not complete within 45 seconds."));
    }

private:
    /** 超时前继续等待；超时后为 Automation Test 记录确定性错误。 */
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
    bool bIdsAssigned = false;
    bool bCaptureSubmitted = false;
};

/** PIE 结束后 Flush 渲染线程，确保临时材质、纹理和 SceneProxy 不泄漏到后续矩阵项。 */
class FFlushSpecialObjectRenderingCommand final : public IAutomationLatentCommand
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
    FSpecialObjectCrossRhiTest,
    "SensorSimulation.Rendering.SpecialObjects.CrossRHI",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSpecialObjectCrossRhiTest::RunTest(const FString& Parameters)
{
    AddExpectedError(
        TEXT("SpawnActor failed because no class was specified"),
        EAutomationExpectedErrorFlags::Contains,
        1);

    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("A special-object test World is created"), World))
    {
        return false;
    }

    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    USkeletalMesh* SkeletalCube = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!TestNotNull(TEXT("Engine Cube is available"), Cube) ||
        !TestNotNull(TEXT("Engine SkeletalCube is available"), SkeletalCube))
    {
        return false;
    }

    AActor* CameraActor = World->SpawnActor<AActor>();
    CameraActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::CameraTag);
    UCameraRigComponent* Rig = NewObject<UCameraRigComponent>(CameraActor, TEXT("SpecialObjectCameraRig"));
    CameraActor->AddInstanceComponent(Rig);
    Rig->SensorName = TEXT("SpecialObjectCamera");
    Rig->HorizontalFovDegrees = 90.0f;
    Rig->Channels.Empty();
    FCameraChannelConfig InstanceChannel;
    InstanceChannel.ChannelType = ECameraChannelType::Instance;
    InstanceChannel.Resolution = FIntPoint(64, 64);
    InstanceChannel.bForceLinearGamma = true;
    Rig->Channels.Add(InstanceChannel);
    Rig->RegisterComponent();

    // 独立 HISM：两个内部实例置于画面下方，分别产生 Base+1 和 Base+2。
    AActor* HismActor = World->SpawnActor<AActor>();
    HismActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::HismTag);
    UHierarchicalInstancedStaticMeshComponent* Hism =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(HismActor, TEXT("SpecialObjectHISM"));
    HismActor->SetRootComponent(Hism);
    HismActor->AddInstanceComponent(Hism);
    Hism->SetStaticMesh(Cube);
    Hism->AddInstance(FTransform(FVector(0.0, -70.0, 0.0)));
    Hism->AddInstance(FTransform(FVector(0.0, 70.0, 0.0)));
    Hism->RegisterComponent();
    HismActor->SetActorLocation(FVector(400.0, 0.0, -150.0));
    USemanticObjectComponent* HismSemantic = UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *HismActor, TEXT("HismSemantic"), 10);
    TestEqual(TEXT("HISM reserves Actor plus two internal IDs"), HismSemantic->GetRequiredInstanceIdCount(), 3u);

    // Masked foliage 风格 HISM 位于不透明 Cube 前方，UV.x 裁剪孔洞必须露出后方 ID。
    UMaterial* MaskMaterial = UE::SensorSimulation::SpecialObjectTests::CreateMaskedFoliageMaterial();
    if (!TestNotNull(TEXT("Transient analytic Masked material is created"), MaskMaterial))
    {
        return false;
    }

    AActor* MaskedActor = World->SpawnActor<AActor>();
    MaskedActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::MaskedFoliageTag);
    UHierarchicalInstancedStaticMeshComponent* MaskedFoliage =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(MaskedActor, TEXT("MaskedFoliageHISM"));
    MaskedActor->SetRootComponent(MaskedFoliage);
    MaskedActor->AddInstanceComponent(MaskedFoliage);
    MaskedFoliage->SetStaticMesh(Cube);
    MaskedFoliage->SetMaterial(0, MaskMaterial);
    MaskedFoliage->AddInstance(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(1.4)));
    MaskedFoliage->RegisterComponent();
    MaskedActor->SetActorLocation(FVector(280.0, 150.0, 100.0));
    USemanticObjectComponent* MaskedSemantic = UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *MaskedActor, TEXT("MaskedFoliageSemantic"), 20);
    TestEqual(TEXT("Masked foliage reserves Actor plus one internal ID"), MaskedSemantic->GetRequiredInstanceIdCount(), 2u);

    AActor* BackingActor = World->SpawnActor<AActor>();
    BackingActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::MaskedBackingTag);
    UStaticMeshComponent* BackingMesh = NewObject<UStaticMeshComponent>(BackingActor, TEXT("MaskedBackingMesh"));
    BackingActor->SetRootComponent(BackingMesh);
    BackingActor->AddInstanceComponent(BackingMesh);
    BackingMesh->SetStaticMesh(Cube);
    BackingMesh->SetWorldScale3D(FVector(1.4));
    BackingMesh->RegisterComponent();
    BackingActor->SetActorLocation(FVector(340.0, 150.0, 100.0));
    UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *BackingActor, TEXT("MaskedBackingSemantic"), 21);

    // SkeletalCube 沿 DynamicMeshElements 路径进入 Instance Pass。
    AActor* SkeletalActor = World->SpawnActor<AActor>();
    SkeletalActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::SkeletalTag);
    USkeletalMeshComponent* SkeletalMesh =
        NewObject<USkeletalMeshComponent>(SkeletalActor, TEXT("SpecialObjectSkeletalMesh"));
    SkeletalActor->SetRootComponent(SkeletalMesh);
    SkeletalActor->AddInstanceComponent(SkeletalMesh);
    SkeletalMesh->SetSkeletalMeshAsset(SkeletalCube);
    SkeletalMesh->RegisterComponent();
    SkeletalActor->SetActorLocation(FVector(350.0, -170.0, 110.0));
    UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *SkeletalActor, TEXT("SkeletalSemantic"), 30);

    // Translucent 产品策略：前景透明 Cube 不输出单一标签，后方 Opaque Cube 必须保持可见。
    UMaterial* TranslucentMaterial = UE::SensorSimulation::SpecialObjectTests::CreateTranslucentMaterial();
    AActor* TranslucentActor = World->SpawnActor<AActor>();
    TranslucentActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::TranslucentTag);
    UStaticMeshComponent* TranslucentMesh = NewObject<UStaticMeshComponent>(TranslucentActor, TEXT("SpecialObjectTranslucentMesh"));
    TranslucentActor->SetRootComponent(TranslucentMesh);
    TranslucentActor->AddInstanceComponent(TranslucentMesh);
    TranslucentMesh->SetStaticMesh(Cube);
    TranslucentMesh->SetMaterial(0, TranslucentMaterial);
    TranslucentMesh->SetWorldScale3D(FVector(1.2));
    TranslucentMesh->RegisterComponent();
    TranslucentActor->SetActorLocation(FVector(280.0, -170.0, -90.0));
    UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *TranslucentActor, TEXT("TranslucentSemantic"), 40);

    AActor* TranslucentBackingActor = World->SpawnActor<AActor>();
    TranslucentBackingActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::TranslucentBackingTag);
    UStaticMeshComponent* TranslucentBackingMesh = NewObject<UStaticMeshComponent>(TranslucentBackingActor, TEXT("TranslucentBackingMesh"));
    TranslucentBackingActor->SetRootComponent(TranslucentBackingMesh);
    TranslucentBackingActor->AddInstanceComponent(TranslucentBackingMesh);
    TranslucentBackingMesh->SetStaticMesh(Cube);
    TranslucentBackingMesh->SetWorldScale3D(FVector(1.2));
    TranslucentBackingMesh->RegisterComponent();
    TranslucentBackingActor->SetActorLocation(FVector(340.0, -170.0, -90.0));
    UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *TranslucentBackingActor, TEXT("TranslucentBackingSemantic"), 41);
    // OpaqueProxy：真实透明 Cube 继续被忽略；同 Actor 的不透明代理代表玻璃本体标签。
    AActor* ProxyActor = World->SpawnActor<AActor>();
    ProxyActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::OpaqueProxyTag);
    UStaticMeshComponent* ProxyTranslucentMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("OpaqueProxySourceTranslucent"));
    ProxyActor->SetRootComponent(ProxyTranslucentMesh);
    ProxyActor->AddInstanceComponent(ProxyTranslucentMesh);
    ProxyTranslucentMesh->SetStaticMesh(Cube);
    ProxyTranslucentMesh->SetMaterial(0, TranslucentMaterial);
    ProxyTranslucentMesh->SetWorldScale3D(FVector(1.0));
    ProxyTranslucentMesh->RegisterComponent();
    UStaticMeshComponent* LabelProxyMesh = NewObject<UStaticMeshComponent>(ProxyActor, TEXT("OpaqueLabelProxyMesh"));
    LabelProxyMesh->SetupAttachment(ProxyTranslucentMesh);
    ProxyActor->AddInstanceComponent(LabelProxyMesh);
    LabelProxyMesh->SetStaticMesh(Cube);
    LabelProxyMesh->SetWorldScale3D(FVector(1.0));
    LabelProxyMesh->RegisterComponent();
    ProxyActor->SetActorLocation(FVector(280.0, 0.0, 100.0));
    USemanticObjectComponent* ProxySemantic = UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *ProxyActor, TEXT("OpaqueProxySemantic"), 50);
    ProxySemantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::OpaqueProxy;
    ProxySemantic->OpaqueLabelProxy = LabelProxyMesh;
    ProxySemantic->SetAssignedInstanceId(UE::SensorSimulation::SpecialObjectTests::OpaqueProxyId, 1);
    TestTrue(TEXT("OpaqueProxy configuration is valid"), ProxySemantic->HasValidOpaqueLabelProxy());
    TestTrue(TEXT("OpaqueProxy is hidden from the main view"), LabelProxyMesh->bVisibleInSceneCaptureOnly != 0);

    AActor* ProxyBackingActor = World->SpawnActor<AActor>();
    ProxyBackingActor->Tags.Add(UE::SensorSimulation::SpecialObjectTests::OpaqueProxyBackingTag);
    UStaticMeshComponent* ProxyBackingMesh = NewObject<UStaticMeshComponent>(ProxyBackingActor, TEXT("OpaqueProxyBackingMesh"));
    ProxyBackingActor->SetRootComponent(ProxyBackingMesh);
    ProxyBackingActor->AddInstanceComponent(ProxyBackingMesh);
    ProxyBackingMesh->SetStaticMesh(Cube);
    // Slightly smaller backing geometry keeps the assertion about policy occlusion independent of raster-edge coverage.
    ProxyBackingMesh->SetWorldScale3D(FVector(0.7));
    ProxyBackingMesh->RegisterComponent();
    ProxyBackingActor->SetActorLocation(FVector(340.0, 0.0, 100.0));
    UE::SensorSimulation::SpecialObjectTests::AddSemanticComponent(
        *ProxyBackingActor, TEXT("OpaqueProxyBackingSemantic"), 51);
    // 运行时构造的材质会异步编译多个顶点工厂排列；必须在复制 PIE World 前等待完成。
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompiling());
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::SpecialObjectTests::FCaptureSpecialObjectsCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(
        UE::SensorSimulation::SpecialObjectTests::FFlushSpecialObjectRenderingCommand());
    return true;
}

#endif
