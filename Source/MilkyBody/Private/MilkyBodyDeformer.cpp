#include "MilkyBodyDeformer.h"
#include "MilkyBodyDeformerInstance.h"

UMilkyBodyDeformer::UMilkyBodyDeformer() = default;

UMeshDeformerInstanceSettings* UMilkyBodyDeformer::CreateSettingsInstance(
	UMeshComponent* InMeshComponent)
{
	return nullptr;
}

UMeshDeformerInstance* UMilkyBodyDeformer::CreateInstance(
	UMeshComponent* InComponent,
	UMeshDeformerInstanceSettings* InSettings)
{
	UMilkyBodyDeformerInstance* Instance = NewObject<UMilkyBodyDeformerInstance>(InComponent);
	Instance->SetDeformerAsset(this);
	Instance->SetMeshComponent(InComponent);
	return Instance;
}
