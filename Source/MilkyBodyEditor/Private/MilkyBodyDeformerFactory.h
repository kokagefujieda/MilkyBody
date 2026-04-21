#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MilkyBodyDeformerFactory.generated.h"

UCLASS(hidecategories=Object)
class UMilkyBodyDeformerFactory : public UFactory
{
	GENERATED_BODY()

public:
	UMilkyBodyDeformerFactory();

	virtual FString GetDefaultNewAssetName() const override
	{
		return TEXT("MilkyBodyDeformer");
	}

	virtual UObject* FactoryCreateNew(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags InFlags,
		UObject* InContext,
		FFeedbackContext* OutWarn) override;

	virtual uint32 GetMenuCategories() const override;
	virtual bool ShouldShowInNewMenu() const override;
};
