#include "MilkyBodyBlueprintLibrary.h"

#include "MilkyBodyCPUSkinning.h"
#include "MilkyBodyDeformerInstance.h"
#include "MilkyBodyPrivate.h"
#include "Components/ChildActorComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/HitResult.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "RawIndexBuffer.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SViewport.h"

DECLARE_CYCLE_STAT(TEXT("Polygon Trace"), STAT_MilkyBody_PolygonTrace, STATGROUP_MilkyBody);

// ─────────────────────────────────────────────────────────────────────────────
namespace MilkyBodyBPPrivate
{
	static constexpr float kDebugDuration = 2.0f;

	static bool IsTraceableSkinnedMesh(USkinnedMeshComponent* Comp)
	{
		return Comp &&
			Comp->GetSkinnedAsset() &&
			Comp->IsRegistered() &&
			Comp->IsVisible();
	}

	static void GatherMilkyBodyTargetsRecursive(
		AActor* Actor,
		TArray<USkinnedMeshComponent*>& OutComps,
		TSet<AActor*>& VisitedActors)
	{
		if (!Actor || VisitedActors.Contains(Actor))
		{
			return;
		}
		VisitedActors.Add(Actor);

		TArray<USkinnedMeshComponent*> LocalComps;
		Actor->GetComponents<USkinnedMeshComponent>(LocalComps);
		for (USkinnedMeshComponent* Comp : LocalComps)
		{
			if (IsTraceableSkinnedMesh(Comp))
			{
				OutComps.Add(Comp);
			}
		}

		TArray<UChildActorComponent*> ChildActorComps;
		Actor->GetComponents<UChildActorComponent>(ChildActorComps);
		for (UChildActorComponent* ChildActorComp : ChildActorComps)
		{
			if (ChildActorComp)
			{
				GatherMilkyBodyTargetsRecursive(ChildActorComp->GetChildActor(), OutComps, VisitedActors);
			}
		}
	}

	static void GatherMilkyBodyTargetsFromActorTree(
		AActor* Actor,
		TArray<USkinnedMeshComponent*>& OutComps)
	{
		TSet<AActor*> VisitedActors;
		GatherMilkyBodyTargetsRecursive(Actor, OutComps, VisitedActors);
	}

	static bool DeprojectCursorToWorldIgnoringWidgets(
		APlayerController* PC,
		FVector& OutRayOrigin,
		FVector& OutRayDir)
	{
		if (!PC)
		{
			return false;
		}

		if (PC->DeprojectMousePositionToWorld(OutRayOrigin, OutRayDir))
		{
			return true;
		}

		float MouseX = 0.0f;
		float MouseY = 0.0f;
		if (PC->GetMousePosition(MouseX, MouseY) &&
			PC->DeprojectScreenPositionToWorld(MouseX, MouseY, OutRayOrigin, OutRayDir))
		{
			return true;
		}

		UWorld* World = PC->GetWorld();
		UGameViewportClient* GameViewport = World ? World->GetGameViewport() : nullptr;
		if (!GameViewport || !FSlateApplication::IsInitialized())
		{
			return false;
		}

		TSharedPtr<SViewport> ViewportWidget = GameViewport->GetGameViewportWidget();
		if (!ViewportWidget.IsValid())
		{
			return false;
		}

		const FGeometry& ViewportGeometry = ViewportWidget->GetCachedGeometry();
		const FVector2D LocalCursor = ViewportGeometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
		const FVector2D LocalSize = ViewportGeometry.GetLocalSize();
		if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
		{
			return false;
		}

		FVector2D ViewportSize = LocalSize;
		GameViewport->GetViewportSize(ViewportSize);
		const float ScreenX = LocalCursor.X * ViewportSize.X / LocalSize.X;
		const float ScreenY = LocalCursor.Y * ViewportSize.Y / LocalSize.Y;
		return PC->DeprojectScreenPositionToWorld(ScreenX, ScreenY, OutRayOrigin, OutRayDir);
	}

	// 3-D debug drawing for a single trace result.
	static void DrawTraceDebug(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		const FVector& HitPoint,
		const FVector& HitNormal,
		bool bHit)
	{
		if (!World) return;

		if (bHit)
		{
			// Green ray up to hit, faint red for the rest
			DrawDebugLine(World, Start, HitPoint,
				FColor::Green, false, kDebugDuration, 0, 1.5f);
			DrawDebugLine(World, HitPoint, End,
				FColor(100, 0, 0), false, kDebugDuration, 0, 0.5f);
			// Yellow sphere at impact point
			DrawDebugSphere(World, HitPoint, 4.0f, 8, FColor::Yellow, false, kDebugDuration);
			// Cyan arrow along impact normal
			DrawDebugDirectionalArrow(
				World,
				HitPoint,
				HitPoint + HitNormal.GetSafeNormal() * 20.0f,
				6.0f, FColor::Cyan, false, kDebugDuration, 0, 1.0f);
		}
		else
		{
			// Miss: full red ray
			DrawDebugLine(World, Start, End, FColor::Red, false, kDebugDuration, 0, 1.0f);
		}
	}

	// Möller–Trumbore ray-vs-triangle. Dir is *not* normalized; OutT is in the
	// same parameterization as Dir, so OutT in [0,1] means the hit lies on the
	// segment Origin → Origin+Dir.
	static bool RayTriangleIntersect(
		const FVector& Origin, const FVector& Dir,
		const FVector& V0, const FVector& V1, const FVector& V2,
		float& OutT)
	{
		constexpr float Epsilon = 1e-8f;
		const FVector E1 = V1 - V0;
		const FVector E2 = V2 - V0;
		const FVector P  = FVector::CrossProduct(Dir, E2);
		const float   Det = FVector::DotProduct(E1, P);
		if (FMath::Abs(Det) < Epsilon) return false;
		const float   InvDet = 1.0f / Det;
		const FVector T = Origin - V0;
		const float   U = FVector::DotProduct(T, P) * InvDet;
		if (U < 0.0f || U > 1.0f) return false;
		const FVector Q = FVector::CrossProduct(T, E1);
		const float   V = FVector::DotProduct(Dir, Q) * InvDet;
		if (V < 0.0f || U + V > 1.0f) return false;
		OutT = FVector::DotProduct(E2, Q) * InvDet;
		return OutT >= 0.0f;
	}

	// Polygon-precise trace against the CPU-skinned triangles of Comps
	// (AABB-prefiltered). Returns the closest hit on the segment and fills
	// OutHit; the impact normal always faces the ray origin. Cost scales with
	// the triangle count of the meshes whose bounds the segment crosses.
	static USkinnedMeshComponent* PolygonTraceComponents(
		const TArray<USkinnedMeshComponent*>& Comps,
		const FVector& Start,
		const FVector& End,
		FHitResult& OutHit)
	{
		SCOPE_CYCLE_COUNTER(STAT_MilkyBody_PolygonTrace);

		USkinnedMeshComponent* BestComp   = nullptr;
		float                  BestT      = 1.0f + KINDA_SMALL_NUMBER;
		FVector                BestHit    = FVector::ZeroVector;
		FVector                BestNormal = FVector::ZeroVector;
		const FVector          RayDir     = (End - Start).GetSafeNormal();

		for (USkinnedMeshComponent* Comp : Comps)
		{
			if (!IsTraceableSkinnedMesh(Comp)) continue;

			// AABB pre-filter (cheap)
			FVector BoxHitPoint, BoxHitNormal;
			float   BoxHitTime;
			if (!FMath::LineExtentBoxIntersection(
					Comp->Bounds.GetBox(), Start, End,
					FVector::ZeroVector,
					BoxHitPoint, BoxHitNormal, BoxHitTime))
			{
				continue;
			}

			MilkyBody::FCPUSkinnedMesh Mesh;
			if (!MilkyBody::BuildCPUSkinnedMesh(Comp, Mesh)) continue;

			// Trace in component space to avoid per-triangle world transforms.
			// An affine transform preserves the segment parameter T, so T stays
			// comparable across components.
			const FTransform& Xform      = Mesh.ComponentToWorld;
			const FVector     LocalStart = Xform.InverseTransformPosition(Start);
			const FVector     LocalEnd   = Xform.InverseTransformPosition(End);
			const FVector     LocalDir   = LocalEnd - LocalStart;

			const int32 NumIndices = Mesh.NumIndices();
			for (int32 i = 0; i + 2 < NumIndices; i += 3)
			{
				const uint32 I0 = Mesh.GetIndex(i);
				const uint32 I1 = Mesh.GetIndex(i + 1);
				const uint32 I2 = Mesh.GetIndex(i + 2);
				if (!Mesh.Positions.IsValidIndex(I0) ||
					!Mesh.Positions.IsValidIndex(I1) ||
					!Mesh.Positions.IsValidIndex(I2))
				{
					continue;
				}
				const FVector V0(Mesh.Positions[I0]);
				const FVector V1(Mesh.Positions[I1]);
				const FVector V2(Mesh.Positions[I2]);

				float HitT;
				if (RayTriangleIntersect(LocalStart, LocalDir, V0, V1, V2, HitT)
					&& HitT <= 1.0f && HitT < BestT)
				{
					BestT = HitT;
					BestHit = Xform.TransformPosition(LocalStart + LocalDir * HitT);
					const FVector LocalN = FVector::CrossProduct(V1 - V0, V2 - V0);
					FVector WorldN = Xform.TransformVectorNoScale(LocalN).GetSafeNormal();
					if (FVector::DotProduct(WorldN, RayDir) > 0.0f)
					{
						WorldN = -WorldN; // face the viewer: the push goes away from the ray origin
					}
					BestNormal = WorldN;
					BestComp   = Comp;
				}
			}
		}

		if (BestComp)
		{
			OutHit                 = FHitResult();
			OutHit.ImpactPoint     = BestHit;
			OutHit.ImpactNormal    = BestNormal;
			OutHit.Location        = BestHit;
			OutHit.Normal          = BestNormal;
			OutHit.Component       = BestComp;
			OutHit.HitObjectHandle = FActorInstanceHandle(BestComp->GetOwner());
		}
		return BestComp;
	}

	// ── Core world-space trace ────────────────────────────────────────────────
	//
	// Priority:
	//   1. LineTraceSingleByChannel. A skinned mesh hit (Physics Asset body) is
	//      used directly. Any other blocking hit (capsule, accessory, wall)
	//      polygon-traces that actor's own skinned meshes, and occludes
	//      everything behind it.
	//   2. Polygon-precise trace over every skinned mesh in the world, up to the
	//      blocking hit (works without a Physics Asset).
	//
	// Returns the hit SkinnedMeshComponent and fills OutHit on success.
	// Draws 3-D debug when bEnableDebug is true.
	static USkinnedMeshComponent* TraceSkinnedMesh(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		ECollisionChannel TraceChannel,
		bool bEnableDebug,
		FHitResult& OutHit)
	{
		if (!World) return nullptr;

		USkinnedMeshComponent* MeshComp = nullptr;
		FVector FallbackEnd = End;

		// ── 1. Channel trace ─────────────────────────────────────────────────
		{
			FCollisionQueryParams QP(NAME_None, /*bTraceComplex=*/true);
			FHitResult ChannelHit;
			if (World->LineTraceSingleByChannel(ChannelHit, Start, End, TraceChannel, QP))
			{
				USkinnedMeshComponent* HitSkinned = Cast<USkinnedMeshComponent>(ChannelHit.GetComponent());
				if (HitSkinned && IsTraceableSkinnedMesh(HitSkinned))
				{
					OutHit = ChannelHit;
					MeshComp = HitSkinned;
				}
				else
				{
					TArray<USkinnedMeshComponent*> ActorTreeComps;
					GatherMilkyBodyTargetsFromActorTree(ChannelHit.GetActor(), ActorTreeComps);
					MeshComp = PolygonTraceComponents(ActorTreeComps, Start, End, OutHit);
					FallbackEnd = ChannelHit.ImpactPoint;
				}
			}
		}

		// ── 2. Fallback: polygon-precise trace ───────────────────────────────
		if (!MeshComp)
		{
			TArray<USkinnedMeshComponent*> WorldComps;
			MilkyBody::GatherSkinnedMeshesInWorld(World, /*bRequireMilkyBody=*/false, WorldComps);
			MeshComp = PolygonTraceComponents(WorldComps, Start, FallbackEnd, OutHit);
		}

		// ── Debug draw ────────────────────────────────────────────────────────
		if (bEnableDebug || CVarMilkyBodyDebugDraw.GetValueOnGameThread() != 0)
		{
			DrawTraceDebug(
				World, Start, End,
				MeshComp ? OutHit.ImpactPoint  : End,
				MeshComp ? OutHit.ImpactNormal : FVector::ZeroVector,
				MeshComp != nullptr);
		}

		return MeshComp;
	}

	// Polygon-precise trace against every skinned mesh in the world, ignoring
	// collision entirely. NOT cheap — call at click rate, not per-frame.
	static USkinnedMeshComponent* TraceSkinnedMeshPolygon(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		bool bEnableDebug,
		FHitResult& OutHit)
	{
		if (!World) return nullptr;

		TArray<USkinnedMeshComponent*> Comps;
		MilkyBody::GatherSkinnedMeshesInWorld(World, /*bRequireMilkyBody=*/false, Comps);
		USkinnedMeshComponent* BestComp = PolygonTraceComponents(Comps, Start, End, OutHit);

		if (bEnableDebug || CVarMilkyBodyDebugDraw.GetValueOnGameThread() != 0)
		{
			DrawTraceDebug(
				World, Start, End,
				BestComp ? OutHit.ImpactPoint  : End,
				BestComp ? OutHit.ImpactNormal : FVector::ZeroVector,
				BestComp != nullptr);
		}
		return BestComp;
	}

	// Cursor wrapper: deprojects the mouse position, then calls TraceSkinnedMesh.
	static USkinnedMeshComponent* TraceSkinnedMeshUnderCursor(
		APlayerController* PC,
		ECollisionChannel  TraceChannel,
		bool               bEnableDebug,
		FVector&           OutImpactPoint,
		FVector&           OutImpactNormal)
	{
		if (!PC) return nullptr;

		FVector RayOrigin, RayDir;
		if (!DeprojectCursorToWorldIgnoringWidgets(PC, RayOrigin, RayDir)) return nullptr;

		const FVector RayEnd = RayOrigin + RayDir * 100000.0f;

		FHitResult Hit;
		USkinnedMeshComponent* Comp =
			TraceSkinnedMesh(PC->GetWorld(), RayOrigin, RayEnd, TraceChannel, bEnableDebug, Hit);

		if (Comp)
		{
			OutImpactPoint  = Hit.ImpactPoint;
			OutImpactNormal = Hit.ImpactNormal;
		}
		return Comp;
	}

	// Shared helper: resolve and return the MilkyBodyDeformerInstance on a component.
	static UMilkyBodyDeformerInstance* GetDeformerInstance(
		USkinnedMeshComponent* MeshComp, bool bEnableDebug)
	{
		auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
		if (!Inst && bEnableDebug && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Orange,
				FString::Printf(TEXT("[MilkyBody] '%s' has no MilkyBodyDeformer"),
					*MeshComp->GetName()));
		}
		return Inst;
	}
} // namespace MilkyBodyBPPrivate

// ─────────────────────────────────────────────────────────────────────────────
// Mesh resolution helper
// ─────────────────────────────────────────────────────────────────────────────

namespace MilkyBodyBPPrivate
{
	// 「アセットさえあれば良い」というゆるい条件で SkinnedMesh を集めるための走査関数。
	// 可視性や登録状態を要求する IsTraceableSkinnedMesh と違い、UI や非表示状態でも拾える。
	static void GatherAnySkinnedRecursive(AActor* Actor, TArray<USkinnedMeshComponent*>& Out, TSet<AActor*>& Visited)
	{
		if (!Actor || Visited.Contains(Actor)) { return; }
		Visited.Add(Actor);

		TArray<USkinnedMeshComponent*> LocalComps;
		Actor->GetComponents<USkinnedMeshComponent>(LocalComps);
		for (USkinnedMeshComponent* Comp : LocalComps)
		{
			if (Comp && Comp->GetSkinnedAsset())
			{
				Out.Add(Comp);
			}
		}

		TArray<UChildActorComponent*> ChildActorComps;
		Actor->GetComponents<UChildActorComponent>(ChildActorComps);
		for (UChildActorComponent* ChildActorComp : ChildActorComps)
		{
			if (ChildActorComp)
			{
				GatherAnySkinnedRecursive(ChildActorComp->GetChildActor(), Out, Visited);
			}
		}
	}
}

USkinnedMeshComponent* UMilkyBodyBlueprintLibrary::FindMilkyBodyMeshOnActor(AActor* Actor, bool bRequireMilkyBody)
{
	if (!Actor)
	{
		return nullptr;
	}

	// 1) ParentActor チェーンを root まで遡る（ChildActorComponent でネストされていても拾う）
	AActor* Root = Actor;
	for (int32 i = 0; i < 32; ++i)
	{
		AActor* Parent = Root->GetParentActor();
		if (!Parent || Parent == Root) { break; }
		Root = Parent;
	}

	// 2) root から下向きに、自身 + ChildActor 配下を再帰探索（可視性は問わない）
	TArray<USkinnedMeshComponent*> Candidates;
	TSet<AActor*> Visited;
	MilkyBodyBPPrivate::GatherAnySkinnedRecursive(Root, Candidates, Visited);

	// 3) MilkyBodyDeformer 付きを優先で返す。なければ条件に応じて素の最初の SkinnedMesh を返す。
	USkinnedMeshComponent* FirstAny = nullptr;
	for (USkinnedMeshComponent* Comp : Candidates)
	{
		if (!Comp) { continue; }
		if (!FirstAny) { FirstAny = Comp; }
		if (Cast<UMilkyBodyDeformerInstance>(Comp->GetMeshDeformerInstance()))
		{
			return Comp;
		}
	}
	return bRequireMilkyBody ? nullptr : FirstAny;
}

// ─────────────────────────────────────────────────────────────────────────────
// Direct API
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyBlueprintLibrary::ApplyPuni(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius,
	float DurationSeconds)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->ApplyPushImpulse(WorldLocation, WorldImpulse, Radius, DurationSeconds);
}

FName UMilkyBodyBlueprintLibrary::SetHoldPuni(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius)
{
	if (!MeshComp) return NAME_None;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (Inst)
	{
		Inst->BeginHoldPush(WorldLocation, WorldImpulse, Radius);
	}
	return MeshComp->FindClosestBone(WorldLocation);
}

void UMilkyBodyBlueprintLibrary::ReleaseHoldPuni(
	USkinnedMeshComponent* MeshComp,
	float FadeOutSeconds)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->ReleaseHoldPush(FadeOutSeconds);
}

// ─────────────────────────────────────────────────────────────────────────────
// World-space line trace
// ─────────────────────────────────────────────────────────────────────────────

bool UMilkyBodyBlueprintLibrary::LineTracePuni(
	UObject* WorldContextObject,
	FVector Start,
	FVector End,
	float PushDepth,
	float Radius,
	float DurationSeconds,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return false;

	FHitResult Hit;
	USkinnedMeshComponent* MeshComp =
		MilkyBodyBPPrivate::TraceSkinnedMesh(World, Start, End, TraceChannel, bEnableDebug, Hit);
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -Hit.ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->ApplyPushImpulse(Hit.ImpactPoint, Impulse, Radius, DurationSeconds);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][LineTrace] hit '%s' @ %s depth=%.1f r=%.1f"),
				*MeshComp->GetName(), *Hit.ImpactPoint.ToCompactString(), PushDepth, Radius));
	}
	return true;
}

USkinnedMeshComponent* UMilkyBodyBlueprintLibrary::LineTraceHoldPuni(
	UObject* WorldContextObject,
	FVector Start,
	FVector End,
	float PushDepth,
	float Radius,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return nullptr;

	FHitResult Hit;
	USkinnedMeshComponent* MeshComp =
		MilkyBodyBPPrivate::TraceSkinnedMesh(World, Start, End, TraceChannel, bEnableDebug, Hit);
	if (!MeshComp) return nullptr;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return MeshComp;

	const FVector Impulse = -Hit.ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->BeginHoldPush(Hit.ImpactPoint, Impulse, Radius);
	return MeshComp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cursor-based wrappers
// ─────────────────────────────────────────────────────────────────────────────

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
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->ApplyPushImpulse(ImpactPoint, Impulse, Radius, DurationSeconds);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][Cursor] hit '%s' @ %s depth=%.1f r=%.1f"),
				*MeshComp->GetName(), *ImpactPoint.ToCompactString(), PushDepth, Radius));
	}
	return true;
}

FName UMilkyBodyBlueprintLibrary::SetHoldPuniUnderCursor(
	APlayerController* PlayerController,
	USkinnedMeshComponent*& OutMeshComp,
	float PushDepth,
	float Radius,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	OutMeshComp = nullptr;

	FVector ImpactPoint, ImpactNormal;
	USkinnedMeshComponent* MeshComp = MilkyBodyBPPrivate::TraceSkinnedMeshUnderCursor(
		PlayerController, TraceChannel, bEnableDebug, ImpactPoint, ImpactNormal);
	if (!MeshComp) return NAME_None;

	OutMeshComp = MeshComp;
	if (UMilkyBodyDeformerInstance* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance()))
	{
		const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
		Inst->BeginHoldPush(ImpactPoint, Impulse, Radius);
	}
	else if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Yellow,
			FString::Printf(TEXT("[MilkyBody][Cursor] hit '%s' without MilkyBodyDeformer; returning mesh/bone only"),
				*MeshComp->GetName()));
	}

	return MeshComp->FindClosestBone(ImpactPoint);
}

// ─────────────────────────────────────────────────────────────────────────────
// Polygon-precise (skinned mesh) line trace
// ─────────────────────────────────────────────────────────────────────────────

bool UMilkyBodyBlueprintLibrary::LineTracePuniPolygon(
	UObject* WorldContextObject,
	FVector Start,
	FVector End,
	float PushDepth,
	float Radius,
	float DurationSeconds,
	bool bEnableDebug)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return false;

	FHitResult Hit;
	USkinnedMeshComponent* MeshComp =
		MilkyBodyBPPrivate::TraceSkinnedMeshPolygon(World, Start, End, bEnableDebug, Hit);
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -Hit.ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->ApplyPushImpulse(Hit.ImpactPoint, Impulse, Radius, DurationSeconds);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][Polygon] hit '%s' @ %s depth=%.1f r=%.1f"),
				*MeshComp->GetName(), *Hit.ImpactPoint.ToCompactString(), PushDepth, Radius));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Kneading
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyBlueprintLibrary::BeginKneading(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius,
	int32 NumPoints,
	float ScatterRadius,
	float MotionSpeed,
	float RandomSeed)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->BeginKneading(WorldLocation, WorldImpulse, Radius, NumPoints, ScatterRadius, MotionSpeed, RandomSeed);
}

void UMilkyBodyBlueprintLibrary::EndKneading(USkinnedMeshComponent* MeshComp)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->EndKneading();
}

bool UMilkyBodyBlueprintLibrary::BeginKneadingUnderCursor(
	APlayerController* PlayerController,
	USkinnedMeshComponent*& OutMeshComp,
	float PushDepth,
	float Radius,
	int32 NumPoints,
	float ScatterRadius,
	float MotionSpeed,
	float RandomSeed,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	OutMeshComp = nullptr;

	FVector ImpactPoint, ImpactNormal;
	USkinnedMeshComponent* MeshComp = MilkyBodyBPPrivate::TraceSkinnedMeshUnderCursor(
		PlayerController, TraceChannel, bEnableDebug, ImpactPoint, ImpactNormal);
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->BeginKneading(ImpactPoint, Impulse, Radius, NumPoints, ScatterRadius, MotionSpeed, RandomSeed);

	OutMeshComp = MeshComp;
	return true;
}

bool UMilkyBodyBlueprintLibrary::ApplyPuniFromCamera(
	APlayerController* PlayerController,
	float TraceDistance,
	float PushDepth,
	float Radius,
	float DurationSeconds,
	bool bEnableDebug)
{
	if (!PlayerController) return false;
	UWorld* World = PlayerController->GetWorld();
	if (!World) return false;

	FVector  CamLoc;
	FRotator CamRot;
	PlayerController->GetPlayerViewPoint(CamLoc, CamRot);

	const FVector Start = CamLoc;
	const FVector End   = CamLoc + CamRot.Vector() * TraceDistance;

	FHitResult Hit;
	USkinnedMeshComponent* MeshComp =
		MilkyBodyBPPrivate::TraceSkinnedMeshPolygon(World, Start, End, bEnableDebug, Hit);
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -Hit.ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->ApplyPushImpulse(Hit.ImpactPoint, Impulse, Radius, DurationSeconds);

	if (bEnableDebug && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::Green,
			FString::Printf(TEXT("[MilkyBody][Camera] hit '%s' @ %s depth=%.1f r=%.1f"),
				*MeshComp->GetName(), *Hit.ImpactPoint.ToCompactString(), PushDepth, Radius));
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Momi (揉み — circular kneading)
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyBlueprintLibrary::BeginMomi(
	USkinnedMeshComponent* MeshComp,
	FVector WorldLocation,
	FVector WorldImpulse,
	float Radius,
	int32 NumPoints,
	float OrbitRadius,
	float OrbitSpeed,
	float SpeedVariation,
	float DepthVariation,
	float RandomSeed)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->BeginMomi(WorldLocation, WorldImpulse, Radius, NumPoints, OrbitRadius, OrbitSpeed, SpeedVariation, DepthVariation, RandomSeed);
}

void UMilkyBodyBlueprintLibrary::EndMomi(USkinnedMeshComponent* MeshComp)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->EndMomi();
}

bool UMilkyBodyBlueprintLibrary::BeginMomiUnderCursor(
	APlayerController* PlayerController,
	USkinnedMeshComponent*& OutMeshComp,
	float PushDepth,
	float Radius,
	int32 NumPoints,
	float OrbitRadius,
	float OrbitSpeed,
	float SpeedVariation,
	float DepthVariation,
	float RandomSeed,
	ECollisionChannel TraceChannel,
	bool bEnableDebug)
{
	OutMeshComp = nullptr;

	FVector ImpactPoint, ImpactNormal;
	USkinnedMeshComponent* MeshComp = MilkyBodyBPPrivate::TraceSkinnedMeshUnderCursor(
		PlayerController, TraceChannel, bEnableDebug, ImpactPoint, ImpactNormal);
	if (!MeshComp) return false;

	UMilkyBodyDeformerInstance* Inst =
		MilkyBodyBPPrivate::GetDeformerInstance(MeshComp, bEnableDebug);
	if (!Inst) return false;

	const FVector Impulse = -ImpactNormal.GetSafeNormal() * PushDepth;
	Inst->BeginMomi(ImpactPoint, Impulse, Radius, NumPoints, OrbitRadius, OrbitSpeed, SpeedVariation, DepthVariation, RandomSeed);

	OutMeshComp = MeshComp;
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// State queries / release helpers
// ─────────────────────────────────────────────────────────────────────────────

float UMilkyBodyBlueprintLibrary::GetPuniAmount(USkinnedMeshComponent* MeshComp)
{
	if (!MeshComp) return 0.0f;
	const auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	return Inst ? Inst->GetPuniAmount() : 0.0f;
}

bool UMilkyBodyBlueprintLibrary::IsPuniActive(USkinnedMeshComponent* MeshComp)
{
	if (!MeshComp) return false;
	const auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	return Inst && Inst->IsPuniActive();
}

void UMilkyBodyBlueprintLibrary::ReleaseAllPuni(USkinnedMeshComponent* MeshComp, float FadeOutSeconds)
{
	if (!MeshComp) return;
	auto* Inst = Cast<UMilkyBodyDeformerInstance>(MeshComp->GetMeshDeformerInstance());
	if (!Inst) return;
	Inst->ReleaseAll(FadeOutSeconds);
}

void UMilkyBodyBlueprintLibrary::ReleaseAllPuniInWorld(UObject* WorldContextObject, float FadeOutSeconds)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return;

	// Includes hidden / unregistered meshes so nothing can stay dented.
	for (TObjectIterator<USkinnedMeshComponent> It; It; ++It)
	{
		USkinnedMeshComponent* Comp = *It;
		if (!IsValid(Comp) || Comp->GetWorld() != World) continue;
		if (auto* Inst = Cast<UMilkyBodyDeformerInstance>(Comp->GetMeshDeformerInstance()))
		{
			Inst->ReleaseAll(FadeOutSeconds);
		}
	}
}
