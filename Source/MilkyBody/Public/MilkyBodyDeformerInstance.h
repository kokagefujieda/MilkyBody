#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformerInstance.h"
#include "HAL/CriticalSection.h"
#include "Tickable.h"
#include "MilkyBodyDeformerInstance.generated.h"

class UMilkyBodyDeformer;
class UMeshComponent;
class USkinnedMeshComponent;

namespace MilkyBody
{
	// Damped spring driving a contact's push amount (0 = released, 1 = full depth).
	// Integrated with fixed sub-steps so it stays stable for every Stiffness /
	// Damping combination the asset allows, independent of frame rate.
	struct FPuniSpring
	{
		float Amount = 0.0f;
		float Velocity = 0.0f;
		float Target = 0.0f;

		void Step(float DeltaSeconds, float SpringK, float SpringC);
		void SnapToZero() { Amount = 0.0f; Velocity = 0.0f; Target = 0.0f; }
		bool IsSettled() const;
	};

	// One-shot dent from ApplyPuni: presses for DurationSeconds, then springs back.
	struct FImpulseContact
	{
		FVector3f LocalPos = FVector3f::ZeroVector;
		FVector3f LocalImpulse = FVector3f::ZeroVector;
		float LocalRadius = 0.0f;
		double ReleaseTime = 0.0;
		FPuniSpring Spring;
	};

	// Caller-driven contact (e.g. a fingertip). Refreshed every frame by its
	// source; fades out on its own spring once the source stops updating it.
	struct FExternalContact
	{
		FVector3f LocalPos = FVector3f::ZeroVector;
		FVector3f LocalImpulse = FVector3f::ZeroVector;
		float LocalRadius = 0.0f;
		uint64 LastUpdateFrame = 0;
		FPuniSpring Spring;
	};

	struct FHoldState
	{
		bool bHeld = false;
		FVector3f AnchorLocalPos = FVector3f::ZeroVector;
		FVector3f AnchorLocalImpulse = FVector3f::ZeroVector;
		FVector3f CurrentLocalOffset = FVector3f::ZeroVector;
		FVector3f TargetLocalOffset = FVector3f::ZeroVector;
		float LocalRadius = 0.0f;
		double LastRefreshTime = 0.0;
		FPuniSpring Spring;
	};

	struct FKneadingState
	{
		bool bActive = false; // stays true while fading so the motion continues
		int32 NumPoints = 0;
		float LocalScatterRadius = 0.0f;
		float MotionSpeed = 0.0f;
		float Time = 0.0f;
		float Seed = 0.0f;
		float LocalEachRadius = 0.0f;
		FVector3f CenterLocal = FVector3f::ZeroVector;
		FVector3f ImpulseLocal = FVector3f::ZeroVector;
		FVector3f TangentU = FVector3f(1, 0, 0); // basis perpendicular to the impulse, used to
		FVector3f TangentV = FVector3f(0, 1, 0); // scatter fingers across the touched surface
		FPuniSpring Spring;
	};

	struct FMomiState
	{
		bool bActive = false;
		int32 NumPoints = 0;
		float LocalOrbitRadius = 0.0f;
		float OrbitSpeed = 0.0f;
		float SpeedVariation = 0.0f; // 0..1 fraction of speed randomness
		float DepthVariation = 0.0f; // 0..1 fraction of impulse depth pulsing
		float Time = 0.0f;
		float Seed = 0.0f;
		float LocalEachRadius = 0.0f;
		FVector3f CenterLocal = FVector3f::ZeroVector;
		FVector3f ImpulseLocal = FVector3f::ZeroVector;
		FVector3f TangentU = FVector3f(1, 0, 0);
		FVector3f TangentV = FVector3f(0, 1, 0);
		FPuniSpring Spring;
	};

	struct FSoftnessCache
	{
		const void* SkinnedAsset = nullptr;
		int32 LODIndex = INDEX_NONE;
		uint32 SettingsHash = 0;
		bool bUseBoneSoftness = false;
		// Shared with in-flight render commands; replaced (never mutated) on change.
		TSharedPtr<const TArray<TArray<FVector4f>>, ESPMode::ThreadSafe> PerSection;
	};
}

UCLASS()
class MILKYBODY_API UMilkyBodyDeformerInstance : public UMeshDeformerInstance, public FTickableGameObject
{
	GENERATED_BODY()

public:
	// --- UMeshDeformerInstance ---
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(const FEnqueueWorkDesc& InDesc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;
	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override;

#if WITH_EDITORONLY_DATA
	virtual bool RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest) override;
#endif

	// --- FTickableGameObject ---
	// The spring simulation runs here on the game thread (after all actor
	// ticks, before end-of-frame render updates) so it is frame-rate
	// independent and can safely draw debug shapes and mark the component's
	// render data dirty while a dent is moving.
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual UWorld* GetTickableGameObjectWorld() const override;
	virtual bool IsTickableInEditor() const override { return false; }
	virtual bool IsTickableWhenPaused() const override { return false; }
	virtual TStatId GetStatId() const override;

public:
	// Set the deformer asset when instance is created
	void SetDeformerAsset(UMilkyBodyDeformer* InDeformer) { DeformerAsset = InDeformer; }
	void SetMeshComponent(UMeshComponent* InComponent) { MeshComponent = InComponent; }

	// All interaction channels below are independent and layer additively, so
	// e.g. a cursor hold, a one-shot tap and fingertip contacts can deform the
	// same mesh at the same time. Radii and impulses are world-space (cm).

	// One-shot dent: presses toward WorldImpulse (direction × depth) for
	// DurationSeconds, then springs back. DurationSeconds <= 0 gives a short
	// velocity "kick" instead. Up to 4 one-shots run at once (oldest dropped).
	void ApplyPushImpulse(const FVector& WorldLocation, const FVector& WorldImpulse, float Radius, float DurationSeconds);

	// Hold-style dent: call every frame while held. The first call anchors the
	// dent; later calls drag the skin toward the new location. Auto-releases
	// after UMilkyBodyDeformer::HoldAutoReleaseSeconds without a refresh.
	void BeginHoldPush(const FVector& WorldLocation, const FVector& WorldImpulse, float Radius);

	// Release the hold (FadeOutSeconds <= 0 = instant, otherwise spring back).
	void ReleaseHoldPush(float FadeOutSeconds);

	// Kneading: up to 4 animated dents wandering inside ScatterRadius around
	// WorldLocation until EndKneading is called.
	void BeginKneading(
		const FVector& WorldLocation,
		const FVector& WorldImpulse,
		float Radius,
		int32 NumPoints,
		float ScatterRadius,
		float MotionSpeed,
		float RandomSeed);

	// Stop kneading; the dents keep moving while they fade out.
	void EndKneading();

	// Momi (揉み): up to 4 dents orbiting the grab center with per-point speed
	// jitter and depth pulsing until EndMomi is called.
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

	// Stop momi; the dents keep orbiting while they fade out.
	void EndMomi();

	// External contacts (e.g. fingertip spheres). Call every frame with the
	// contacts currently touching. Source identifies the caller so several
	// drivers (two hands) can touch the same mesh; ContactIds must be stable per
	// finger across frames. Contacts of this Source missing from the call (or
	// not refreshed this frame) fade out individually on their own spring.
	// All arrays must have the same length.
	void UpdateExternalContacts(
		const UObject* Source,
		const TArray<int32>& ContactIds,
		const TArray<FVector>& WorldLocations,
		const TArray<float>& Radii,
		const TArray<FVector>& WorldImpulses);

	// Fade out every external contact supplied by Source.
	void ReleaseExternalContacts(const UObject* Source);

	// Legacy single-source wrappers around UpdateExternalContacts / ReleaseExternalContacts.
	void BeginExternalMultiHold(
		const TArray<FVector>& WorldLocations,
		const TArray<float>& Radii,
		const TArray<FVector>& WorldImpulses);
	void EndExternalMultiHold();

	// Release every channel (hold, one-shots, kneading, momi, external contacts).
	void ReleaseAll(float FadeOutSeconds);

	// Largest current push amount across all contacts (0 = idle, ~1 = full depth;
	// may briefly exceed 1 or go negative while the spring overshoots).
	float GetPuniAmount() const;

	// True while any contact is being pressed (not merely fading out).
	bool IsPuniActive() const;

private:
	UPROPERTY()
	TObjectPtr<UMilkyBodyDeformer> DeformerAsset;

	TWeakObjectPtr<UMeshComponent> MeshComponent;

	USkinnedMeshComponent* GetSkinnedComponent() const;
	bool HasAnyContact() const;
	void BuildSlots(const FTransform& ComponentToWorld, bool bDebugDraw, UWorld* DebugWorld);

	// Game-time clock advanced in Tick. Used for one-shot durations and hold
	// auto-release so both respect pause and time dilation.
	double SimTime = 0.0;

	// --- Single hold ---
	// The anchor stays fixed for the lifetime of a hold so the dent doesn't
	// teleport when the cursor moves; CurrentLocalOffset (smoothed toward
	// TargetLocalOffset, clamped to a fraction of the radius) is folded into
	// the impulse as a tangential pull.
	MilkyBody::FHoldState Hold;

	// --- One-shot impulses ---
	TArray<MilkyBody::FImpulseContact> Impulses;

	// --- Kneading ---
	MilkyBody::FKneadingState Kneading;

	// --- Momi (揉み) ---
	MilkyBody::FMomiState Momi;

	// --- External contacts, keyed by (source unique id << 32 | contact id) ---
	TMap<uint64, MilkyBody::FExternalContact> ExternalContacts;

	// --- Slots handed to the GPU (component space, push amount applied) ---
	// Written in Tick, read in EnqueueWork (which may run on a worker thread
	// during end-of-frame updates), hence the lock.
	FCriticalSection PendingSlotsLock;
	TArray<FVector4f> PendingPosAndRadius;
	TArray<FVector4f> PendingImpulse;
	// One extra dispatch after everything settles so the mesh returns cleanly.
	bool bSlotsActiveLastTick = false;

	// --- Bone softness cache (per section fake bone matrices) ---
	MilkyBody::FSoftnessCache SoftnessCache;

	void UpdateSoftnessCache(USkinnedMeshComponent* SkinnedComp, int32 LODIndex);

	bool bWarnedNoVertexColors = false;
};
