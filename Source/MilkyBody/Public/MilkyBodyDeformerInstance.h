#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformerInstance.h"
#include "RenderResource.h"
#include "MilkyBodyDeformerInstance.generated.h"

class UMilkyBodyDeformer;
class UMeshComponent;
class FSkeletalMeshObject;
class FSkeletalMeshLODRenderData;
class FRHICommandListBase;

UCLASS()
class MILKYBODY_API UMilkyBodyDeformerInstance : public UMeshDeformerInstance
{
	GENERATED_BODY()

public:
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(const FEnqueueWorkDesc& InDesc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;
	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override;

#if WITH_EDITORONLY_DATA
	virtual bool RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest) override;
#endif

public:
	// Set the deformer asset when instance is created
	void SetDeformerAsset(UMilkyBodyDeformer* InDeformer) { DeformerAsset = InDeformer; }
	void SetMeshComponent(UMeshComponent* InComponent) { MeshComponent = InComponent; }

	// Apply a transient "puni" dent around WorldLocation, pushing particles along WorldImpulse
	// (direction * distance in cm). Lasts for DurationSeconds, linearly fading out.
	void ApplyPushImpulse(const FVector& WorldLocation, const FVector& WorldImpulse, float Radius, float DurationSeconds);

	// Hold-style dent: stays at full strength until ReleaseHold is called. Call each frame
	// to keep the dent centered on a moving location. Cancels any in-flight one-shot fade.
	void BeginHoldPush(const FVector& WorldLocation, const FVector& WorldImpulse, float Radius);

	// Release a held push and start a fade-out over FadeOutSeconds (0 = instant).
	void ReleaseHoldPush(float FadeOutSeconds);

private:
	TObjectPtr<UMilkyBodyDeformer> DeformerAsset;
	TWeakObjectPtr<UMeshComponent> MeshComponent;

	// --- Puni push state ---
	FVector3f PushLocalPos = FVector3f::ZeroVector;
	FVector3f PushLocalImpulse = FVector3f::ZeroVector;
	float PushRadius = 0.0f;
	float CurrentPushAmount = 0.0f;
	float PushVelocity = 0.0f;
	float TargetPushAmount = 0.0f;
	bool bPushHeld = false;
};
