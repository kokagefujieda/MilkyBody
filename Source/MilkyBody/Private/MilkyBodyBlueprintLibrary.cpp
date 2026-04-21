#include "MilkyBodyBlueprintLibrary.h"

#include "MilkyBodyDeformerInstance.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"

namespace MilkyBodyBPPrivate
{
	// Resolve the skinned mesh + impact info under the mouse cursor. Used by both the
	// one-shot and hold-style puni helpers. Returns nullptr if nothing suitable was hit.
	static USkinnedMeshComponent* TraceSkinnedMeshUnderCursor(
		APlayerController* PlayerController,
		ECollisionChannel TraceChannel,
		bool bEnableDebug,
		FVector& OutImpactPoint,
		FVector& OutImpactNormal)
	{
		auto DebugPrint = [bEnableDebug](const FString& Msg, const FColor Color)
		{
			if (bEnableDebug && GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 4.0f, Color, Msg);
				UE_LOG(LogTemp, Log, TEXT("[MilkyBody][Puni] %s"), *Msg);
			}
		};

		if (!PlayerController)
		{
			DebugPrint(TEXT("TraceSkinnedMeshUnderCursor: PlayerController is null"), FColor::Red);
			return nullptr;
		}

		FHitResult Hit;
		USkinnedMeshComponent* MeshComp = nullptr;

		if (PlayerController->GetHitResultUnderCursor(TraceChannel, /*bTraceComplex=*/true, Hit))
		{
			MeshComp = Cast<USkinnedMeshComponent>(Hit.GetComponent());
			if (!MeshComp)
			{
				const FString CompName  = Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("<null>");
				const FString ActorName = Hit.GetActor()     ? Hit.GetActor()->GetName()     : TEXT("<null>");
				DebugPrint(FString::Printf(TEXT("Trace hit non-SkeletalMesh: Actor=%s Comp=%s — trying bounds fallback"), *ActorName, *CompName), FColor::Yellow);
			}
		}
		else
		{
			DebugPrint(TEXT("Trace missed (no Physics Asset?) — trying bounds fallback"), FColor::Yellow);
		}

		// Fallback: ray vs SkinnedMesh AABBs (no Physics Asset required).
		if (!MeshComp)
		{
			FVector RayOrigin, RayDir;
			if (PlayerController->DeprojectMousePositionToWorld(RayOrigin, RayDir))
			{
				const float TraceLen = 100000.0f;
				const FVector RayEnd = RayOrigin + RayDir * TraceLen;
				UWorld* World = PlayerController->GetWorld();
				float BestDistSq = FLT_MAX;
				FVector BestHitPoint = FVector::ZeroVector;

				TArray<AActor*> AllActors;
				UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
				for (AActor* Actor : AllActors)
				{
					TArray<USkinnedMeshComponent*> Comps;
					Actor->GetComponents<USkinnedMeshComponent>(Comps);
					for (USkinnedMeshComponent* Comp : Comps)
					{
						if (!Comp->GetSkinnedAsset()) continue;
						FBox Box = Comp->Bounds.GetBox();
						FVector HitPoint, HitNormal;
						float HitTime;
						if (FMath::LineExtentBoxIntersection(Box, RayOrigin, RayEnd, FVector::ZeroVector, HitPoint, HitNormal, HitTime))
						{
							const float DistSq = FVector::DistSquared(RayOrigin, HitPoint);
							if (DistSq < BestDistSq)
							{
								BestDistSq   = DistSq;
								BestHitPoint = HitPoint;
								MeshComp     = Comp;
							}
						}
					}
				}

				if (MeshComp)
				{
					Hit.ImpactPoint  = BestHitPoint;
					Hit.ImpactNormal = -RayDir.GetSafeNormal();
					DebugPrint(FString::Printf(TEXT("Bounds fallback hit: %s"), *MeshComp->GetName()), FColor::Cyan);
				}
				else
				{
					DebugPrint(TEXT("Bounds fallback also missed — no SkinnedMesh in cursor ray."), FColor::Red);
					return nullptr;
				}
			}
			else
			{
				DebugPrint(TEXT("Trace missed: no hit under cursor."), FColor::Yellow);
				return nullptr;
			}
		}

		OutImpactPoint  = Hit.ImpactPoint;
		OutImpactNormal = Hit.ImpactNormal;
		return MeshComp;
	}
} // namespace MilkyBodyBPPrivate

void UMilkyBodyBlueprintLibrary::ApplyPuni(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius,
	float DurationSeconds)
{
	if (!MeshComp)
	{
		return;
	}
	UMilkyBodyDeformerInstance* Instance = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Instance)
	{
		return;
	}
	Instance->ApplyPushImpulse(WorldLocation, WorldImpulse, Radius, DurationSeconds);
}

bool UMilkyBodyBlueprintLibrary::ApplyPuniUnderCursor(
	APlayerController* PlayerController,
	float PushDepth,
	float Radius,
	float DurationSeconds,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	FVector ImpactPoint, ImpactNormal;
	USkinnedMeshComponent* MeshComp = MilkyBodyBPPrivate::TraceSkinnedMeshUnderCursor(
		PlayerController, TraceChannel, bEnableDebug, ImpactPoint, ImpactNormal);
	if (!MeshComp)
	{
		return false;
	}

	UMilkyBodyDeformerInstance* Instance = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Instance)
	{
		if (bEnableDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Orange,
				FString::Printf(TEXT("[MilkyBody][Puni] Hit SkeletalMesh w/o MilkyBodyDeformer: %s"), *MeshComp->GetName()));
		}
		return false;
	}

	const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
	Instance->ApplyPushImpulse(ImpactPoint, Impulse, Radius, DurationSeconds);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][Puni] hit %s @ %s depth=%.1f r=%.1f dur=%.2fs"),
				*MeshComp->GetName(), *ImpactPoint.ToCompactString(), PushDepth, Radius, DurationSeconds));
	}
	return true;
}

void UMilkyBodyBlueprintLibrary::SetHoldPuni(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius)
{
	if (!MeshComp)
	{
		return;
	}
	UMilkyBodyDeformerInstance* Instance = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Instance)
	{
		return;
	}
	Instance->BeginHoldPush(WorldLocation, WorldImpulse, Radius);
}

USkinnedMeshComponent* UMilkyBodyBlueprintLibrary::SetHoldPuniUnderCursor(
	APlayerController* PlayerController,
	float PushDepth,
	float Radius,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	FVector ImpactPoint, ImpactNormal;
	USkinnedMeshComponent* MeshComp = MilkyBodyBPPrivate::TraceSkinnedMeshUnderCursor(
		PlayerController, TraceChannel, bEnableDebug, ImpactPoint, ImpactNormal);
	if (!MeshComp)
	{
		return nullptr;
	}

	UMilkyBodyDeformerInstance* Instance = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Instance)
	{
		if (bEnableDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Orange,
				FString::Printf(TEXT("[MilkyBody][Hold] Hit SkeletalMesh w/o MilkyBodyDeformer: %s"), *MeshComp->GetName()));
		}
		return nullptr;
	}

	const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
	Instance->BeginHoldPush(ImpactPoint, Impulse, Radius);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][Hold] hold %s @ %s depth=%.1f r=%.1f"),
				*MeshComp->GetName(), *ImpactPoint.ToCompactString(), PushDepth, Radius));
	}
	return MeshComp;
}

void UMilkyBodyBlueprintLibrary::ReleaseHoldPuni(
	USkinnedMeshComponent* MeshComp,
	float FadeOutSeconds)
{
	if (!MeshComp)
	{
		return;
	}
	UMilkyBodyDeformerInstance* Instance = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Instance)
	{
		return;
	}
	Instance->ReleaseHoldPush(FadeOutSeconds);
}
