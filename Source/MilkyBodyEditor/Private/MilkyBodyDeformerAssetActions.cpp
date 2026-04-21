#include "MilkyBodyDeformerAssetActions.h"

#include "AssetTypeCategories.h"
#include "MilkyBodyDeformer.h"

#define LOCTEXT_NAMESPACE "MilkyBodyDeformerAssetActions"

FText FMilkyBodyDeformerAssetActions::GetName() const
{
	return LOCTEXT("MilkyBodyDeformerAssetName", "Milky Body Deformer");
}

FColor FMilkyBodyDeformerAssetActions::GetTypeColor() const
{
	return FColor(255, 180, 200);
}

UClass* FMilkyBodyDeformerAssetActions::GetSupportedClass() const
{
	return UMilkyBodyDeformer::StaticClass();
}

uint32 FMilkyBodyDeformerAssetActions::GetCategories()
{
	return EAssetTypeCategories::Animation;
}

const TArray<FText>& FMilkyBodyDeformerAssetActions::GetSubMenus() const
{
	static const TArray<FText> SubMenus
	{
		LOCTEXT("MilkyBodyDeformerSubMenu", "Deformers"),
	};
	return SubMenus;
}

#undef LOCTEXT_NAMESPACE
