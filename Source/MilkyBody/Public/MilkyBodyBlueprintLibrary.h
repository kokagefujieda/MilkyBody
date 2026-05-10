#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MilkyBodyBlueprintLibrary.generated.h"

class USkinnedMeshComponent;
class APlayerController;

UCLASS()
class MILKYBODY_API UMilkyBodyBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ── Mesh resolution helper ────────────────────────────────────────────────
	// Walks the actor's hierarchy (parent → self → child actors recursively) and
	// returns the first SkinnedMeshComponent it finds. When bRequireMilkyBody is
	// true, only meshes that already have a UMilkyBodyDeformerInstance attached
	// are returned. Use this from Blueprint to obtain the MeshComp argument for
	// the direct BeginMomi / SetHoldPuni / BeginKneading APIs without worrying
	// about whether the SkinnedMesh sits on a child actor.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="MilkyBody")
	static USkinnedMeshComponent* FindMilkyBodyMeshOnActor(AActor* Actor, bool bRequireMilkyBody = true);

	// ── Direct API ────────────────────────────────────────────────────────────

	// Trigger a "puni" dent on a SkeletalMesh that has a MilkyBodyDeformer assigned.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius,DurationSeconds"))
	static void ApplyPuni(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f);

	// Hold-style dent via direct world-space parameters. Call each frame while held.
	// Returns the name of the bone on MeshComp closest to WorldLocation.
	// If no MilkyBodyDeformer is attached, no dent is applied but the bone is still returned.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius"))
	static FName SetHoldPuni(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 15.0f);

	// Release a held puni dent.
	UFUNCTION(BlueprintCallable, Category="MilkyBody")
	static void ReleaseHoldPuni(
		USkinnedMeshComponent* MeshComp,
		float FadeOutSeconds = 0.3f);

	// ── World-space line trace ─────────────────────────────────────────────────
	// These functions cast a ray from Start to End in world space,
	// find the first SkinnedMeshComponent along that ray, and apply a puni dent.
	// Debug draw (bEnableDebug=true): green/red ray, yellow sphere at hit, cyan
	// arrow for impact normal — visible for ~2 s in the viewport.
	//
	// Detection priority:
	//   1. Channel trace (fast, requires Physics Asset on the mesh)
	//   2. LineTraceComponent per component (uses Physics Asset bodies if present)
	//   3. AABB intersection fallback (no Physics Asset required)

	// One-shot puni via world-space line trace. Returns true when a dent fired.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(WorldContext="WorldContextObject",
		      AdvancedDisplay="Radius,DurationSeconds,TraceChannel,bEnableDebug"))
	static bool LineTracePuni(
		UObject* WorldContextObject,
		FVector Start,
		FVector End,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// Hold-style puni via world-space line trace. Call each frame while the input
	// is held. Returns the hit component (pass to ReleaseHoldPuni on release).
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(WorldContext="WorldContextObject",
		      AdvancedDisplay="Radius,TraceChannel,bEnableDebug"))
	static USkinnedMeshComponent* LineTraceHoldPuni(
		UObject* WorldContextObject,
		FVector Start,
		FVector End,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// ── Cursor-based convenience wrappers ─────────────────────────────────────

	// One-shot puni under the mouse cursor.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="Radius,DurationSeconds,TraceChannel,bEnableDebug"))
	static bool ApplyPuniUnderCursor(
		APlayerController* PlayerController,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// Hold-style puni under the cursor. Call each frame while held.
	// Returns the name of the bone on the hit mesh closest to the impact point
	// (NAME_None on miss). The hit component is written to OutMeshComp even when
	// no MilkyBodyDeformer is attached. In that case no dent is applied.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="Radius,TraceChannel,bEnableDebug"))
	static FName SetHoldPuniUnderCursor(
		APlayerController* PlayerController,
		USkinnedMeshComponent*& OutMeshComp,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// ── Polygon-precise (skinned mesh) line trace ─────────────────────────────
	// These functions trace against the actual skinned vertex triangles of any
	// SkinnedMeshComponent in the world (Möller–Trumbore on CPU-skinned positions),
	// NOT the Physics Asset bodies. Useful when a Physics Asset is missing or
	// when you need true mesh-surface precision.
	//
	// On hit, a debug ball (yellow sphere) is drawn at the impact point when
	// bEnableDebug is true. Cost scales with triangle count, so prefer for
	// click-rate calls rather than per-frame.

	// One-shot puni via line trace against actual skinned mesh polygons.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(WorldContext="WorldContextObject",
		      AdvancedDisplay="Radius,DurationSeconds,bEnableDebug"))
	static bool LineTracePuniPolygon(
		UObject* WorldContextObject,
		FVector Start,
		FVector End,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f,
		bool bEnableDebug = false);

	// One-shot puni traced from the player's camera center forward, hitting
	// the actual skinned mesh polygons. A ball appears at the impact point
	// when bEnableDebug is true.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="TraceDistance,Radius,DurationSeconds,bEnableDebug"))
	static bool ApplyPuniFromCamera(
		APlayerController* PlayerController,
		float TraceDistance = 100000.0f,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f,
		bool bEnableDebug = false);

	// ── Kneading (multi-finger animated puni) ────────────────────────────────
	// Simulates a hand grabbing and kneading the body: NumPoints (≤4) animated
	// puni dents wander across the surface within ScatterRadius around the
	// grab point. Continues until EndKneading is called.

	// Begin kneading at WorldLocation. Cancels any single hold puni in flight.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="NumPoints,ScatterRadius,MotionSpeed,RandomSeed"))
	static void BeginKneading(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 6.0f,
		int32 NumPoints = 3,
		float ScatterRadius = 8.0f,
		float MotionSpeed = 4.0f,
		float RandomSeed = 0.0f);

	UFUNCTION(BlueprintCallable, Category="MilkyBody")
	static void EndKneading(USkinnedMeshComponent* MeshComp);

	// Begin kneading on whatever skinned mesh is under the cursor. Returns
	// the hit MeshComp via OutMeshComp (pass to EndKneading on release).
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="NumPoints,ScatterRadius,MotionSpeed,RandomSeed,TraceChannel,bEnableDebug"))
	static bool BeginKneadingUnderCursor(
		APlayerController* PlayerController,
		USkinnedMeshComponent*& OutMeshComp,
		float PushDepth = 5.0f,
		float Radius = 6.0f,
		int32 NumPoints = 3,
		float ScatterRadius = 8.0f,
		float MotionSpeed = 4.0f,
		float RandomSeed = 0.0f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// ── Momi (揉み — circular kneading) ──────────────────────────────────────
	// Simulates a hand grabbing and massaging the body in a circular orbit.
	// Each point traces an elliptical path around the grab center on the
	// surface tangent plane with per-point speed jitter and depth pulsing
	// for a realistic hand-massage feel. Continues until EndMomi is called.

	// Begin momi at WorldLocation. Cancels any hold puni or kneading in flight.
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="NumPoints,OrbitRadius,OrbitSpeed,SpeedVariation,DepthVariation,RandomSeed"))
	static void BeginMomi(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 6.0f,
		int32 NumPoints = 3,
		float OrbitRadius = 8.0f,
		float OrbitSpeed = 3.0f,
		float SpeedVariation = 0.3f,
		float DepthVariation = 0.2f,
		float RandomSeed = 0.0f);

	UFUNCTION(BlueprintCallable, Category="MilkyBody")
	static void EndMomi(USkinnedMeshComponent* MeshComp);

	// Begin momi on whatever skinned mesh is under the cursor. Returns
	// the hit MeshComp via OutMeshComp (pass to EndMomi on release).
	UFUNCTION(BlueprintCallable, Category="MilkyBody",
		meta=(AdvancedDisplay="NumPoints,OrbitRadius,OrbitSpeed,SpeedVariation,DepthVariation,RandomSeed,TraceChannel,bEnableDebug"))
	static bool BeginMomiUnderCursor(
		APlayerController* PlayerController,
		USkinnedMeshComponent*& OutMeshComp,
		float PushDepth = 5.0f,
		float Radius = 6.0f,
		int32 NumPoints = 3,
		float OrbitRadius = 8.0f,
		float OrbitSpeed = 3.0f,
		float SpeedVariation = 0.3f,
		float DepthVariation = 0.2f,
		float RandomSeed = 0.0f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);
};
