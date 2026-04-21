#include "MilkyBodyDeformer.h"
#include "MilkyBodyDeformerInstance.h"

UMilkyBodyDeformer::UMilkyBodyDeformer()
{
	VertexIndexStart = 0;
	VertexIndexEnd = 0;
	Stiffness = 0.8f;
	Damping = 0.1f;
	SolverIterations = 4;
	AnimBlendPerFrame = 0.05f;
	GlobalBlendWeight = 1.0f;
}

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
