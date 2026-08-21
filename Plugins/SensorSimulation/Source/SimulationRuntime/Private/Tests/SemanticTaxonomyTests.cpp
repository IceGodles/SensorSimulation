#include "SemanticTaxonomy.h"
#include "SemanticRegistry.h"
#include "SemanticObjectComponent.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSemanticTaxonomyRegistryTest,
    "SensorSimulation.Runtime.Semantic.TaxonomyEnforcement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSemanticTaxonomyRegistryTest::RunTest(const FString& Parameters)
{
    USemanticTaxonomy* Taxonomy = NewObject<USemanticTaxonomy>();
    FSemanticTaxonomyEntry& Entry = Taxonomy->Classes.AddDefaulted_GetRef();
    Entry.Id = 7;
    Entry.StableName = TEXT("vehicle");
    TestTrue(TEXT("Declared semantic class is discoverable"), Taxonomy->ContainsId(7));

    AActor* Actor = NewObject<AActor>();
    USemanticObjectComponent* Component = NewObject<USemanticObjectComponent>(Actor);
    FSemanticRegistry Registry;
    Registry.ConfigureTaxonomy(Taxonomy);
    Component->SemanticId = 7;
    TestTrue(TEXT("Declared SemanticId receives an InstanceId"), Registry.Register(*Component) > 0);

    AActor* InvalidActor = NewObject<AActor>();
    USemanticObjectComponent* Invalid = NewObject<USemanticObjectComponent>(InvalidActor);
    Invalid->SemanticId = 99;
    AddExpectedError(TEXT("is not declared by the configured taxonomy"),
        EAutomationExpectedErrorFlags::Contains, 1);
    TestEqual(TEXT("Undeclared SemanticId is rejected"), Registry.Register(*Invalid), uint32{0});
    return true;
}
#endif
