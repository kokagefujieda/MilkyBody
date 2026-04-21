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
	// Trigger a "puni" dent on a SkeletalMesh that has a MilkyBodyDeformer assigned.
	// WorldLocation: impact point. WorldImpulse: push direction * displacement (cm, local to the world).
	// Typical use from an Enhanced Input On Triggered: line-trace under cursor, take HitResult.Location
	// and ImpactNormal * -Strength as WorldImpulse.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius,DurationSeconds"))
	static void ApplyPuni(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f);

	// One-shot convenience: does a line-trace under the mouse cursor from the given
	// PlayerController, finds the SkinnedMeshComponent that was hit, and applies a puni dent
	// along -ImpactNormal * PushDepth. Returns true if a dent was actually triggered.
	// Wire this straight into an Enhanced Input "Triggered" event and you'll see the dent.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius,DurationSeconds,TraceChannel,bEnableDebug"))
	static bool ApplyPuniUnderCursor(
		APlayerController* PlayerController,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		float DurationSeconds = 0.3f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// Hold-style dent via direct world-space parameters. Call repeatedly (e.g., every tick)
	// while the input is held to keep the dent centered. Call ReleaseHoldPuni on mouse-up.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius"))
	static void SetHoldPuni(
		USkinnedMeshComponent* MeshComp,
		FVector WorldLocation,
		FVector WorldImpulse,
		float Radius = 15.0f);

	// Hold-style dent under the mouse cursor. Call each frame while the mouse button is held
	// down (re-traces every call so the dent follows a moving cursor). Returns the skinned
	// mesh component you should pass to ReleaseHoldPuni on release, or null if no mesh was hit.
	UFUNCTION(BlueprintCallable, Category="MilkyBody", meta=(AdvancedDisplay="Radius,TraceChannel,bEnableDebug"))
	static USkinnedMeshComponent* SetHoldPuniUnderCursor(
		APlayerController* PlayerController,
		float PushDepth = 5.0f,
		float Radius = 15.0f,
		ECollisionChannel TraceChannel = ECC_Visibility,
		bool bEnableDebug = false);

	// Release a held puni dent and let it fade back to the anim pose over FadeOutSeconds.
	UFUNCTION(BlueprintCallable, Category="MilkyBody")
	static void ReleaseHoldPuni(
		USkinnedMeshComponent* MeshComp,
		float FadeOutSeconds = 0.3f);
};
