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

	// Kneading: simulates a hand grabbing and kneading the body. Spawns up to
	// 3 animated puni dents wandering inside ScatterRadius around WorldLocation.
	// Mutually exclusive with the single hold push.
	void BeginKneading(
		const FVector& WorldLocation,
		const FVector& WorldImpulse,
		float Radius,
		int32 NumPoints,
		float ScatterRadius,
		float MotionSpeed,
		float RandomSeed);

	// Stop kneading; fades out alongside the push amount spring.
	void EndKneading();

	// Momi (揉み): Circular kneading motion that simulates hands grabbing
	// and massaging in a circular orbit. Each point orbits around the grab
	// center with slight speed variation and radius wobble for realism.
	// Mutually exclusive with hold push and linear kneading.
	void BeginMomi(
		const FVector& WorldLocation,
		const FVector& WorldImpulse,
		float Radius,
		int32 NumPoints,
		float OrbitRadius,
		float OrbitSpeed,
		float SpeedVariation,
		float DepthVariation,
		float RandomSeed);

	// Stop momi; fades out alongside the push amount spring.
	void EndMomi();

	// External multi-hold: caller supplies N world-space sphere centers + radii
	// each frame (e.g. fingertip bones snapped to the body surface). Each slot
	// gets its own per-slot impulse vector (typically inward along the local
	// surface normal). Mutually exclusive with single hold / kneading / momi.
	// Locations.Num() must equal Radii.Num() and Impulses.Num(); excess beyond
	// MaxPuniSlots is clamped.
	void BeginExternalMultiHold(
		const TArray<FVector>& WorldLocations,
		const TArray<float>& Radii,
		const TArray<FVector>& WorldImpulses);

	// Stop external multi-hold; fades out alongside the push amount spring.
	void EndExternalMultiHold();

private:
	TObjectPtr<UMilkyBodyDeformer> DeformerAsset;
	TWeakObjectPtr<UMeshComponent> MeshComponent;

	// --- Puni push state ---
	// Anchor: dent center (and impulse) captured the frame the hold began (or
	// on a one-shot impulse). Stays fixed for the lifetime of a hold so the
	// dent doesn't teleport when the cursor moves.
	// Effective dent center = AnchorLocalPos + CurrentLocalOffset, where
	// CurrentLocalOffset smooths toward TargetLocalOffset using
	// UMilkyBodyDeformer::HoldFollowSmoothing and is clamped to a fraction of
	// PushRadius (HoldMaxFollowOffsetRatio).
	FVector3f AnchorLocalPos = FVector3f::ZeroVector;
	FVector3f AnchorLocalImpulse = FVector3f::ZeroVector;
	FVector3f CurrentLocalOffset = FVector3f::ZeroVector;
	FVector3f TargetLocalOffset = FVector3f::ZeroVector;
	float PushRadius = 0.0f;
	float CurrentPushAmount = 0.0f;
	float PushVelocity = 0.0f;
	float TargetPushAmount = 0.0f;
	bool bPushHeld = false;

	// --- Kneading state ---
	// When active, slots 0..NumKneadingPoints-1 are driven procedurally each
	// frame in EnqueueWork (sin/cos wandering on the impulse-tangent plane),
	// replacing the single hold dispatch.
	bool bKneading = false;
	int32 NumKneadingPoints = 0;
	float KneadingScatterRadius = 0.0f;
	float KneadingMotionSpeed = 0.0f;
	float KneadingTime = 0.0f;
	float KneadingSeed = 0.0f;
	FVector3f KneadCenterLocal = FVector3f::ZeroVector;
	FVector3f KneadImpulseLocal = FVector3f::ZeroVector;
	FVector3f KneadTangentU = FVector3f(1, 0, 0);  // basis vectors perpendicular to KneadImpulseLocal,
	FVector3f KneadTangentV = FVector3f(0, 1, 0);  // used to scatter fingers across the touched surface
	float KneadEachRadius = 0.0f;

	// --- Momi (揉み) state ---
	// Circular orbit kneading. Each point traces an elliptical orbit around
	// the grab center on the tangent plane, with per-point speed jitter and
	// depth pulsing for a realistic hand-massage feel.
	bool bMomi = false;
	int32 NumMomiPoints = 0;
	float MomiOrbitRadius = 0.0f;
	float MomiOrbitSpeed = 0.0f;
	float MomiSpeedVariation = 0.0f;   // 0..1 fraction of speed randomness
	float MomiDepthVariation = 0.0f;   // 0..1 fraction of impulse depth pulsing
	float MomiTime = 0.0f;
	float MomiSeed = 0.0f;
	FVector3f MomiCenterLocal = FVector3f::ZeroVector;
	FVector3f MomiImpulseLocal = FVector3f::ZeroVector;
	FVector3f MomiTangentU = FVector3f(1, 0, 0);
	FVector3f MomiTangentV = FVector3f(0, 1, 0);
	float MomiEachRadius = 0.0f;

	// --- External multi-hold state (per-frame supplied positions, e.g. fingertips) ---
	bool bExternalMultiHold = false;
	TArray<FVector3f> ExternalLocalPositions;
	TArray<FVector3f> ExternalLocalImpulses;
	TArray<float> ExternalRadii;
};
