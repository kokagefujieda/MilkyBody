#include "BoneSpherePuniComponent.h"

#include "MilkyBodyCPUSkinning.h"
#include "MilkyBodyDeformerInstance.h"
#include "MilkyBodyPrivate.h"

#include "Components/ChildActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "RawIndexBuffer.h"
#include "DrawDebugHelpers.h"

DECLARE_CYCLE_STAT(TEXT("BoneSpherePuni ApplyNow"), STAT_MilkyBody_BoneSpherePuni, STATGROUP_MilkyBody);

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

		// ワールド内の MilkyBody 付き SkinnedMesh を列挙する (ChildActor 配下も含む)。
		TArray<USkinnedMeshComponent*> AllComps;
		MilkyBody::GatherSkinnedMeshesInWorld(World, /*bRequireMilkyBody=*/true, AllComps);
		for (USkinnedMeshComponent* Comp : AllComps)
		{
			if (Comp != InSource)
			{
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
				Inst->ReleaseExternalContacts(this);
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
		int32 ContactId = INDEX_NONE; // index into the gathered sphere specs; stable per finger
	};

	struct FPerTargetHits
	{
		TArray<int32>   ContactIds;
		TArray<FVector> Locations;
		TArray<float>   Radii;
		TArray<FVector> Impulses;

		void Add(int32 ContactId, const FVector& Location, float Radius, const FVector& Impulse)
		{
			ContactIds.Add(ContactId);
			Locations.Add(Location);
			Radii.Add(Radius);
			Impulses.Add(Impulse);
		}
	};

	static bool SphereOverlapsBox(const FVector& Center, float Radius, const FVector& BoxMin, const FVector& BoxMax)
	{
		return !(Center.X + Radius < BoxMin.X || Center.X - Radius > BoxMax.X
			  || Center.Y + Radius < BoxMin.Y || Center.Y - Radius > BoxMax.Y
			  || Center.Z + Radius < BoxMin.Z || Center.Z - Radius > BoxMax.Z);
	}
} // namespace BoneSpherePuniLocal

void UBoneSpherePuniComponent::ApplyNow()
{
	SCOPE_CYCLE_COUNTER(STAT_MilkyBody_BoneSpherePuni);
	using namespace BoneSpherePuniLocal;

	USkeletalMeshComponent* Source = ResolveSource();
	if (!Source || !Source->GetSkinnedAsset())
	{
		if (!bWarnedSourceNotFound)
		{
			bWarnedSourceNotFound = true;
			UE_LOG(LogMilkyBody, Warning,
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

	// Resolve world-space sphere centers for enabled, valid bones. The spec
	// index doubles as the contact id so each finger keeps its own spring.
	TArray<FResolvedSphere> Resolved;
	Resolved.Reserve(SphereSpecs.Num());
	for (int32 SpecIdx = 0; SpecIdx < SphereSpecs.Num(); ++SpecIdx)
	{
		const FBoneColliderSphere& S = SphereSpecs[SpecIdx];
		if (!S.bEnabled || S.BoneName.IsNone() || S.Radius <= 0.0f) { continue; }
		const int32 BoneIdx = Source->GetBoneIndex(S.BoneName);
		if (BoneIdx == INDEX_NONE) { continue; }
		const FTransform BoneXform = Source->GetSocketTransform(S.BoneName, RTS_World);
		FResolvedSphere R;
		R.WorldCenter = BoneXform.TransformPosition(S.LocalOffset);
		R.Radius = S.Radius;
		R.ContactId = SpecIdx;
		Resolved.Add(R);
	}

	if (Resolved.Num() == 0)
	{
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}

	const bool bDraw = bDebugDraw || CVarMilkyBodyDebugDraw.GetValueOnGameThread() != 0;
	UWorld* DebugWorld = bDraw ? GetWorld() : nullptr;

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

	TArray<USkinnedMeshComponent*> Targets;
	GatherTargetCandidates(Source, Targets);
	if (Targets.Num() == 0)
	{
		if (!bWarnedTargetNotFound)
		{
			bWarnedTargetNotFound = true;
			UE_LOG(LogMilkyBody, Warning,
				TEXT("[BoneSpherePuni] '%s': MilkyBody Deformer 付きのターゲット SkinnedMesh が見つかりません。"
				     "TargetBody プロパティを設定するか、ワールド上に MilkyBody Deformer をアサインした"
				     " SkinnedMesh を配置してください。"),
				*GetReadableName());
		}
		if (PrevDrivenTargets.Num() > 0) { StopDriving(); }
		return;
	}
	bWarnedTargetNotFound = false;

	TMap<int32, FPerTargetHits> HitsByTarget;

	if (!bSurfaceSnap)
	{
		// Non-snap path (cheap, for users who don't care about surface accuracy):
		// drive the explicit TargetBody (or first auto candidate) with raw bone
		// positions, pushing toward the target origin with a constant depth.
		USkinnedMeshComponent* T = Targets[0];
		const FVector TargetOrigin = T->GetComponentLocation();
		FPerTargetHits& Hits = HitsByTarget.FindOrAdd(0);
		for (const FResolvedSphere& Sphere : Resolved)
		{
			const FVector ToTarget = (TargetOrigin - Sphere.WorldCenter).GetSafeNormal();
			Hits.Add(Sphere.ContactId, Sphere.WorldCenter, Sphere.Radius, ToTarget * ImpulseDepth);
			if (DebugWorld) { DrawDebugSphere(DebugWorld, Sphere.WorldCenter, Sphere.Radius, 12, FColor::Yellow, false, -1.0f, 0, 0.5f); }
		}
	}
	else
	{
		// Surface-snap path. For every sphere, find the closest point on each
		// target's CPU-skinned surface, then assign the sphere to its closest
		// target so a single finger never deforms two bodies in the same frame.
		struct FSphereTargetCandidate
		{
			int32 TargetIdx = INDEX_NONE;
			float DistSqr = TNumericLimits<float>::Max(); // world space
			FVector WorldPoint = FVector::ZeroVector;
			FVector WorldImpulse = FVector::ZeroVector;
		};
		TArray<FSphereTargetCandidate> BestPerSphere;
		BestPerSphere.SetNum(Resolved.Num());
		const int32 NumSpheres = Resolved.Num();

		for (int32 TIdx = 0; TIdx < Targets.Num(); ++TIdx)
		{
			USkinnedMeshComponent* T = Targets[TIdx];
			if (!T) { continue; }

			// Bounds prefilter: skip the target entirely if no sphere overlaps.
			const FBoxSphereBounds Bounds = T->Bounds;
			const FVector BMin = Bounds.Origin - Bounds.BoxExtent;
			const FVector BMax = Bounds.Origin + Bounds.BoxExtent;
			bool bAnyOverlap = false;
			for (const FResolvedSphere& S : Resolved)
			{
				if (SphereOverlapsBox(S.WorldCenter, S.Radius, BMin, BMax)) { bAnyOverlap = true; break; }
			}
			if (!bAnyOverlap) { continue; }

			MilkyBody::FCPUSkinnedMesh Mesh;
			if (!MilkyBody::BuildCPUSkinnedMesh(T, Mesh)) { continue; }

			const FTransform& TargetXform = Mesh.ComponentToWorld;
			const float TargetScale = FMath::Max((float)TargetXform.GetMaximumAxisScale(), KINDA_SMALL_NUMBER);
			const int32 NumIndices = Mesh.NumIndices();

			// Component-space sphere centers / radii, best hit so far for THIS target.
			TArray<FVector> LocalCenters;
			TArray<float> LocalRadii;
			TArray<float> ThisBestDistSqr;
			TArray<FVector> ThisBestPoint;
			TArray<FVector> ThisBestNormal;
			TArray<bool> ThisHit;
			LocalCenters.SetNum(NumSpheres);
			LocalRadii.SetNum(NumSpheres);
			ThisBestDistSqr.SetNum(NumSpheres);
			ThisBestPoint.SetNum(NumSpheres);
			ThisBestNormal.SetNum(NumSpheres);
			ThisHit.Init(false, NumSpheres);
			for (int32 SI = 0; SI < NumSpheres; ++SI)
			{
				LocalCenters[SI] = TargetXform.InverseTransformPosition(Resolved[SI].WorldCenter);
				LocalRadii[SI] = Resolved[SI].Radius / TargetScale;
				ThisBestDistSqr[SI] = LocalRadii[SI] * LocalRadii[SI];
			}

			for (int32 i = 0; i + 2 < NumIndices; i += 3)
			{
				const uint32 I0 = Mesh.GetIndex(i);
				const uint32 I1 = Mesh.GetIndex(i + 1);
				const uint32 I2 = Mesh.GetIndex(i + 2);
				if (!Mesh.Positions.IsValidIndex(I0) || !Mesh.Positions.IsValidIndex(I1) || !Mesh.Positions.IsValidIndex(I2)) { continue; }
				const FVector V0(Mesh.Positions[I0]);
				const FVector V1(Mesh.Positions[I1]);
				const FVector V2(Mesh.Positions[I2]);
				const FVector AABBMin(FMath::Min3(V0.X, V1.X, V2.X), FMath::Min3(V0.Y, V1.Y, V2.Y), FMath::Min3(V0.Z, V1.Z, V2.Z));
				const FVector AABBMax(FMath::Max3(V0.X, V1.X, V2.X), FMath::Max3(V0.Y, V1.Y, V2.Y), FMath::Max3(V0.Z, V1.Z, V2.Z));
				FVector TriNormal = FVector::ZeroVector;
				bool bNormalCached = false;

				for (int32 SI = 0; SI < NumSpheres; ++SI)
				{
					if (!SphereOverlapsBox(LocalCenters[SI], LocalRadii[SI], AABBMin, AABBMax)) { continue; }
					const FVector Closest = FMath::ClosestPointOnTriangleToPoint(LocalCenters[SI], V0, V1, V2);
					const float DistSqr = FVector::DistSquared(LocalCenters[SI], Closest);
					if (DistSqr < ThisBestDistSqr[SI])
					{
						if (!bNormalCached)
						{
							// Outward-facing (winding checked against the vertex normals).
							TriNormal = (FVector::CrossProduct(V1 - V0, V2 - V0) * Mesh.WindingSign).GetSafeNormal();
							bNormalCached = true;
						}
						ThisBestDistSqr[SI] = DistSqr;
						ThisBestPoint[SI] = Closest;
						ThisBestNormal[SI] = TriNormal;
						ThisHit[SI] = true;
					}
				}
			}

			// Promote to the global best-per-sphere if this target is closer.
			for (int32 SI = 0; SI < NumSpheres; ++SI)
			{
				if (!ThisHit[SI]) { continue; }
				const float WorldDistSqr = ThisBestDistSqr[SI] * TargetScale * TargetScale;
				if (WorldDistSqr >= BestPerSphere[SI].DistSqr) { continue; }

				const FVector WorldNormal = TargetXform.TransformVectorNoScale(ThisBestNormal[SI]).GetSafeNormal();

				float Depth = ImpulseDepth;
				if (bDepthFromPenetration)
				{
					// Penetration of the sphere into the surface (world cm). The
					// center may already be behind the surface on a firm press.
					const float Dist = FMath::Sqrt(WorldDistSqr);
					const FVector ToCenterLocal = LocalCenters[SI] - ThisBestPoint[SI];
					const bool bCenterInside = FVector::DotProduct(ToCenterLocal, ThisBestNormal[SI]) < 0.0f;
					const float Penetration = bCenterInside ? (Resolved[SI].Radius + Dist) : (Resolved[SI].Radius - Dist);
					Depth = FMath::Clamp(Penetration * PenetrationDepthScale, 0.0f, ImpulseDepth);
				}

				BestPerSphere[SI].TargetIdx = TIdx;
				BestPerSphere[SI].DistSqr = WorldDistSqr;
				BestPerSphere[SI].WorldPoint = TargetXform.TransformPosition(ThisBestPoint[SI]);
				BestPerSphere[SI].WorldImpulse = -WorldNormal * Depth;
			}
		}

		// Bucket assigned spheres by target.
		for (int32 SI = 0; SI < NumSpheres; ++SI)
		{
			const FSphereTargetCandidate& B = BestPerSphere[SI];
			if (B.TargetIdx == INDEX_NONE)
			{
				if (DebugWorld)
				{
					// Miss: faint grey sphere at the bone position.
					DrawDebugSphere(DebugWorld, Resolved[SI].WorldCenter, Resolved[SI].Radius, 12, FColor(80, 80, 80), false, -1.0f, 0, 0.3f);
				}
				continue;
			}
			HitsByTarget.FindOrAdd(B.TargetIdx).Add(Resolved[SI].ContactId, B.WorldPoint, Resolved[SI].Radius, B.WorldImpulse);

			if (DebugWorld)
			{
				DrawDebugSphere(DebugWorld, B.WorldPoint, Resolved[SI].Radius, 12, FColor::Green, false, -1.0f, 0, 0.5f);
				DrawDebugLine(DebugWorld, B.WorldPoint, B.WorldPoint + B.WorldImpulse, FColor::Cyan, false, -1.0f, 0, 0.5f);
			}
		}
	}

	// Feed each touched target. Fingers that lifted off fade out on their own
	// spring inside the deformer instance.
	TSet<TWeakObjectPtr<USkinnedMeshComponent>> NewDriven;
	for (TPair<int32, FPerTargetHits>& Pair : HitsByTarget)
	{
		USkinnedMeshComponent* T = Targets[Pair.Key];
		if (!T) { continue; }
		UMilkyBodyDeformerInstance* Inst = Cast<UMilkyBodyDeformerInstance>(T->GetMeshDeformerInstance());
		if (!Inst) { continue; }
		Inst->UpdateExternalContacts(this, Pair.Value.ContactIds, Pair.Value.Locations, Pair.Value.Radii, Pair.Value.Impulses);
		NewDriven.Add(T);
	}

	// Release targets that had contacts last frame but none this frame.
	for (const TWeakObjectPtr<USkinnedMeshComponent>& W : PrevDrivenTargets)
	{
		if (!NewDriven.Contains(W))
		{
			if (USkinnedMeshComponent* Old = W.Get())
			{
				if (auto* OldInst = Cast<UMilkyBodyDeformerInstance>(Old->GetMeshDeformerInstance()))
				{
					OldInst->ReleaseExternalContacts(this);
				}
			}
		}
	}
	PrevDrivenTargets = MoveTemp(NewDriven);
}
