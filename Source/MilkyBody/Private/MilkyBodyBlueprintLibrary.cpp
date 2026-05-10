#include "MilkyBodyBlueprintLibrary.h"

#include "MilkyBodyDeformerInstance.h"
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
#include "Kismet/GameplayStatics.h"
#include "RawIndexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalRenderPublic.h"
#include "Widgets/SViewport.h"

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

	static void GatherMilkyBodyTargetsInWorld(
		UWorld* World,
		TArray<USkinnedMeshComponent*>& OutComps)
	{
		OutComps.Reset();
		if (!World)
		{
			return;
		}

		TSet<AActor*> VisitedActors;
		TArray<AActor*> AllActors;
		UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);
		for (AActor* Actor : AllActors)
		{
			GatherMilkyBodyTargetsRecursive(Actor, OutComps, VisitedActors);
		}
	}

	static USkinnedMeshComponent* FindBestTraceHitOnTargets(
		const TArray<USkinnedMeshComponent*>& Comps,
		const FVector& Start,
		const FVector& End,
		FHitResult& OutHit)
	{
		const FVector RayDir = (End - Start).GetSafeNormal();
		const FVector Segment = End - Start;
		const float SegmentLenSq = Segment.SizeSquared();
		float BestDistSq = FLT_MAX;
		USkinnedMeshComponent* BestComp = nullptr;

		for (USkinnedMeshComponent* Comp : Comps)
		{
			if (!IsTraceableSkinnedMesh(Comp))
			{
				continue;
			}

			FVector BoxHitPoint, BoxHitNormal;
			float BoxHitTime;
			const bool bBoxHit = FMath::LineExtentBoxIntersection(
				Comp->Bounds.GetBox().ExpandBy(5.0f),
				Start,
				End,
				FVector::ZeroVector,
				BoxHitPoint,
				BoxHitNormal,
				BoxHitTime);

			FVector BroadHitPoint = BoxHitPoint;
			FVector BroadHitNormal = BoxHitNormal;
			if (!bBoxHit)
			{
				if (SegmentLenSq <= SMALL_NUMBER)
				{
					continue;
				}

				const FVector SphereCenter = Comp->Bounds.Origin;
				const float SphereRadius = FMath::Max(Comp->Bounds.SphereRadius * 1.15f, 10.0f);
				const float ClosestT = FMath::Clamp(
					FVector::DotProduct(SphereCenter - Start, Segment) / SegmentLenSq,
					0.0f,
					1.0f);
				const FVector ClosestPoint = Start + Segment * ClosestT;
				if (FVector::DistSquared(ClosestPoint, SphereCenter) > FMath::Square(SphereRadius))
				{
					continue;
				}

				BroadHitPoint = ClosestPoint;
				BroadHitNormal = (ClosestPoint - SphereCenter).GetSafeNormal();
				if (BroadHitNormal.IsNearlyZero())
				{
					BroadHitNormal = -RayDir;
				}
			}

			const float BroadDistSq = FVector::DistSquared(Start, BroadHitPoint);
			if (BroadDistSq >= BestDistSq)
			{
				continue;
			}

			FHitResult CompHit;
			FCollisionQueryParams Params(NAME_None, /*bTraceComplex=*/true);
			if (bBoxHit && Comp->LineTraceComponent(CompHit, Start, End, Params))
			{
				const float HitDistSq = FVector::DistSquared(Start, CompHit.ImpactPoint);
				if (HitDistSq < BestDistSq)
				{
					BestDistSq = HitDistSq;
					OutHit = CompHit;
					OutHit.Component = Comp;
					OutHit.HitObjectHandle = FActorInstanceHandle(Comp->GetOwner());
					BestComp = Comp;
				}
			}
			else
			{
				BestDistSq = BroadDistSq;
				OutHit = FHitResult();
				OutHit.ImpactPoint = BroadHitPoint;
				OutHit.ImpactNormal = BroadHitNormal.IsNearlyZero() ? -RayDir : BroadHitNormal.GetSafeNormal();
				OutHit.Location = BroadHitPoint;
				OutHit.Normal = OutHit.ImpactNormal;
				OutHit.Component = Comp;
				OutHit.HitObjectHandle = FActorInstanceHandle(Comp->GetOwner());
				BestComp = Comp;
			}
		}

		return BestComp;
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

	// ── Core world-space trace ────────────────────────────────────────────────
	//
	// Priority:
	//   1. LineTraceSingleByChannel  (fast; needs Physics Asset + correct channel)
	//   2. Per-component AABB pre-filter → LineTraceComponent
	//      (uses Physics Asset bodies when present, otherwise AABB hit point)
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

		// ── 1. Channel trace ─────────────────────────────────────────────────
		{
			FCollisionQueryParams QP(NAME_None, /*bTraceComplex=*/true);
			FHitResult ChannelHit;
			if (World->LineTraceSingleByChannel(ChannelHit, Start, End, TraceChannel, QP))
			{
				if (USkinnedMeshComponent* HitSkinned = Cast<USkinnedMeshComponent>(ChannelHit.GetComponent()))
				{
					if (IsTraceableSkinnedMesh(HitSkinned))
					{
						OutHit = ChannelHit;
						MeshComp = HitSkinned;
					}
				}

				if (!MeshComp)
				{
					TArray<USkinnedMeshComponent*> ActorTreeComps;
					GatherMilkyBodyTargetsFromActorTree(ChannelHit.GetActor(), ActorTreeComps);
					MeshComp = FindBestTraceHitOnTargets(ActorTreeComps, Start, End, OutHit);
				}
			}
		}

		// ── 2. Fallback: AABB → LineTraceComponent ────────────────────────────
		if (!MeshComp)
		{
			TArray<USkinnedMeshComponent*> WorldComps;
			GatherMilkyBodyTargetsInWorld(World, WorldComps);
			MeshComp = FindBestTraceHitOnTargets(WorldComps, Start, End, OutHit);
		}

		// ── Debug draw ────────────────────────────────────────────────────────
		if (bEnableDebug)
		{
			DrawTraceDebug(
				World, Start, End,
				MeshComp ? OutHit.ImpactPoint  : End,
				MeshComp ? OutHit.ImpactNormal : FVector::ZeroVector,
				MeshComp != nullptr);
		}

		return MeshComp;
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

	// Polygon-precise trace against the skinned mesh: walks every
	// SkinnedMeshComponent in the world, AABB-prefilters, then CPU-skins the
	// active LOD and tests each triangle. Returns the closest hit and fills
	// OutHit. NOT cheap — call at click rate, not per-frame.
	static USkinnedMeshComponent* TraceSkinnedMeshPolygon(
		UWorld* World,
		const FVector& Start,
		const FVector& End,
		bool bEnableDebug,
		FHitResult& OutHit)
	{
		if (!World) return nullptr;

		USkinnedMeshComponent* BestComp   = nullptr;
		float                  BestT      = 1.0f + KINDA_SMALL_NUMBER;
		FVector                BestHit    = FVector::ZeroVector;
		FVector                BestNormal = FVector::ZeroVector;

		TArray<USkinnedMeshComponent*> Comps;
		GatherMilkyBodyTargetsInWorld(World, Comps);
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

			FSkeletalMeshRenderData* RenderData = Comp->GetSkeletalMeshRenderData();
			if (!RenderData) continue;

			const int32 LODIndex = Comp->MeshObject
				? Comp->MeshObject->GetLOD()
				: Comp->GetPredictedLODLevel();
			if (!RenderData->LODRenderData.IsValidIndex(LODIndex)) continue;

			FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
			const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
			if (!SkinWeightBuffer) continue;

			const FRawStaticIndexBuffer16or32Interface* IndexBuffer =
				LODData.MultiSizeIndexContainer.GetIndexBuffer();
			if (!IndexBuffer || IndexBuffer->Num() == 0) continue;

			// CPU-skin every vertex of this LOD (component space)
			TArray<FMatrix44f> RefToLocals;
			Comp->CacheRefToLocalMatrices(RefToLocals);
			TArray<FVector3f> Positions;
			USkinnedMeshComponent::ComputeSkinnedPositions(
				Comp, Positions, RefToLocals, LODData, *SkinWeightBuffer);
			if (Positions.Num() == 0) continue;

			// Trace in component space to avoid per-triangle world transforms
			const FTransform& Xform     = Comp->GetComponentTransform();
			const FVector     LocalStart = Xform.InverseTransformPosition(Start);
			const FVector     LocalEnd   = Xform.InverseTransformPosition(End);
			const FVector     LocalDir   = LocalEnd - LocalStart;

			const int32 NumIndices = IndexBuffer->Num();
			for (int32 i = 0; i + 2 < NumIndices; i += 3)
			{
				const uint32 I0 = IndexBuffer->Get(i);
				const uint32 I1 = IndexBuffer->Get(i + 1);
				const uint32 I2 = IndexBuffer->Get(i + 2);
				if (!Positions.IsValidIndex(I0) ||
					!Positions.IsValidIndex(I1) ||
					!Positions.IsValidIndex(I2))
				{
					continue;
				}
				const FVector V0((FVector3f)Positions[I0]);
				const FVector V1((FVector3f)Positions[I1]);
				const FVector V2((FVector3f)Positions[I2]);

				float HitT;
				if (RayTriangleIntersect(LocalStart, LocalDir, V0, V1, V2, HitT)
					&& HitT <= 1.0f && HitT < BestT)
				{
					BestT = HitT;
					const FVector LocalHit = LocalStart + LocalDir * HitT;
					BestHit = Xform.TransformPosition(LocalHit);
					const FVector LocalN =
						FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();
					// Rotation-only transform (skeletal characters typically use uniform scale)
					BestNormal = Xform.GetRotation().RotateVector(LocalN).GetSafeNormal();
					BestComp   = Comp;
				}
			}
		}

		if (BestComp)
		{
			OutHit                = FHitResult();
			OutHit.ImpactPoint    = BestHit;
			OutHit.ImpactNormal   = BestNormal;
			OutHit.Location       = BestHit;
			OutHit.Normal         = BestNormal;
			OutHit.Component      = BestComp;
			OutHit.HitObjectHandle = FActorInstanceHandle(BestComp->GetOwner());
		}

		if (bEnableDebug)
		{
			DrawTraceDebug(
				World, Start, End,
				BestComp ? BestHit    : End,
				BestComp ? BestNormal : FVector::ZeroVector,
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
