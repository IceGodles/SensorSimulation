#pragma once
#include "Engine/DataAsset.h"
#include "SemanticTaxonomy.generated.h"

USTRUCT(BlueprintType)
struct SIMULATIONRUNTIME_API FSemanticTaxonomyEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="1", ClampMax="255")) int32 Id = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FName StableName = NAME_None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor DebugColor = FLinearColor::White;
};

UCLASS(BlueprintType)
class SIMULATIONRUNTIME_API USemanticTaxonomy : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Semantic") FString Version = TEXT("1.0");
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Semantic") TArray<FSemanticTaxonomyEntry> Classes;
    bool ContainsId(int32 Id) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
