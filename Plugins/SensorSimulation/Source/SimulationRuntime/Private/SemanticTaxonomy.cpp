#include "SemanticTaxonomy.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

bool USemanticTaxonomy::ContainsId(const int32 Id) const
{
    return Classes.ContainsByPredicate([Id](const FSemanticTaxonomyEntry& Entry) { return Entry.Id == Id; });
}

#if WITH_EDITOR
EDataValidationResult USemanticTaxonomy::IsDataValid(FDataValidationContext& Context) const
{
    TSet<int32> Ids;
    TSet<FName> Names;
    for (const FSemanticTaxonomyEntry& Entry : Classes)
    {
        if (Entry.Id < 1 || Entry.Id > 255 || Entry.StableName.IsNone()
            || Ids.Contains(Entry.Id) || Names.Contains(Entry.StableName))
        {
            Context.AddError(FText::FromString(TEXT("Semantic taxonomy requires unique IDs 1..255 and unique non-empty StableName values.")));
            return EDataValidationResult::Invalid;
        }
        Ids.Add(Entry.Id);
        Names.Add(Entry.StableName);
    }
    return EDataValidationResult::Valid;
}
#endif
