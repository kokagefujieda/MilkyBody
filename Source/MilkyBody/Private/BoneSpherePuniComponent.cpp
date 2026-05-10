#include "BoneSpherePuniComponent.h"

#include "MilkyBodyDeformerInstance.h"

#include "Components/ChildActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalRenderPublic.h"
#include "DrawDebugHelpers.h"

namespace
{
	// 与えられたアクターから ParentActor チェーンの最上位（root）まで遡る。
	// ChildActorComponent でネストされた配置でも、最初に階層全体のトップを取り出すための足場。
	AActor* GetActorHierarchyRoot(AActor* Actor)
	{
		if (!Actor) { return nullptr; }
		AActor* Current = Actor;
		// SafetyMax: 病的な循環参照は事実上発生しないが念のため。
		for (int32 i = 0; i < 32; ++i)
		{
			AActor* Parent = Current->GetParentActor();
			if (!Parent || Parent == Current)
			{
				break;
			}
			Current = Parent;
		}
		return Current;
	}

	// 自身 + UChildActorComponent の子孫を再帰的に巡って最初の USkeletalMeshComponent を返す。
	USkeletalMeshComponent* FindSkeletalMeshDownTree(AActor* Actor, TSet<AActor*>& Visited)
	{
		if (!Actor || Visited.Contains(Actor))
		{
			return nullptr;
		}
		Visited.Add(Actor);

		if (USkeletalMeshComponent* Direct = Actor->FindComponentByClass<USkeletalMeshComponent>())
		{
			return Direct;
		}

		TArray<UChildActorComponent*> ChildActorComps;
		Actor->GetComponents<UChildActorComponent>(ChildActorComps);
		for (UChildActorComponent* ChildActorComp : ChildActorComps)
		{
			if (!ChildActorComp)
			{
				continue;
			}
			if (USkeletalMeshComponent* Found = FindSkeletalMeshDownTree(ChildActorComp->GetChildActor(), Visited))
			{
				return Found;
			}
		}
		return nullptr;
	}

	// アクター階層のどこにあっても拾えるよう、まず ParentActor チェーンを辿って root を求め、
	// そこから下方向に全て探す。
	// これにより以下のすべての構成で SkeletalMesh の自動解決が成立する:
	//   * Owner 直下に SkeletalMesh
	//   * Owner の ChildActor 配下に SkeletalMesh
	//   * Owner が ChildActor のスポーン物で、親アクター（または兄弟）に SkeletalMesh
	USkeletalMeshComponent* FindSkeletalMeshInActorHierarchy(AActor* StartActor)
	{
		AActor* Root = GetActorHierarchyRoot(StartActor);
		TSet<AActor*> Visited;
		return FindSkeletalMeshDownTree(Root, Visited);
	}

	// アクターツリー全体（自身 + ChildActor 子孫）から SkinnedMeshComponent を集める。
	void GatherSkinnedInActorTree(AActor* Actor, TArray<USkinnedMeshComponent*>& Out, TSet<AActor*>& Visited)
	{
		if (!Actor || Visited.Contains(Actor))
		{
			return;
		}
		Visited.Add(Actor);

		TArray<USkinnedMeshComponent*> LocalComps;
		Actor->GetComponents<USkinnedMeshComponent>(LocalComps);
		Out.Append(LocalComps);

		TArray<UChildActorComponent*> ChildActorComps;
		Actor->GetComponents<UChildActorComponent>(ChildActorComps);
		for (UChildActorComponent* ChildActorComp : ChildActorComps)
		{
			if (ChildActorComp)
			{
				GatherSkinnedInActorTree(ChildActorComp->GetChildActor(), Out, Visited);
			}
		}
	}
}

UBoneSpherePuniComponent::UBoneSpherePuniComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;  // run after animation has updated bone transforms
}

void UBoneSpherePuniComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UBoneSpherePuniComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopDriving();
	Super::EndPlay(EndPlayReason);
}

void UBoneSpherePuniComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	TimeSinceTargetRefresh += DeltaTime;
	if (bAutoTick)
	{
		ApplyNow();
	}
}

void UBoneSpherePuniComponent::RefreshAutoTargets()
{
	CachedAutoTargets.Reset();
	TimeSinceTargetRefresh = 1e9f;
}

USkeletalMeshComponent* UBoneSpherePuniComponent::ResolveSource() const
{
	USkeletalMeshComponent* Source = SourceMesh.Get();
	if (!Source)
	{
		if (AActor* Owner = GetOwner())
		{
			// アクター階層全体（親→子→孫）を探索する。
			// 旧コードは Owner->FindComponentByClass のみで、
			//   * SkeletalMesh が ChildActor 配下にあるケース
			//   * このコンポーネントが ChildActor 側にあって SkeletalMesh が親側にあるケース
			// のいずれも解決できていなかった。
			Source = FindSkeletalMeshInActorHierarchy(Owner);
		}
	}
	return Source;
}

void UBoneSpherePuniComponent::GatherTargetCandidates(USkeletalMeshComponent* InSource, TArray<USkinnedMeshComponent*>& OutTargets)
{
	OutTargets.Reset();
	if (USkinnedMeshComponent* Explicit = TargetBody.Get())
	{
		// Explicit target wins; no auto discovery.
		if (Explicit->GetSkinnedAsset() && Explicit->GetMeshDeformerInstance())
		{
			OutTargets.Add(Explicit);
		}
		return;
	}

	UWorld* World = GetWorld();
	if (!World) { return; }

	const bool bNeedRefresh =
		CachedAutoTargets.Num() == 0 ||
		TimeSinceTargetRefresh >= TargetAutoRefreshSeconds;
	if (bNeedRefresh)
	{
		CachedAutoTargets.Reset();
		TimeSinceTargetRefresh = 0.0f;

		// アクターツリーを再帰的に走査することで、ChildActor 配下に置かれた SkinnedMesh も
		// MilkyBody ターゲット候補として拾う。直下成分しか見ていなかった旧コードでは
		// 子アクター構成のキャラクターが対象から漏れていた。
		TSet<AActor*> Visited;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) { continue; }

			TArray<USkinnedMeshComponent*> AllComps;
			GatherSkinnedInActorTree(Actor, AllComps, Visited);
			for (USkinnedMeshComponent* Comp : AllComps)
			{
				if (!Comp || Comp == InSource) { continue; }
				if (!Comp->GetSkinnedAsset()) { continue; }
				// Must have a MilkyBody deformer instance to be a candidate.
				if (!Cast<UMilkyBodyDeformerInstance>(Comp->GetMeshDeformerInstance()))
				{
					continue;
				}
				CachedAutoTargets.AddUnique(Comp);
			}
		}
	}

	OutTargets.Reserve(CachedAutoTargets.Num());
	for (const TWeakObjectPtr<USkinnedMeshComponent>& W : CachedAutoTargets)
	{
		if (USkinnedMeshComponent* C = W.Get())
		{
			if (C != InSource && C->GetSkinnedAsset() && C->GetMeshDeformerInstance())
			{
				OutTargets.Add(C);
			}
		}
	}
}

void UBoneSpherePuniComponent::GatherSpheres(
	const USkeletalMeshComponent* InSource,
	const TArray<FBoneColliderSphere>& Additional,
	TArray<FBoneColliderSphere>& Out)
{
	Out.Reset();
	if (InSource)
	{
		if (USkeletalMesh* SK = Cast<USkeletalMesh>(InSource->GetSkinnedAsset()))
		{
			if (UAssetUserData* UD = SK->GetAssetUserDataOfClass(UBoneColliderAssetUserData::StaticClass()))
			{
				if (UBoneColliderAssetUserData* BC = Cast<UBoneColliderAssetUserData>(UD))
				{
					Out.Append(BC->Spheres);
				}
			}
		}
	}
	Out.Append(Additional);
}

void UBoneSpherePuniComponent::StopDriving()
{
	for (const TWeakObjectPtr<USkinnedMeshComponent>& W : PrevDrivenTargets)
	{
		if (USkinnedMeshComponent* T = W.Get())
		{
			if (UMilkyBodyDeformerInstance* Inst = Cast<UMilkyBodyDeformerInstance>(T->GetMeshDeformerInstance()))
			{
				Inst->EndExternalMultiHold();
			}
		}
	}
	PrevDrivenTargets.Reset();
}

namespace BoneSpherePuniLocal
{
	struct FResolvedSphere
	{
		FVector WorldCenter = FVector::ZeroVector;
		float Radius = 0.0f;
	};

	struct FPerTargetHits
	{
		TArray<FVector> Locations;
		TArray<float>   Radii;
		TArray<FVector> Impulses;
	};

	// Compute closest-point on a target's CPU-skinned mesh for each finger sphere.
	// For every sphere whose closest distance ≤ Radius, write a hit into OutHits.
	// Returns number of hits.
	static int32 ComputeTargetHits(
		USkinnedMeshComponent* Target,
		const TArray<FResolvedSphere>& Spheres,
		const TArray<bool>& bSphereAlreadyClaimed,
		float ImpulseDepth,
		bool bDebugDraw,
		FPerTargetHits& OutHits,
		TArray<float>& OutBestDistSqrPerSphere,
		TArray<int32>& OutBestSphereIdx,
		UWorld* DebugWorld)
	{
		OutHits.Locations.Reset();
		OutHits.Radii.Reset();
		OutHits.Impulses.Reset();

		FSkeletalMeshRenderData* RenderData = Target->GetSkeletalMeshRenderData();
		const int32 LODIndex = Target->MeshObject ? Target->MeshObject->GetLOD() : Target->GetPredictedLODLevel();
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) { return 0; }
		FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
		const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
		const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
		if (!SkinWeightBuffer || !IndexBuffer || IndexBuffer->Num() == 0) { return 0; }

		// Cheap broadphase: skip the target entirely if its world bounds don't overlap
		// any unclaimed sphere.
		const FBoxSphereBounds Bounds = Target->Bounds;
		const FVector BoundsMin = Bounds.Origin - Bounds.BoxExtent;
		const FVector BoundsMax = Bounds.Origin + Bounds.BoxExtent;
		bool bAnySphereOverlapsBounds = false;
		for (int32 SphereIdx = 0; SphereIdx < Spheres.Num(); ++SphereIdx)
		{
			if (bSphereAlreadyClaimed[SphereIdx]) { continue; }
			const FResolvedSphere& S = Spheres[SphereIdx];
			if (S.WorldCenter.X + S.Radius < BoundsMin.X || S.WorldCenter.X - S.Radius > BoundsMax.X) { continue; }
			if (S.WorldCenter.Y + S.Radius < BoundsMin.Y || S.WorldCenter.Y - S.Radius > BoundsMax.Y) { continue; }
			if (S.WorldCenter.Z + S.Radius < BoundsMin.Z || S.WorldCenter.Z - S.Radius > BoundsMax.Z) { continue; }
			bAnySphereOverlapsBounds = true;
			break;
		}
		if (!bAnySphereOverlapsBounds) { return 0; }

		TArray<FMatrix44f> RefToLocals;
		Target->CacheRefToLocalMatrices(RefToLocals);
		TArray<FVector3f> Positions;
		USkinnedMeshComponent::ComputeSkinnedPositions(
			Target, Positions, RefToLocals, LODData, *SkinWeightBuffer);
		if (Positions.Num() == 0) { return 0; }

		const FTransform TargetXform = Target->GetComponentTransform();
		const int32 NumIndices = IndexBuffer->Num();

		struct FHit
		{
			bool bHit = false;
			FVector LocalPoint = FVector::ZeroVector;
			FVector LocalNormal = FVector::ZeroVector;
			float DistSqr = TNumericLimits<float>::Max();
		};
		TArray<FHit> SphereHits;
		SphereHits.SetNum(Spheres.Num());
		// Cache per-sphere local center and squared radius for the inner loop.
		TArray<FVector> LocalCenters;
		TArray<float>   RadSqr;
		LocalCenters.SetNum(Spheres.Num());
		RadSqr.SetNum(Spheres.Num());
		for (int32 i = 0; i < Spheres.Num(); ++i)
		{
			LocalCenters[i] = TargetXform.InverseTransformPosition(Spheres[i].WorldCenter);
			RadSqr[i] = Spheres[i].Radius * Spheres[i].Radius;
			SphereHits[i].DistSqr = RadSqr[i];
		}

		for (int32 i = 0; i + 2 < NumIndices; i += 3)
		{
			const uint32 I0 = IndexBuffer->Get(i);
			const uint32 I1 = IndexBuffer->Get(i + 1);
			const uint32 I2 = IndexBuffer->Get(i + 2);
			if (!Positions.IsValidIndex(I0) || !Positions.IsValidIndex(I1) || !Positions.IsValidIndex(I2)) { continue; }
			const FVector V0((FVector)Positions[I0]);
			const FVector V1((FVector)Positions[I1]);
			const FVector V2((FVector)Positions[I2]);
			const FVector AABBMin(FMath::Min3(V0.X, V1.X, V2.X), FMath::Min3(V0.Y, V1.Y, V2.Y), FMath::Min3(V0.Z, V1.Z, V2.Z));
			const FVector AABBMax(FMath::Max3(V0.X, V1.X, V2.X), FMath::Max3(V0.Y, V1.Y, V2.Y), FMath::Max3(V0.Z, V1.Z, V2.Z));
			FVector TriNormalCache = FVector::ZeroVector;
			bool bTriNormalComputed = false;

			for (int32 SphereIdx = 0; SphereIdx < Spheres.Num(); ++SphereIdx)
			{
				if (bSphereAlreadyClaimed[SphereIdx]) { continue; }
				const FResolvedSphere& S = Spheres[SphereIdx];
				const FVector& LC = LocalCenters[SphereIdx];
				if (LC.X + S.Radius < AABBMin.X || LC.X - S.Radius > AABBMax.X) { continue; }
				if (LC.Y + S.Radius < AABBMin.Y || LC.Y - S.Radius > AABBMax.Y) { continue; }
				if (LC.Z + S.Radius < AABBMin.Z || LC.Z - S.Radius > AABBMax.Z) { continue; }
				const FVector Closest = FMath::ClosestPointOnTriangleToPoint(LC, V0, V1, V2);
				const float DistSqr = FVector::DistSquared(LC, Closest);
				if (DistSqr < SphereHits[SphereIdx].DistSqr)
				{
					if (!bTriNormalComputed)
					{
						TriNormalCache = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();
						bTriNormalComputed = true;
					}
					SphereHits[SphereIdx].bHit = true;
					SphereHits[SphereIdx].DistSqr = DistSqr;
					SphereHits[SphereIdx].LocalPoint = Closest;
					SphereHits[SphereIdx].LocalNormal = TriNormalCache;
				}
			}
		}

		int32 HitCount = 0;
		OutBestDistSqrPerSphere.SetNumUninitialized(Spheres.Num());
		OutBestSphereIdx.Reset();
		for (int32 SphereIdx = 0; SphereIdx < Spheres.Num(); ++SphereIdx)
		{
			OutBestDistSqrPerSphere[SphereIdx] = SphereHits[SphereIdx].bHit ? SphereHits[SphereIdx].DistSqr : TNumericLimits<float>::Max();
			if (!SphereHits[SphereIdx].bHit) { continue; }

			const FResolvedSphere& S = Spheres[SphereIdx];
			const FVector WorldPoint = TargetXform.TransformPosition(SphereHits[SphereIdx].LocalPoint);
			const FVector WorldNormal = TargetXform.GetRotation().RotateVector(SphereHits[SphereIdx].LocalNormal).GetSafeNormal();
			const FVector WorldImpulse = -WorldNormal * ImpulseDepth;

			OutHits.Locations.Add(WorldPoint);
			OutHits.Radii.Add(S.Radius);
			OutHits.Impulses.Add(WorldImpulse);
			OutBestSphereIdx.Add(SphereIdx);
			++HitCount;

			if (DebugWorld && bDebugDraw)
			{
				DrawDebugSphere(DebugWorld, WorldPoint, S.Radius, 12, FColor::Green, false, -1.0f, 0, 0.5f);
				DrawDebugLine(DebugWorld, WorldPoint, WorldPoint + WorldImpulse, FColor::Cyan, false, -1.0f, 0, 0.5f);
			}
		}
		return HitCount;
	}
} // namespace BoneSpherePuniLocal

void UBoneSpherePuniComponent::ApplyNow()
{
	using namespace BoneSpherePuniLocal;

	USkeletalMeshComponent* Source = ResolveSource();
	if (!Source || !Source->GetSkinnedAsset())
	{
		if (!bWarnedSourceNotFound)
		{
			bWarnedSourceNotFound = true;
			UE_LOG(LogTemp, Warning,
				TEXT("[BoneSpherePuni] '%s': SourceMesh を解決できませんでした。"
				     "SourceMesh プロパティに手等の SkeletalMeshComponent を割り当てるか、"
				     "アクター階層内に USkeletalMeshComponent を配置してください。"),
				*GetReadableName());
		}
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}
	bWarnedSourceNotFound = false;

	TArray<FBoneColliderSphere> SphereSpecs;
	GatherSpheres(Source, AdditionalSpheres, SphereSpecs);
	if (SphereSpecs.Num() == 0)
	{
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}

	// Resolve world-space sphere centers for enabled, valid bones.
	TArray<FResolvedSphere> Resolved;
	Resolved.Reserve(SphereSpecs.Num());
	for (const FBoneColliderSphere& S : SphereSpecs)
	{
		if (!S.bEnabled || S.BoneName.IsNone() || S.Radius <= 0.0f) { continue; }
		const int32 BoneIdx = Source->GetBoneIndex(S.BoneName);
		if (BoneIdx == INDEX_NONE) { continue; }
		const FTransform BoneXform = Source->GetSocketTransform(S.BoneName, RTS_World);
		FResolvedSphere R;
		R.WorldCenter = BoneXform.TransformPosition(S.LocalOffset);
		R.Radius = S.Radius;
		Resolved.Add(R);
	}

	if (Resolved.Num() == 0)
	{
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}

	UWorld* DebugWorld = bDebugDraw ? GetWorld() : nullptr;

	// Always visualize the configured spheres at their bone positions when debug
	// drawing is on, so you can verify bone names / radii / local offsets even
	// before any surface contact happens. Hit/miss is layered on top below.
	if (DebugWorld)
	{
		for (const FResolvedSphere& Sphere : Resolved)
		{
			DrawDebugSphere(DebugWorld, Sphere.WorldCenter, Sphere.Radius, 12, FColor::Magenta, false, -1.0f, 0, 0.4f);
			DrawDebugPoint(DebugWorld, Sphere.WorldCenter, 4.0f, FColor::Magenta, false, -1.0f, 0);
		}
	}

	// Non-snap path (cheap, for users who don't care about surface accuracy).
	// Just drive the explicit TargetBody (or first auto candidate) with raw bone positions.
	if (!bSurfaceSnap)
	{
		TArray<USkinnedMeshComponent*> NonSnapTargets;
		GatherTargetCandidates(Source, NonSnapTargets);
		if (NonSnapTargets.Num() == 0)
		{
			if (!bWarnedTargetNotFound)
			{
				bWarnedTargetNotFound = true;
				UE_LOG(LogTemp, Warning,
					TEXT("[BoneSpherePuni] '%s': MilkyBody Deformer 付きのターゲット SkinnedMesh が見つかりません (non-snap)。"),
					*GetReadableName());
			}
			if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
			return;
		}
		bWarnedTargetNotFound = false;

		USkinnedMeshComponent* T = NonSnapTargets[0];
		UMilkyBodyDeformerInstance* Inst = Cast<UMilkyBodyDeformerInstance>(T->GetMeshDeformerInstance());
		if (!Inst)
		{
			if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
			return;
		}

		const FVector TargetOrigin = T->GetComponentLocation();
		TArray<FVector> Locations;
		TArray<float>   Radii;
		TArray<FVector> Impulses;
		Locations.Reserve(Resolved.Num());
		Radii.Reserve(Resolved.Num());
		Impulses.Reserve(Resolved.Num());
		for (const FResolvedSphere& Sphere : Resolved)
		{
			const FVector ToTarget = (TargetOrigin - Sphere.WorldCenter).GetSafeNormal();
			Locations.Add(Sphere.WorldCenter);
			Radii.Add(Sphere.Radius);
			Impulses.Add(ToTarget * ImpulseDepth);
			if (DebugWorld) { DrawDebugSphere(DebugWorld, Sphere.WorldCenter, Sphere.Radius, 12, FColor::Yellow, false, -1.0f, 0, 0.5f); }
		}
		Inst->BeginExternalMultiHold(Locations, Radii, Impulses);

		TSet<TWeakObjectPtr<USkinnedMeshComponent>> NewDriven;
		NewDriven.Add(T);
		for (const TWeakObjectPtr<USkinnedMeshComponent>& W : PrevDrivenTargets)
		{
			if (!NewDriven.Contains(W))
			{
				if (USkinnedMeshComponent* Old = W.Get())
				{
					if (auto* OldInst = Cast<UMilkyBodyDeformerInstance>(Old->GetMeshDeformerInstance()))
					{
						OldInst->EndExternalMultiHold();
					}
				}
			}
		}
		PrevDrivenTargets = MoveTemp(NewDriven);
		return;
	}

	// Surface-snap path: iterate targets, claim spheres greedily by closest distance
	// so a single sphere never deforms multiple bodies in the same frame.
	TArray<USkinnedMeshComponent*> Targets;
	GatherTargetCandidates(Source, Targets);
	if (Targets.Num() == 0)
	{
		if (!bWarnedTargetNotFound)
		{
			bWarnedTargetNotFound = true;
			UE_LOG(LogTemp, Warning,
				TEXT("[BoneSpherePuni] '%s': MilkyBody Deformer 付きのターゲット SkinnedMesh が見つかりません。"
				     "TargetBody プロパティを設定するか、ワールド上に MilkyBody Deformer をアサインした"
				     " SkinnedMesh を配置してください。"),
				*GetReadableName());
		}
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}
	bWarnedTargetNotFound = false;

	TArray<bool> SphereClaimed;
	SphereClaimed.Init(false, Resolved.Num());

	// First pass: per target, find each sphere's closest distance and which
	// surface point that distance refers to. We DON'T claim spheres yet — a
	// sphere may be closer to target B than to target A even though A intersected
	// first. So we first compute everyone's per-target distances, then assign
	// each sphere to its closest target, then build the per-target hold array.
	struct FSphereTargetCandidate
	{
		int32 TargetIdx = INDEX_NONE;
		float DistSqr = TNumericLimits<float>::Max();
		FVector WorldPoint = FVector::ZeroVector;
		FVector WorldImpulse = FVector::ZeroVector;
	};
	TArray<FSphereTargetCandidate> BestPerSphere;
	BestPerSphere.SetNum(Resolved.Num());

	for (int32 TIdx = 0; TIdx < Targets.Num(); ++TIdx)
	{
		USkinnedMeshComponent* T = Targets[TIdx];
		if (!T) { continue; }

		FSkeletalMeshRenderData* RenderData = T->GetSkeletalMeshRenderData();
		const int32 LODIndex = T->MeshObject ? T->MeshObject->GetLOD() : T->GetPredictedLODLevel();
		if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex)) { continue; }
		FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
		const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
		const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
		if (!SkinWeightBuffer || !IndexBuffer || IndexBuffer->Num() == 0) { continue; }

		// Bounds prefilter: skip target entirely if no sphere overlaps.
		const FBoxSphereBounds Bounds = T->Bounds;
		const FVector BMin = Bounds.Origin - Bounds.BoxExtent;
		const FVector BMax = Bounds.Origin + Bounds.BoxExtent;
		bool bAnyOverlap = false;
		for (const FResolvedSphere& S : Resolved)
		{
			if (S.WorldCenter.X + S.Radius < BMin.X || S.WorldCenter.X - S.Radius > BMax.X) continue;
			if (S.WorldCenter.Y + S.Radius < BMin.Y || S.WorldCenter.Y - S.Radius > BMax.Y) continue;
			if (S.WorldCenter.Z + S.Radius < BMin.Z || S.WorldCenter.Z - S.Radius > BMax.Z) continue;
			bAnyOverlap = true;
			break;
		}
		if (!bAnyOverlap) { continue; }

		TArray<FMatrix44f> RefToLocals;
		T->CacheRefToLocalMatrices(RefToLocals);
		TArray<FVector3f> Positions;
		USkinnedMeshComponent::ComputeSkinnedPositions(T, Positions, RefToLocals, LODData, *SkinWeightBuffer);
		if (Positions.Num() == 0) { continue; }

		const FTransform TargetXform = T->GetComponentTransform();
		const int32 NumIndices = IndexBuffer->Num();
		const int32 NumSpheres = Resolved.Num();

		// Local-space sphere centers and threshold per sphere (best so far for THIS target).
		TArray<FVector> LocalCenters;
		LocalCenters.SetNum(NumSpheres);
		TArray<float> ThisTargetBestDistSqr;
		ThisTargetBestDistSqr.Init(TNumericLimits<float>::Max(), NumSpheres);
		TArray<FVector> ThisTargetBestPoint;
		ThisTargetBestPoint.SetNum(NumSpheres);
		TArray<FVector> ThisTargetBestNormal;
		ThisTargetBestNormal.SetNum(NumSpheres);
		TArray<bool> ThisTargetSphereHit;
		ThisTargetSphereHit.Init(false, NumSpheres);
		for (int32 SI = 0; SI < NumSpheres; ++SI)
		{
			LocalCenters[SI] = TargetXform.InverseTransformPosition(Resolved[SI].WorldCenter);
			ThisTargetBestDistSqr[SI] = Resolved[SI].Radius * Resolved[SI].Radius;
		}

		for (int32 i = 0; i + 2 < NumIndices; i += 3)
		{
			const uint32 I0 = IndexBuffer->Get(i);
			const uint32 I1 = IndexBuffer->Get(i + 1);
			const uint32 I2 = IndexBuffer->Get(i + 2);
			if (!Positions.IsValidIndex(I0) || !Positions.IsValidIndex(I1) || !Positions.IsValidIndex(I2)) { continue; }
			const FVector V0((FVector)Positions[I0]);
			const FVector V1((FVector)Positions[I1]);
			const FVector V2((FVector)Positions[I2]);
			const FVector AABBMin(FMath::Min3(V0.X, V1.X, V2.X), FMath::Min3(V0.Y, V1.Y, V2.Y), FMath::Min3(V0.Z, V1.Z, V2.Z));
			const FVector AABBMax(FMath::Max3(V0.X, V1.X, V2.X), FMath::Max3(V0.Y, V1.Y, V2.Y), FMath::Max3(V0.Z, V1.Z, V2.Z));
			FVector TriNormal = FVector::ZeroVector;
			bool bNormalCached = false;

			for (int32 SI = 0; SI < NumSpheres; ++SI)
			{
				const float R = Resolved[SI].Radius;
				const FVector& LC = LocalCenters[SI];
				if (LC.X + R < AABBMin.X || LC.X - R > AABBMax.X) continue;
				if (LC.Y + R < AABBMin.Y || LC.Y - R > AABBMax.Y) continue;
				if (LC.Z + R < AABBMin.Z || LC.Z - R > AABBMax.Z) continue;
				const FVector Closest = FMath::ClosestPointOnTriangleToPoint(LC, V0, V1, V2);
				const float DistSqr = FVector::DistSquared(LC, Closest);
				if (DistSqr < ThisTargetBestDistSqr[SI])
				{
					if (!bNormalCached)
					{
						TriNormal = FVector::CrossProduct(V1 - V0, V2 - V0).GetSafeNormal();
						bNormalCached = true;
					}
					ThisTargetBestDistSqr[SI] = DistSqr;
					ThisTargetBestPoint[SI] = Closest;
					ThisTargetBestNormal[SI] = TriNormal;
					ThisTargetSphereHit[SI] = true;
				}
			}
		}

		// Promote to global best-per-sphere if this target is closer.
		for (int32 SI = 0; SI < NumSpheres; ++SI)
		{
			if (!ThisTargetSphereHit[SI]) { continue; }
			if (ThisTargetBestDistSqr[SI] < BestPerSphere[SI].DistSqr)
			{
				BestPerSphere[SI].TargetIdx = TIdx;
				BestPerSphere[SI].DistSqr = ThisTargetBestDistSqr[SI];
				BestPerSphere[SI].WorldPoint = TargetXform.TransformPosition(ThisTargetBestPoint[SI]);
				const FVector WN = TargetXform.GetRotation().RotateVector(ThisTargetBestNormal[SI]).GetSafeNormal();
				BestPerSphere[SI].WorldImpulse = -WN * ImpulseDepth;
			}
		}
	}

	// Bucket assigned spheres by target.
	TMap<int32, FPerTargetHits> HitsByTarget;
	for (int32 SI = 0; SI < Resolved.Num(); ++SI)
	{
		const FSphereTargetCandidate& B = BestPerSphere[SI];
		if (B.TargetIdx == INDEX_NONE) { continue; }
		FPerTargetHits& H = HitsByTarget.FindOrAdd(B.TargetIdx);
		H.Locations.Add(B.WorldPoint);
		H.Radii.Add(Resolved[SI].Radius);
		H.Impulses.Add(B.WorldImpulse);

		if (DebugWorld)
		{
			DrawDebugSphere(DebugWorld, B.WorldPoint, Resolved[SI].Radius, 12, FColor::Green, false, -1.0f, 0, 0.5f);
			DrawDebugLine(DebugWorld, B.WorldPoint, B.WorldPoint + B.WorldImpulse, FColor::Cyan, false, -1.0f, 0, 0.5f);
		}
	}

	if (DebugWorld)
	{
		// Misses: faint grey sphere at bone position.
		for (int32 SI = 0; SI < Resolved.Num(); ++SI)
		{
			if (BestPerSphere[SI].TargetIdx == INDEX_NONE)
			{
				DrawDebugSphere(DebugWorld, Resolved[SI].WorldCenter, Resolved[SI].Radius, 12, FColor(80, 80, 80), false, -1.0f, 0, 0.3f);
			}
		}
	}

	// Apply per-target multi-hold and track which targets we drove this frame.
	TSet<TWeakObjectPtr<USkinnedMeshComponent>> NewDriven;
	for (TPair<int32, FPerTargetHits>& Pair : HitsByTarget)
	{
		USkinnedMeshComponent* T = Targets[Pair.Key];
		if (!T) { continue; }
		UMilkyBodyDeformerInstance* Inst = Cast<UMilkyBodyDeformerInstance>(T->GetMeshDeformerInstance());
		if (!Inst) { continue; }
		Inst->BeginExternalMultiHold(Pair.Value.Locations, Pair.Value.Radii, Pair.Value.Impulses);
		NewDriven.Add(T);
	}

	// End on targets that had holds last frame but no fingers this frame.
	for (const TWeakObjectPtr<USkinnedMeshComponent>& W : PrevDrivenTargets)
	{
		if (!NewDriven.Contains(W))
		{
			if (USkinnedMeshComponent* Old = W.Get())
			{
				if (auto* OldInst = Cast<UMilkyBodyDeformerInstance>(Old->GetMeshDeformerInstance()))
				{
					OldInst->EndExternalMultiHold();
				}
			}
		}
	}
	PrevDrivenTargets = MoveTemp(NewDriven);
}
