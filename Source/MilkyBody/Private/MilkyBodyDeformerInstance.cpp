#include "MilkyBodyDeformerInstance.h"
#include "MilkyBodyDeformer.h"
#include "MilkyBodyShaders.h"

#include "Components/SkinnedMeshComponent.h"
#include "GlobalRenderResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIBufferInitializer.h"
#include "RHICommandList.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "DrawDebugHelpers.h"

#if WITH_EDITORONLY_DATA
#include "Animation/MeshDeformerGeometryReadback.h"
#endif

void UMilkyBodyDeformerInstance::AllocateResources()
{
	// Base class is PURE_VIRTUAL — do NOT call Super.
	// Actual GPU buffer allocation happens lazily on the render thread.
}

void UMilkyBodyDeformerInstance::ReleaseResources()
{
	FlushRenderingCommands();
}

void UMilkyBodyDeformerInstance::ApplyPushImpulse(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius,
	float DurationSeconds)
{
	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const FVector LocalPos = Xform.InverseTransformPosition(WorldLocation);
	const FVector LocalImpulse = Xform.InverseTransformVectorNoScale(WorldImpulse);

	AnchorLocalPos = FVector3f(LocalPos);
	AnchorLocalImpulse = FVector3f(LocalImpulse);
	CurrentLocalOffset = FVector3f::ZeroVector;
	TargetLocalOffset = FVector3f::ZeroVector;
	PushRadius = FMath::Max(Radius, 0.0f);

	// Apply a sudden impulse to velocity to kick off the jiggle seamlessly without snapping
	TargetPushAmount = 0.0f;
	PushVelocity += 10.0f; // Give it a swift kick
	bPushHeld = false;
	bKneading = false;
	bMomi = false;
	bExternalMultiHold = false;
	ExternalLocalPositions.Reset();
	ExternalLocalImpulses.Reset();
	ExternalRadii.Reset();
}

void UMilkyBodyDeformerInstance::BeginHoldPush(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius)
{
	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const FVector3f NewLocalPos = FVector3f(Xform.InverseTransformPosition(WorldLocation));
	const FVector3f NewLocalImpulse = FVector3f(Xform.InverseTransformVectorNoScale(WorldImpulse));

	if (!bPushHeld)
	{
		// First frame of a fresh hold — anchor here, no offset yet.
		AnchorLocalPos = NewLocalPos;
		AnchorLocalImpulse = NewLocalImpulse;
		CurrentLocalOffset = FVector3f::ZeroVector;
		TargetLocalOffset = FVector3f::ZeroVector;
	}
	else
	{
		// Subsequent frame — anchor and impulse direction stay locked; only the
		// follow target moves. CurrentLocalOffset is smoothed toward this each
		// tick in EnqueueWork.
		TargetLocalOffset = NewLocalPos - AnchorLocalPos;
	}
	PushRadius = FMath::Max(Radius, 0.0f);
	bPushHeld = true;
	bKneading = false;
	bMomi = false;
	bExternalMultiHold = false;
	ExternalLocalPositions.Reset();
	ExternalLocalImpulses.Reset();
	ExternalRadii.Reset();
	TargetPushAmount = 1.0f;
}

void UMilkyBodyDeformerInstance::ReleaseHoldPush(float FadeOutSeconds)
{
	const bool bWasSingleHold = bPushHeld;

	bPushHeld = false;
	TargetPushAmount = 0.0f;
	TargetLocalOffset = FVector3f::ZeroVector;

	if (FadeOutSeconds <= 0.0f)
	{
		CurrentPushAmount = 0.0f;
		PushVelocity = 0.0f;
		CurrentLocalOffset = FVector3f::ZeroVector;
		bKneading = false;
		bMomi = false;
		bExternalMultiHold = false;
		ExternalLocalPositions.Reset();
		ExternalLocalImpulses.Reset();
		ExternalRadii.Reset();
	}
	else if (bWasSingleHold)
	{
		bKneading = false;
		bMomi = false;
		bExternalMultiHold = false;
		ExternalLocalPositions.Reset();
		ExternalLocalImpulses.Reset();
		ExternalRadii.Reset();
	}
}

void UMilkyBodyDeformerInstance::BeginKneading(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius,
	int32 NumPoints,
	float ScatterRadius,
	float MotionSpeed,
	float RandomSeed)
{
	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const FVector3f LocalCenter = FVector3f(Xform.InverseTransformPosition(WorldLocation));
	const FVector3f LocalImpulse = FVector3f(Xform.InverseTransformVectorNoScale(WorldImpulse));

	// Build a tangent basis perpendicular to the impulse direction so fingers
	// scatter along the touched surface, not into / out of it.
	const FVector LocalImpulseD = FVector(LocalImpulse);
	FVector N = LocalImpulseD.GetSafeNormal();
	if (N.IsNearlyZero())
	{
		N = FVector(0, 0, -1);
	}
	FVector Up = (FMath::Abs(N.Z) < 0.95f) ? FVector::UpVector : FVector::ForwardVector;
	const FVector U = FVector::CrossProduct(N, Up).GetSafeNormal();
	const FVector V = FVector::CrossProduct(N, U).GetSafeNormal();

	KneadCenterLocal = LocalCenter;
	KneadImpulseLocal = LocalImpulse;
	KneadTangentU = FVector3f(U);
	KneadTangentV = FVector3f(V);

	NumKneadingPoints = FMath::Clamp(NumPoints, 1, 4);
	KneadEachRadius = FMath::Max(Radius, 0.0f);
	KneadingScatterRadius = FMath::Max(ScatterRadius, 0.0f);
	KneadingMotionSpeed = FMath::Max(MotionSpeed, 0.0f);
	KneadingSeed = RandomSeed;

	// Cancel single hold and lock kneading on with full push amount.
	bPushHeld = false;
	bExternalMultiHold = false;
	TargetLocalOffset = FVector3f::ZeroVector;
	CurrentLocalOffset = FVector3f::ZeroVector;
	bKneading = true;
	ExternalLocalPositions.Reset();
	ExternalLocalImpulses.Reset();
	ExternalRadii.Reset();
	if (FMath::Abs(TargetPushAmount - 1.0f) > KINDA_SMALL_NUMBER)
	{
		KneadingTime = 0.0f; // reset clock on a fresh begin
	}
	TargetPushAmount = 1.0f;
}

void UMilkyBodyDeformerInstance::EndKneading()
{
	// Do not clear bKneading here; we want the procedural motion to continue
	// while the PushAmount fades out to 0.
	TargetPushAmount = 0.0f;
}

void UMilkyBodyDeformerInstance::BeginMomi(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius,
	int32 NumPoints,
	float OrbitRadius,
	float OrbitSpeed,
	float SpeedVariation,
	float DepthVariation,
	float RandomSeed)
{
	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const FVector3f LocalCenter = FVector3f(Xform.InverseTransformPosition(WorldLocation));
	const FVector3f LocalImpulse = FVector3f(Xform.InverseTransformVectorNoScale(WorldImpulse));

	// Build a tangent basis perpendicular to the impulse direction so orbit
	// points travel along the touched surface, not into / out of it.
	const FVector LocalImpulseD = FVector(LocalImpulse);
	FVector N = LocalImpulseD.GetSafeNormal();
	if (N.IsNearlyZero())
	{
		N = FVector(0, 0, -1);
	}
	FVector Up = (FMath::Abs(N.Z) < 0.95f) ? FVector::UpVector : FVector::ForwardVector;
	const FVector U = FVector::CrossProduct(N, Up).GetSafeNormal();
	const FVector V = FVector::CrossProduct(N, U).GetSafeNormal();

	MomiCenterLocal = LocalCenter;
	MomiImpulseLocal = LocalImpulse;
	MomiTangentU = FVector3f(U);
	MomiTangentV = FVector3f(V);

	NumMomiPoints = FMath::Clamp(NumPoints, 1, 4);
	MomiEachRadius = FMath::Max(Radius, 0.0f);
	MomiOrbitRadius = FMath::Max(OrbitRadius, 0.0f);
	MomiOrbitSpeed = FMath::Max(OrbitSpeed, 0.0f);
	MomiSpeedVariation = FMath::Clamp(SpeedVariation, 0.0f, 1.0f);
	MomiDepthVariation = FMath::Clamp(DepthVariation, 0.0f, 1.0f);
	MomiSeed = RandomSeed;

	// Cancel single hold and kneading, lock momi on.
	bPushHeld = false;
	bKneading = false;
	bExternalMultiHold = false;
	TargetLocalOffset = FVector3f::ZeroVector;
	CurrentLocalOffset = FVector3f::ZeroVector;
	bMomi = true;
	ExternalLocalPositions.Reset();
	ExternalLocalImpulses.Reset();
	ExternalRadii.Reset();
	if (FMath::Abs(TargetPushAmount - 1.0f) > KINDA_SMALL_NUMBER)
	{
		MomiTime = 0.0f;
	}
	TargetPushAmount = 1.0f;
}

void UMilkyBodyDeformerInstance::EndMomi()
{
	// Do not clear bMomi here; we want the orbital motion to continue
	// while the PushAmount fades out to 0.
	TargetPushAmount = 0.0f;
}

void UMilkyBodyDeformerInstance::BeginExternalMultiHold(
	const TArray<FVector>& WorldLocations,
	const TArray<float>& Radii,
	const TArray<FVector>& WorldImpulses)
{
	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const int32 InN = FMath::Min3(WorldLocations.Num(), Radii.Num(), WorldImpulses.Num());
	const int32 N = FMath::Min(InN, FMilkyBodySkinCS::MaxPuniSlots);
	if (N <= 0)
	{
		// Nothing to drive — fade out.
		TargetPushAmount = 0.0f;
		return;
	}

	const FTransform& Xform = Comp->GetComponentTransform();
	ExternalLocalPositions.Reset(N);
	ExternalLocalImpulses.Reset(N);
	ExternalRadii.Reset(N);
	for (int32 i = 0; i < N; ++i)
	{
		const FVector LocalPos = Xform.InverseTransformPosition(WorldLocations[i]);
		const FVector LocalImp = Xform.InverseTransformVectorNoScale(WorldImpulses[i]);
		ExternalLocalPositions.Add(FVector3f(LocalPos));
		ExternalLocalImpulses.Add(FVector3f(LocalImp));
		ExternalRadii.Add(FMath::Max(Radii[i], 0.0f));
	}

	// Mutually exclusive with other modes.
	bPushHeld = false;
	bKneading = false;
	bMomi = false;
	TargetLocalOffset = FVector3f::ZeroVector;
	CurrentLocalOffset = FVector3f::ZeroVector;
	bExternalMultiHold = true;
	TargetPushAmount = 1.0f;
}

void UMilkyBodyDeformerInstance::EndExternalMultiHold()
{
	// Keep bExternalMultiHold = true so the slots keep dispatching while the
	// spring fades, mirroring EndKneading / EndMomi behavior.
	TargetPushAmount = 0.0f;
}

// PBD and edge constraint buffers removed

namespace MilkyBodyPrivate
{
	struct FSectionDispatchData
	{
		uint32 SectionIndex = 0;
		uint32 BaseVertexIndex = 0;
		uint32 NumVertices = 0;
		uint32 NumBoneInfluences = 0;
		uint32 InputWeightStride = 0;
		uint32 InputWeightIndexSize = 0;
		bool bUnlimitedBoneInfluence = false;
		bool bBoneIndexUint16 = false;
		bool bBoneWeightsUint16 = false;
		FRHIShaderResourceView* WeightStreamSRV = nullptr;
		FRHIShaderResourceView* WeightLookupSRV = nullptr;
	};

	struct FDispatchContext
	{
		FDispatchContext()
		{
			for (int32 i = 0; i < FMilkyBodySkinCS::MaxPuniSlots; ++i)
			{
				PushPosAndRadius[i] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
				PushImpulse[i] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			}
		}

		FSkeletalMeshObject* MeshObject = nullptr;
		int32 LODIndex = 0;
		uint32 NumVertices = 0;
		FRHIShaderResourceView* StaticPositionsSRV = nullptr;
		FRHIShaderResourceView* StaticTangentsSRV = nullptr;
		TArray<FSectionDispatchData> Sections;
		FVector4f PushPosAndRadius[FMilkyBodySkinCS::MaxPuniSlots];
		FVector4f PushImpulse[FMilkyBodySkinCS::MaxPuniSlots];
		uint32 NumActivePuniSlots = 0;
		float GeometryDepthScale = 1.0f;
		float ShadingDepthScale = 1.0f;
		bool bEnableDebugDraw = false;
	};

// Particle state removed

	static void PopulateDispatchContext(
		FDispatchContext& Ctx,
		USkinnedMeshComponent* InMeshComponent,
		const UMilkyBodyDeformer* InAsset)
	{
		if (!InMeshComponent || !InMeshComponent->MeshObject)
		{
			return;
		}
		Ctx.MeshObject = InMeshComponent->MeshObject;
		Ctx.LODIndex = Ctx.MeshObject->GetLOD();

		FSkeletalMeshRenderData const& RenderData = Ctx.MeshObject->GetSkeletalMeshRenderData();
		if (!RenderData.LODRenderData.IsValidIndex(Ctx.LODIndex))
		{
			Ctx.MeshObject = nullptr;
			return;
		}
		FSkeletalMeshLODRenderData const& LodRenderData = RenderData.LODRenderData[Ctx.LODIndex];
		Ctx.NumVertices = LodRenderData.GetNumVertices();
		Ctx.StaticPositionsSRV = LodRenderData.StaticVertexBuffers.PositionVertexBuffer.GetSRV();
		Ctx.StaticTangentsSRV  = LodRenderData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

		FSkinWeightVertexBuffer const* WeightBuffer = LodRenderData.GetSkinWeightVertexBuffer();
		if (!WeightBuffer)
		{
			Ctx.MeshObject = nullptr;
			return;
		}
		FRHIShaderResourceView* WeightStreamSRV = WeightBuffer->GetDataVertexBuffer()->GetSRV();
		const bool bUnlimited = WeightBuffer->GetBoneInfluenceType() == GPUSkinBoneInfluenceType::UnlimitedBoneInfluence;
		FRHIShaderResourceView* LookupSRV = bUnlimited ? WeightBuffer->GetLookupVertexBuffer()->GetSRV() : nullptr;

		const uint32 NumSections = LodRenderData.RenderSections.Num();
		Ctx.Sections.Reserve(NumSections);
		for (uint32 S = 0; S < NumSections; ++S)
		{
			FSectionDispatchData Sec;
			Sec.SectionIndex = S;
			Sec.BaseVertexIndex = LodRenderData.RenderSections[S].BaseVertexIndex;
			Sec.NumVertices = LodRenderData.RenderSections[S].NumVertices;
			Sec.NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();
			Sec.InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
			Sec.InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() | (WeightBuffer->GetBoneWeightByteSize() << 8);
			Sec.bUnlimitedBoneInfluence = bUnlimited;
			Sec.bBoneIndexUint16 = WeightBuffer->Use16BitBoneIndex();
			Sec.bBoneWeightsUint16 = WeightBuffer->Use16BitBoneWeight();
			Sec.WeightStreamSRV = WeightStreamSRV;
			Sec.WeightLookupSRV = LookupSRV;
			Ctx.Sections.Add(Sec);
		}

		Ctx.bEnableDebugDraw = InAsset->bEnableDebugDraw;
		Ctx.GeometryDepthScale = FMath::Clamp(InAsset->GeometryDepthScale, 0.0f, 1.0f);
		Ctx.ShadingDepthScale  = FMath::Clamp(InAsset->ShadingDepthScale,  0.0f, 1.0f);
	}
} // namespace MilkyBodyPrivate

void UMilkyBodyDeformerInstance::EnqueueWork(const FEnqueueWorkDesc& InDesc)
{
	using namespace MilkyBodyPrivate;

	USkinnedMeshComponent* SkinnedComp = Cast<USkinnedMeshComponent>(MeshComponent.Get());
	const bool bCompNotReady =
		!IsValid(SkinnedComp)
		|| SkinnedComp->IsBeingDestroyed()
		|| !SkinnedComp->IsRegistered()
		|| !SkinnedComp->IsRenderStateCreated()
		|| SkinnedComp->IsRenderStateDirty();
	if (bCompNotReady || !SkinnedComp->MeshObject || !DeformerAsset)
	{
		ENQUEUE_RENDER_COMMAND(MilkyBodyFallback)(
			[FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate&)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		return;
	}

	// 非ゲームワールド(BP エディタプレビュー / アセットエディタプレビュー / サムネ生成 等)では
	// FSkeletalMeshObjectGPUSkin の per-LOD VertexFactories 配列がまだ構築されていない
	// 状態で本パスに入ることがあり、AllocateVertexFactoryPositionBuffer →
	// GetBaseSkinVertexFactory が境界チェックなしの配列参照で assert する。
	// レンダースレッドからは Num を安全に取れないため、ここでフォールバックに落とす。
	{
		const UWorld* World = SkinnedComp->GetWorld();
		if (!World || !World->IsGameWorld())
		{
			ENQUEUE_RENDER_COMMAND(MilkyBodyFallbackPreview)(
				[FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate&)
				{
					FallbackDelegate.ExecuteIfBound();
				});
			return;
		}
	}

	FDispatchContext Ctx;
	PopulateDispatchContext(Ctx, SkinnedComp, DeformerAsset);

	if (!Ctx.MeshObject || Ctx.NumVertices == 0 || !Ctx.StaticPositionsSRV || !Ctx.StaticTangentsSRV || Ctx.Sections.Num() == 0)
	{
		ENQUEUE_RENDER_COMMAND(MilkyBodyFallback)(
			[FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate&)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		return;
	}

	// Update Spring Simulation (Fixed time step for stability)
	UMilkyBodyDeformer* InAsset = DeformerAsset;
	const float DeltaTime = 1.0f / 60.0f;
	const float Stiffness = InAsset ? (InAsset->Stiffness * 2000.0f) : 1500.0f;
	const float Damping = InAsset ? (InAsset->Damping * 200.0f) : 20.0f;

	float Force = Stiffness * (TargetPushAmount - CurrentPushAmount) - Damping * PushVelocity;
	PushVelocity += Force * DeltaTime;
	CurrentPushAmount += PushVelocity * DeltaTime;

	// Drag-pull follow: smooth CurrentLocalOffset toward TargetLocalOffset, then
	// clamp magnitude to (Radius * MaxRatio). The offset will be added to the
	// impulse as a tangential pull — NOT to the dent center — so the falloff
	// region stays anchored while the inside vertices get yanked sideways.
	{
		const float FollowAlpha = InAsset ? FMath::Clamp(InAsset->HoldFollowSmoothing, 0.0f, 1.0f) : 0.3f;
		CurrentLocalOffset = FMath::Lerp(CurrentLocalOffset, TargetLocalOffset, FollowAlpha);

		const float MaxRatio = InAsset ? FMath::Clamp(InAsset->HoldMaxFollowOffsetRatio, 0.0f, 1.0f) : 0.5f;
		const float MaxOffset = PushRadius * MaxRatio;
		const float OffsetLen = CurrentLocalOffset.Length();
		if (OffsetLen > MaxOffset && OffsetLen > 1e-6f)
		{
			CurrentLocalOffset *= (MaxOffset / OffsetLen);
		}
	}

	const bool bAnyDeformActive =
		FMath::Abs(CurrentPushAmount) > 0.001f || FMath::Abs(PushVelocity) > 0.001f;

	if (!bAnyDeformActive && TargetPushAmount <= 0.0f && !bPushHeld)
	{
		CurrentPushAmount = 0.0f;
		PushVelocity = 0.0f;
		CurrentLocalOffset = FVector3f::ZeroVector;
		TargetLocalOffset = FVector3f::ZeroVector;
		bKneading = false;
		bMomi = false;
		bExternalMultiHold = false;
		ExternalLocalPositions.Reset();
		ExternalLocalImpulses.Reset();
		ExternalRadii.Reset();
	}

	if (bExternalMultiHold && bAnyDeformActive)
	{
		// External multi-hold: caller supplied per-slot world positions / radii /
		// impulses (e.g. fingertip bones snapped to body surface). Slots are not
		// procedurally animated — caller refreshes via BeginExternalMultiHold each
		// frame. Each slot uses its own per-slot impulse (typically inward along
		// the snapped surface normal × push depth).
		const int32 N = FMath::Min3(
			ExternalLocalPositions.Num(),
			FMath::Min(ExternalLocalImpulses.Num(), ExternalRadii.Num()),
			(int32)FMilkyBodySkinCS::MaxPuniSlots);
		Ctx.NumActivePuniSlots = (uint32)FMath::Max(N, 0);

		UWorld* DebugWorld = (Ctx.bEnableDebugDraw) ? SkinnedComp->GetWorld() : nullptr;
		const FTransform XformWS = SkinnedComp->GetComponentTransform();

		for (int32 i = 0; i < N; ++i)
		{
			const FVector3f& SlotPos = ExternalLocalPositions[i];
			const FVector3f& SlotImp = ExternalLocalImpulses[i];
			const float SlotR = ExternalRadii[i];

			Ctx.PushPosAndRadius[i] = FVector4f(SlotPos.X, SlotPos.Y, SlotPos.Z, SlotR);
			Ctx.PushImpulse[i] = FVector4f(
				SlotImp.X * CurrentPushAmount,
				SlotImp.Y * CurrentPushAmount,
				SlotImp.Z * CurrentPushAmount,
				0.0f);

			if (DebugWorld)
			{
				const FVector SlotWS = XformWS.TransformPosition(FVector(SlotPos.X, SlotPos.Y, SlotPos.Z));
				DrawDebugSphere(DebugWorld, SlotWS, SlotR, 12, FColor::Green, false, -1.0f, 0, 1.0f);
			}
		}
	}
	else if (bMomi && bAnyDeformActive)
	{
		// Momi (揉み): Each point traces a circular orbit around MomiCenterLocal
		// on the tangent plane. Points are evenly spaced around the circle with
		// per-point speed jitter and a subtle depth pulse for realism.
		MomiTime += DeltaTime;
		const int32 N = FMath::Clamp(NumMomiPoints, 1, FMilkyBodySkinCS::MaxPuniSlots);
		Ctx.NumActivePuniSlots = (uint32)N;

		UWorld* DebugWorld = (Ctx.bEnableDebugDraw) ? SkinnedComp->GetWorld() : nullptr;
		const FTransform XformWS = SkinnedComp->GetComponentTransform();

		for (int32 i = 0; i < N; ++i)
		{
			const float fi = (float)i;
			// Evenly distribute points around the orbit circle
			const float BasePhase = (fi / (float)N) * 2.0f * PI;

			// Per-point speed variation: each point orbits at slightly different speed
			// seeded deterministically so the pattern is stable but organic.
			const float PointSeed = MomiSeed + fi * 13.37f;
			const float SpeedJitter = 1.0f + MomiSpeedVariation * FMath::Sin(PointSeed * 3.71f);
			const float Angle = BasePhase + MomiTime * MomiOrbitSpeed * SpeedJitter;

			// Circular orbit with subtle radius wobble for organic feel
			const float RadiusWobble = 1.0f + 0.15f * FMath::Sin(MomiTime * 2.3f + PointSeed * 1.17f);
			const float EffectiveOrbitR = MomiOrbitRadius * RadiusWobble;

			const float u = FMath::Cos(Angle) * EffectiveOrbitR;
			const float v = FMath::Sin(Angle) * EffectiveOrbitR;
			const FVector3f Orbit =
				MomiTangentU * u +
				MomiTangentV * v;
			const FVector3f SlotPos = MomiCenterLocal + Orbit;

			// Depth pulsing: vary the push depth per-point over time
			const float DepthPulse = 1.0f + MomiDepthVariation *
				FMath::Sin(MomiTime * MomiOrbitSpeed * 1.7f + PointSeed * 2.53f);
			const float EffectiveAmount = CurrentPushAmount * DepthPulse;

			Ctx.PushPosAndRadius[i] = FVector4f(SlotPos.X, SlotPos.Y, SlotPos.Z, MomiEachRadius);
			Ctx.PushImpulse[i] = FVector4f(
				MomiImpulseLocal.X * EffectiveAmount,
				MomiImpulseLocal.Y * EffectiveAmount,
				MomiImpulseLocal.Z * EffectiveAmount,
				0.0f);

			if (DebugWorld)
			{
				const FVector SlotWS = XformWS.TransformPosition(FVector(SlotPos.X, SlotPos.Y, SlotPos.Z));
				DrawDebugSphere(DebugWorld, SlotWS, MomiEachRadius, 12, FColor::Orange, false, -1.0f, 0, 1.0f);
			}
		}
	}
	else if (bKneading && bAnyDeformActive)
	{
		// Kneading: animate up to NumKneadingPoints fingers wandering on the
		// (KneadTangentU, KneadTangentV) plane around KneadCenterLocal. Each
		// finger uses incommensurate sin/cos frequencies so they don't sync up.
		KneadingTime += DeltaTime;
		const int32 N = FMath::Clamp(NumKneadingPoints, 1, FMilkyBodySkinCS::MaxPuniSlots);
		Ctx.NumActivePuniSlots = (uint32)N;

		UWorld* DebugWorld = (Ctx.bEnableDebugDraw) ? SkinnedComp->GetWorld() : nullptr;
		const FTransform XformWS = SkinnedComp->GetComponentTransform();

		for (int32 i = 0; i < N; ++i)
		{
			// Per-finger phase / frequency seeded with i and KneadingSeed for variety.
			const float fi = (float)i;
			const float Seed = KneadingSeed + fi * 17.31f;
			const float FreqU = KneadingMotionSpeed * (0.7f + 0.3f * FMath::Sin(Seed));
			const float FreqV = KneadingMotionSpeed * (0.9f + 0.3f * FMath::Cos(Seed * 1.31f));
			const float PhaseU = Seed * 2.39f;       // golden-ratio-ish stride
			const float PhaseV = Seed * 4.13f + 1.7f;

			const float u = FMath::Sin(KneadingTime * FreqU + PhaseU);
			const float v = FMath::Cos(KneadingTime * FreqV + PhaseV);
			const FVector3f Wander =
				KneadTangentU * (u * KneadingScatterRadius) +
				KneadTangentV * (v * KneadingScatterRadius);
			const FVector3f SlotPos = KneadCenterLocal + Wander;

			Ctx.PushPosAndRadius[i] = FVector4f(SlotPos.X, SlotPos.Y, SlotPos.Z, KneadEachRadius);
			Ctx.PushImpulse[i] = FVector4f(
				KneadImpulseLocal.X * CurrentPushAmount,
				KneadImpulseLocal.Y * CurrentPushAmount,
				KneadImpulseLocal.Z * CurrentPushAmount,
				0.0f);

			if (DebugWorld)
			{
				const FVector SlotWS = XformWS.TransformPosition(FVector(SlotPos.X, SlotPos.Y, SlotPos.Z));
				DrawDebugSphere(DebugWorld, SlotWS, KneadEachRadius, 12, FColor::Magenta, false, -1.0f, 0, 1.0f);
			}
		}
	}
	else if (bAnyDeformActive)
	{
		// Single-slot hold/impulse path — slot 0 only.
		const FVector3f EffectiveLocalImpulse = AnchorLocalImpulse + CurrentLocalOffset;
		Ctx.NumActivePuniSlots = 1;
		Ctx.PushPosAndRadius[0] = FVector4f(AnchorLocalPos.X, AnchorLocalPos.Y, AnchorLocalPos.Z, PushRadius);
		Ctx.PushImpulse[0] = FVector4f(
			EffectiveLocalImpulse.X * CurrentPushAmount,
			EffectiveLocalImpulse.Y * CurrentPushAmount,
			EffectiveLocalImpulse.Z * CurrentPushAmount,
			0.0f);

		if (Ctx.bEnableDebugDraw)
		{
			UWorld* World = SkinnedComp->GetWorld();
			if (World)
			{
				FVector WorldLocation = SkinnedComp->GetComponentTransform().TransformPosition(FVector(AnchorLocalPos.X, AnchorLocalPos.Y, AnchorLocalPos.Z));
				DrawDebugSphere(World, WorldLocation, PushRadius, 16, FColor::Cyan, false, -1.0f, 0, 1.0f);
			}
		}
	}

	ENQUEUE_RENDER_COMMAND(MilkyBodyDispatch)(
		[Ctx = MoveTemp(Ctx),
		 FallbackDelegate = InDesc.FallbackDelegate,
		 InstanceWeak = TWeakObjectPtr<UMilkyBodyDeformerInstance>(this)]
		(FRHICommandListImmediate& RHICmdList) mutable
		{
			UMilkyBodyDeformerInstance* Instance = InstanceWeak.Get();
			if (!Instance)
			{
				FallbackDelegate.ExecuteIfBound();
				return;
			}

			// LOD may have changed (streaming/switch) between game-thread capture and render-thread execution.
			if (!Ctx.MeshObject || !Ctx.MeshObject->GetSkeletalMeshRenderData().LODRenderData.IsValidIndex(Ctx.LODIndex))
			{
				FallbackDelegate.ExecuteIfBound();
				return;
			}

			// AllocateVertexFactoryPositionBuffer → GetBaseSkinVertexFactory indexes into the
			// MeshObjectGPU's per-LOD VertexFactories array WITHOUT bounds checks (engine bug
			// surface). If the GPU-side LOD state isn't initialized yet, or every render
			// section is disabled (GetIndexOfFirstAvailableSection returns INDEX_NONE),
			// passing it through asserts with "index 0 into array of size 0". Bail early in
			// either case so we fall back to the default skinning path for this frame.
			if (Ctx.MeshObject->IsCPUSkinned())
			{
				FallbackDelegate.ExecuteIfBound();
				return;
			}
			{
				const TArray<FSkelMeshRenderSection>& RT_Sections = Ctx.MeshObject->GetRenderSections(Ctx.LODIndex);
				bool bAnySectionAvailable = false;
				for (const FSkelMeshRenderSection& Sec : RT_Sections)
				{
					if (!Sec.bDisabled) { bAnySectionAvailable = true; break; }
				}
				if (!bAnySectionAvailable)
				{
					FallbackDelegate.ExecuteIfBound();
					return;
				}
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGExternalAccessQueue ExternalAccessQueue;

			FRDGBuffer* PassthroughPos = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
				GraphBuilder, ExternalAccessQueue, Ctx.MeshObject, Ctx.LODIndex, TEXT("MilkyBodyPosition"));
			if (!PassthroughPos)
			{
				GraphBuilder.Execute();
				FallbackDelegate.ExecuteIfBound();
				return;
			}

			FRDGBuffer* PassthroughTan = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
				GraphBuilder, ExternalAccessQueue, Ctx.MeshObject, Ctx.LODIndex, TEXT("MilkyBodyTangent"));
			if (!PassthroughTan)
			{
				GraphBuilder.Execute();
				FallbackDelegate.ExecuteIfBound();
				return;
			}

			FRDGBufferUAVRef PassthroughUAV    = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PassthroughPos, PF_R32_FLOAT));
			FRDGBufferUAVRef PassthroughTanUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PassthroughTan, PF_R16G16B16A16_SNORM));
			FRHIShaderResourceView* NullSRVBinding = GWhiteVertexBufferWithSRV->ShaderResourceViewRHI.GetReference();

			// =========== Skin & Dent dispatch per section ===========
			uint32 RunningSectionOffset = 0;
			for (const FSectionDispatchData& Sec : Ctx.Sections)
			{
				FRHIShaderResourceView* BoneBufferSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
					Ctx.MeshObject, Ctx.LODIndex, Sec.SectionIndex, /*bPreviousFrame=*/false);
				const bool bValidBones = BoneBufferSRV != nullptr
					&& Sec.WeightStreamSRV != nullptr
					&& (!Sec.bUnlimitedBoneInfluence || Sec.WeightLookupSRV != nullptr);

				FMilkyBodySkinCS::FParameters* Params = GraphBuilder.AllocParameters<FMilkyBodySkinCS::FParameters>();
				Params->NumVertices = Ctx.NumVertices;
				Params->NumBoneInfluences = Sec.NumBoneInfluences;
				Params->InputWeightStride = Sec.InputWeightStride;
				Params->InputWeightIndexSize = Sec.InputWeightIndexSize;
				Params->SectionVertexOffset = RunningSectionOffset;
				Params->SectionNumVertices = Sec.NumVertices;
				Params->StaticPositions = Ctx.StaticPositionsSRV;
				Params->StaticTangents = Ctx.StaticTangentsSRV ? Ctx.StaticTangentsSRV : NullSRVBinding;
				Params->BoneMatrices = BoneBufferSRV ? BoneBufferSRV : NullSRVBinding;
				Params->InputWeightStream = Sec.WeightStreamSRV ? Sec.WeightStreamSRV : NullSRVBinding;
				Params->InputWeightLookupStream = Sec.WeightLookupSRV ? Sec.WeightLookupSRV : NullSRVBinding;
				Params->OutSkinnedPositions = PassthroughUAV;
				Params->OutSkinnedTangents = PassthroughTanUAV;
				for (int32 SlotIdx = 0; SlotIdx < FMilkyBodySkinCS::MaxPuniSlots; ++SlotIdx)
				{
					Params->PushPosAndRadius[SlotIdx] = Ctx.PushPosAndRadius[SlotIdx];
					Params->PushImpulse[SlotIdx] = Ctx.PushImpulse[SlotIdx];
				}
				Params->NumActivePuniSlots = Ctx.NumActivePuniSlots;
				Params->GeometryDepthScale = Ctx.GeometryDepthScale;
				Params->ShadingDepthScale  = Ctx.ShadingDepthScale;

				FMilkyBodySkinCS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FMilkyBodySkinCS::FEnableDeformerBones>(bValidBones);
				PermutationVector.Set<FMilkyBodySkinCS::FUnlimitedBoneInfluence>(Sec.bUnlimitedBoneInfluence);
				PermutationVector.Set<FMilkyBodySkinCS::FBoneIndexUint16>(Sec.bBoneIndexUint16);
				PermutationVector.Set<FMilkyBodySkinCS::FBoneWeightsUint16>(Sec.bBoneWeightsUint16);

				TShaderMapRef<FMilkyBodySkinCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
				const uint32 GroupCount = FMath::DivideAndRoundUp<uint32>(Sec.NumVertices, FMilkyBodySkinCS::ThreadGroupSize);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("MilkyBodySkin_Section%u", Sec.SectionIndex),
					Shader,
					Params,
					FIntVector(GroupCount, 1, 1));
					
				RunningSectionOffset += Sec.NumVertices;
			}

			FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(
				GraphBuilder, Ctx.MeshObject, Ctx.LODIndex, /*bInvalidatePreviousPosition=*/false);

			ExternalAccessQueue.Submit(GraphBuilder);
			GraphBuilder.Execute();
		});
}

EMeshDeformerOutputBuffer UMilkyBodyDeformerInstance::GetOutputBuffers() const
{
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition | EMeshDeformerOutputBuffer::SkinnedMeshTangents;
}

UMeshDeformerInstance* UMilkyBodyDeformerInstance::GetInstanceForSourceDeformer()
{
	return this;
}

#if WITH_EDITORONLY_DATA
bool UMilkyBodyDeformerInstance::RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest)
{
	return false;
}
#endif
