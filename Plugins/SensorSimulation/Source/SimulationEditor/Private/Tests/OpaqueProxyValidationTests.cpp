#include "SemanticObjectComponent.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Tests/AutomationEditorCommon.h"

namespace UE::SensorSimulation::OpaqueProxyValidationTests
{
/** 创建只用于验证材质分类的瞬态半透明材质，不依赖内容目录资产。 */
UMaterial* CreateTranslucentMaterial()
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(), TEXT("OpaqueProxyValidationTranslucent"), RF_Transient);
    Material->BlendMode = BLEND_Translucent;
    Material->PostEditChange();
    return Material;
}

/** 向 Actor 添加已注册的 Cube 图元，并按需设置半透明材质。 */
UStaticMeshComponent* AddCube(
    AActor& Actor,
    const FName Name,
    UMaterialInterface* Material = nullptr)
{
    UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(&Actor, Name);
    if (!Actor.GetRootComponent())
    {
        Actor.SetRootComponent(Mesh);
    }
    else
    {
        Mesh->SetupAttachment(Actor.GetRootComponent());
    }
    Actor.AddInstanceComponent(Mesh);
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    if (Material)
    {
        Mesh->SetMaterial(0, Material);
    }
    Mesh->RegisterComponent();
    return Mesh;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOpaqueProxyDataValidationTest,
    "SensorSimulation.Rendering.OpaqueProxy.DataValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOpaqueProxyDataValidationTest::RunTest(const FString& Parameters)
{
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("OpaqueProxy validation World is created"), World))
    {
        return false;
    }

    UMaterial* TranslucentMaterial =
        UE::SensorSimulation::OpaqueProxyValidationTests::CreateTranslucentMaterial();
    AActor* Actor = World->SpawnActor<AActor>();
    UStaticMeshComponent* Source =
        UE::SensorSimulation::OpaqueProxyValidationTests::AddCube(
            *Actor, TEXT("TranslucentSource"), TranslucentMaterial);
    UStaticMeshComponent* Proxy =
        UE::SensorSimulation::OpaqueProxyValidationTests::AddCube(
            *Actor, TEXT("OpaqueLabelProxy"));

    USemanticObjectComponent* Semantic =
        NewObject<USemanticObjectComponent>(Actor, TEXT("OpaqueProxySemantic"));
    Semantic->SemanticId = 1;
    Semantic->TranslucentLabelPolicy = ETranslucentLabelPolicy::OpaqueProxy;
    Semantic->OpaqueLabelProxy = Proxy;
    Semantic->OpaqueProxyBoundsTolerance = 0.2f;
    Actor->AddInstanceComponent(Semantic);
    Semantic->RegisterComponent();

    FDataValidationContext ValidContext;
    TestEqual(TEXT("A same-Actor opaque and aligned proxy is valid"),
        Semantic->IsDataValid(ValidContext), EDataValidationResult::Valid);
    TestEqual(TEXT("A valid proxy has no validation errors"), ValidContext.GetNumErrors(), 0u);
    TestEqual(TEXT("An aligned proxy has no validation warnings"), ValidContext.GetNumWarnings(), 0u);

    Semantic->OpaqueLabelProxy = nullptr;
    FDataValidationContext MissingContext;
    TestEqual(TEXT("OpaqueProxy without a proxy is invalid"),
        Semantic->IsDataValid(MissingContext), EDataValidationResult::Invalid);
    TestTrue(TEXT("Missing proxy reports an error"), MissingContext.GetNumErrors() > 0);

    AActor* ForeignActor = World->SpawnActor<AActor>();
    UStaticMeshComponent* ForeignProxy =
        UE::SensorSimulation::OpaqueProxyValidationTests::AddCube(
            *ForeignActor, TEXT("ForeignOpaqueProxy"));
    Semantic->OpaqueLabelProxy = ForeignProxy;
    FDataValidationContext ForeignContext;
    TestEqual(TEXT("A proxy on another Actor is invalid"),
        Semantic->IsDataValid(ForeignContext), EDataValidationResult::Invalid);
    TestTrue(TEXT("Foreign proxy reports an error"), ForeignContext.GetNumErrors() > 0);

    Semantic->OpaqueLabelProxy = Proxy;
    Proxy->SetMaterial(0, TranslucentMaterial);
    FDataValidationContext MaterialContext;
    TestEqual(TEXT("A translucent label proxy is invalid"),
        Semantic->IsDataValid(MaterialContext), EDataValidationResult::Invalid);
    TestTrue(TEXT("Translucent proxy reports an error"), MaterialContext.GetNumErrors() > 0);
    Proxy->SetMaterial(0, nullptr);

    Proxy->SetRelativeLocation(FVector(500.0, 0.0, 0.0));
    Proxy->UpdateBounds();
    FDataValidationContext BoundsContext;
    TestEqual(TEXT("Approximate proxy bounds remain usable"),
        Semantic->IsDataValid(BoundsContext), EDataValidationResult::Valid);
    TestTrue(TEXT("A strongly misaligned proxy reports a warning"), BoundsContext.GetNumWarnings() > 0);
    Proxy->SetRelativeLocation(FVector::ZeroVector);
    Proxy->UpdateBounds();

    Proxy->UnregisterComponent();
    FDataValidationContext RegistrationContext;
    TestEqual(TEXT("An unregistered proxy is reported without invalidating the asset"),
        Semantic->IsDataValid(RegistrationContext), EDataValidationResult::Valid);
    TestTrue(TEXT("Unregistered proxy reports a warning"), RegistrationContext.GetNumWarnings() > 0);
    Proxy->RegisterComponent();

    // 保持 Source 被引用到测试末尾，防止编译器或 GC 假定测试不关心透明源。
    TestNotNull(TEXT("The translucent source remains available"), Source);
    return true;
}

#endif
