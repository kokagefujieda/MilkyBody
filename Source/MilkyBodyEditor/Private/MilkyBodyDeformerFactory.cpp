#include "MilkyBodyDeformerFactory.h"

#include "AssetTypeCategories.h"
#include "MilkyBodyDeformer.h"

UMilkyBodyDeformerFactory::UMilkyBodyDeformerFactory()
{
	SupportedClass = UMilkyBodyDeformer::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UMilkyBodyDeformerFactory::FactoryCreateNew(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags InFlags,
	UObject* InContext,
	FFeedbackContext* OutWarn)
{
	return NewObject<UMilkyBodyDeformer>(InParent, InClass, InName, InFlags);
}

uint32 UMilkyBodyDeformerFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Animation;
}

bool UMilkyBodyDeformerFactory::ShouldShowInNewMenu() const
{
	return true;
}
