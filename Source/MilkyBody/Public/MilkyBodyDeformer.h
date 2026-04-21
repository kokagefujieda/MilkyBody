#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "MilkyBodyDeformer.generated.h"

class UMilkyBodyDeformerInstance;

UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="Milky Body Deformer"))
class MILKYBODY_API UMilkyBodyDeformer : public UMeshDeformer
{
	GENERATED_BODY()

public:
	UMilkyBodyDeformer();

	// Vertex range to simulate (bust area)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody")
	int32 VertexIndexStart = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody")
	int32 VertexIndexEnd = 0;

	// PBD parameters
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float Stiffness = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float Damping = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="1", ClampMax="16"))
	int32 SolverIterations = 4;

	// Per-frame blend toward the anim-target pose. 0.0 = pure physics (dent stays
	// forever), 1.0 = snap to anim every frame. Small values (0.02 - 0.1) give a
	// soft, delayed return after a puni hit.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody",
		meta=(ClampMin="0.0", ClampMax="1.0"))
	float AnimBlendPerFrame = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody")
	float GlobalBlendWeight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MilkyBody", meta=(DisplayName="Enable Debug Draw"))
	bool bEnableDebugDraw = false;

	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(
		UMeshComponent* InMeshComponent) override;

	virtual UMeshDeformerInstance* CreateInstance(
		UMeshComponent* InComponent,
		UMeshDeformerInstanceSettings* InSettings) override;
};
