#include "MilkyBodyDeformerInstance.h"
#include "MilkyBodyDeformer.h"
#include "MilkyBodyPrivate.h"
#include "MilkyBodyShaders.h"

#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "GlobalRenderResources.h"
#include "Misc/ScopeLock.h"
#include "ReferenceSkeleton.h"
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

DECLARE_CYCLE_STAT(TEXT("Deformer Tick"), STAT_MilkyBody_Tick, STATGROUP_MilkyBody);
DECLARE_CYCLE_STAT(TEXT("Deformer EnqueueWork"), STAT_MilkyBody_EnqueueWork, STATGROUP_MilkyBody);
DECLARE_DWORD_COUNTER_STAT(TEXT("Active Puni Slots"), STAT_MilkyBody_ActiveSlots, STATGROUP_MilkyBody);

namespace MilkyBodyInstancePrivate
{
	// Spring integration sub-step. Semi-implicit Euler is stable while
	// Damping*200*h < 2 and Stiffness*2000*h² is small; 240 Hz covers the whole
	// 0–1 range of both properties with a wide margin.
	constexpr float SpringSubstep = 1.0f / 240.0f;
	constexpr int32 MaxSpringSubsteps = 64;
	constexpr float MaxTickDelta = 0.1f;
	constexpr float SettleEpsilon = 1e-3f;
	constexpr float MinSlotAmount = 1e-4f;
	constexpr int32 MaxImpulses = 4;
	constexpr int32 MaxProceduralPoints = 4;
	constexpr float LegacyKickVelocity = 10.0f;

	static float GetUniformScale(const FTransform& Xform)
	{
		return FMath::Max((float)Xform.GetMaximumAxisScale(), KINDA_SMALL_NUMBER);
	}

	// The shader works in component space, so world-space radii must be divided
	// by the component scale and impulses transformed *with* scale.
	static float ToLocalRadius(const FTransform& Xform, float WorldRadius)
	{
		return FMath::Max(WorldRadius, 0.0f) / GetUniformScale(Xform);
	}

	static FVector3f ToLocalPosition(const FTransform& Xform, const FVector& WorldPosition)
	{
		return FVector3f(Xform.InverseTransformPosition(WorldPosition));
	}

	static FVector3f ToLocalVector(const FTransform& Xform, const FVector& WorldVector)
	{
		return FVector3f(Xform.InverseTransformVector(WorldVector));
	}

	// Tangent basis perpendicular to the impulse so procedural points travel
	// along the touched surface, not into / out of it.
	static void BuildTangentBasis(const FVector3f& LocalImpulse, FVector3f& OutU, FVector3f& OutV)
	{
		FVector N = FVector(LocalImpulse.X, LocalImpulse.Y, LocalImpulse.Z).GetSafeNormal();
		if (N.IsNearlyZero())
		{
			N = FVector(0, 0, -1);
		}
		const FVector Up = (FMath::Abs(N.Z) < 0.95f) ? FVector::UpVector : FVector::ForwardVector;
		const FVector U = FVector::CrossProduct(N, Up).GetSafeNormal();
		const FVector V = FVector::CrossProduct(N, U).GetSafeNormal();
		OutU = FVector3f(U);
		OutV = FVector3f(V);
	}

	static uint32 GetSourceId(const UObject* Source)
	{
		return Source ? Source->GetUniqueID() : 0u;
	}

	static uint64 MakeExternalKey(uint32 SourceId, int32 ContactId)
	{
		return (uint64(SourceId) << 32) | uint64(uint32(ContactId));
	}

	static uint32 GetExternalKeySource(uint64 Key)
	{
		return uint32(Key >> 32);
	}

	static bool IsDebugDrawForced()
	{
		return CVarMilkyBodyDebugDraw.GetValueOnAnyThread() != 0;
	}

	template <typename DelegateType>
	static void EnqueueFallback(const DelegateType& FallbackDelegate)
	{
		ENQUEUE_RENDER_COMMAND(MilkyBodyFallback)(
			[FallbackDelegate](FRHICommandListImmediate&)
			{
				FallbackDelegate.ExecuteIfBound();
			});
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Spring
// ─────────────────────────────────────────────────────────────────────────────

void MilkyBody::FPuniSpring::Step(float DeltaSeconds, float SpringK, float SpringC)
{
	using namespace MilkyBodyInstancePrivate;
	if (DeltaSeconds <= 0.0f)
	{
		return;
	}
	const int32 NumSubsteps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / SpringSubstep), 1, MaxSpringSubsteps);
	const float H = DeltaSeconds / (float)NumSubsteps;
	for (int32 i = 0; i < NumSubsteps; ++i)
	{
		const float Force = SpringK * (Target - Amount) - SpringC * Velocity;
		Velocity += Force * H;
		Amount += Velocity * H;
	}
}

bool MilkyBody::FPuniSpring::IsSettled() const
{
	using namespace MilkyBodyInstancePrivate;
	return Target <= 0.0f
		&& FMath::Abs(Amount) < SettleEpsilon
		&& FMath::Abs(Velocity) < SettleEpsilon;
}

// ─────────────────────────────────────────────────────────────────────────────
// UMeshDeformerInstance basics
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyDeformerInstance::AllocateResources()
{
	// Base class is PURE_VIRTUAL — do NOT call Super.
	// Actual GPU buffer allocation happens lazily on the render thread.
}

void UMilkyBodyDeformerInstance::ReleaseResources()
{
	FlushRenderingCommands();
}

USkinnedMeshComponent* UMilkyBodyDeformerInstance::GetSkinnedComponent() const
{
	return Cast<USkinnedMeshComponent>(MeshComponent.Get());
}

// ─────────────────────────────────────────────────────────────────────────────
// Interaction API
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyDeformerInstance::ApplyPushImpulse(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius,
	float DurationSeconds)
{
	using namespace MilkyBodyInstancePrivate;

	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();

	MilkyBody::FImpulseContact NewImpulse;
	NewImpulse.LocalPos = ToLocalPosition(Xform, WorldLocation);
	NewImpulse.LocalImpulse = ToLocalVector(Xform, WorldImpulse);
	NewImpulse.LocalRadius = ToLocalRadius(Xform, Radius);
	if (DurationSeconds > 0.0f)
	{
		// Press to full depth, hold for DurationSeconds, then spring back.
		NewImpulse.Spring.Target = 1.0f;
		NewImpulse.ReleaseTime = SimTime + DurationSeconds;
	}
	else
	{
		// Legacy behaviour: a short velocity kick that wobbles back to rest.
		NewImpulse.Spring.Target = 0.0f;
		NewImpulse.Spring.Velocity = LegacyKickVelocity;
		NewImpulse.ReleaseTime = SimTime;
	}

	Impulses.Add(NewImpulse);
	while (Impulses.Num() > MaxImpulses)
	{
		Impulses.RemoveAt(0);
	}
}

void UMilkyBodyDeformerInstance::BeginHoldPush(
	const FVector& WorldLocation,
	const FVector& WorldImpulse,
	float Radius)
{
	using namespace MilkyBodyInstancePrivate;

	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const FVector3f NewLocalPos = ToLocalPosition(Xform, WorldLocation);

	if (!Hold.bHeld)
	{
		// First frame of a fresh hold — anchor here, no offset yet.
		Hold.AnchorLocalPos = NewLocalPos;
		Hold.AnchorLocalImpulse = ToLocalVector(Xform, WorldImpulse);
		Hold.CurrentLocalOffset = FVector3f::ZeroVector;
		Hold.TargetLocalOffset = FVector3f::ZeroVector;
	}
	else
	{
		// Subsequent frame — anchor and impulse direction stay locked; only the
		// follow target moves. CurrentLocalOffset is smoothed toward this in Tick.
		Hold.TargetLocalOffset = NewLocalPos - Hold.AnchorLocalPos;
	}
	Hold.LocalRadius = ToLocalRadius(Xform, Radius);
	Hold.bHeld = true;
	Hold.LastRefreshTime = SimTime;
	Hold.Spring.Target = 1.0f;
}

void UMilkyBodyDeformerInstance::ReleaseHoldPush(float FadeOutSeconds)
{
	Hold.bHeld = false;
	Hold.Spring.Target = 0.0f;
	Hold.TargetLocalOffset = FVector3f::ZeroVector;

	if (FadeOutSeconds <= 0.0f)
	{
		Hold.Spring.SnapToZero();
		Hold.CurrentLocalOffset = FVector3f::ZeroVector;
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
	using namespace MilkyBodyInstancePrivate;

	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();

	const bool bFreshStart = !Kneading.bActive || Kneading.Spring.Target <= 0.0f;

	Kneading.CenterLocal = ToLocalPosition(Xform, WorldLocation);
	Kneading.ImpulseLocal = ToLocalVector(Xform, WorldImpulse);
	BuildTangentBasis(Kneading.ImpulseLocal, Kneading.TangentU, Kneading.TangentV);
	Kneading.NumPoints = FMath::Clamp(NumPoints, 1, MaxProceduralPoints);
	Kneading.LocalEachRadius = ToLocalRadius(Xform, Radius);
	Kneading.LocalScatterRadius = ToLocalRadius(Xform, ScatterRadius);
	Kneading.MotionSpeed = FMath::Max(MotionSpeed, 0.0f);
	Kneading.Seed = RandomSeed;
	if (bFreshStart)
	{
		Kneading.Time = 0.0f; // reset clock on a fresh begin
	}
	Kneading.bActive = true;
	Kneading.Spring.Target = 1.0f;
}

void UMilkyBodyDeformerInstance::EndKneading()
{
	// Keep bActive so the procedural motion continues while the spring fades.
	Kneading.Spring.Target = 0.0f;
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
	using namespace MilkyBodyInstancePrivate;

	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();

	const bool bFreshStart = !Momi.bActive || Momi.Spring.Target <= 0.0f;

	Momi.CenterLocal = ToLocalPosition(Xform, WorldLocation);
	Momi.ImpulseLocal = ToLocalVector(Xform, WorldImpulse);
	BuildTangentBasis(Momi.ImpulseLocal, Momi.TangentU, Momi.TangentV);
	Momi.NumPoints = FMath::Clamp(NumPoints, 1, MaxProceduralPoints);
	Momi.LocalEachRadius = ToLocalRadius(Xform, Radius);
	Momi.LocalOrbitRadius = ToLocalRadius(Xform, OrbitRadius);
	Momi.OrbitSpeed = FMath::Max(OrbitSpeed, 0.0f);
	Momi.SpeedVariation = FMath::Clamp(SpeedVariation, 0.0f, 1.0f);
	Momi.DepthVariation = FMath::Clamp(DepthVariation, 0.0f, 1.0f);
	Momi.Seed = RandomSeed;
	if (bFreshStart)
	{
		Momi.Time = 0.0f;
	}
	Momi.bActive = true;
	Momi.Spring.Target = 1.0f;
}

void UMilkyBodyDeformerInstance::EndMomi()
{
	// Keep bActive so the orbit continues while the spring fades.
	Momi.Spring.Target = 0.0f;
}

void UMilkyBodyDeformerInstance::UpdateExternalContacts(
	const UObject* Source,
	const TArray<int32>& ContactIds,
	const TArray<FVector>& WorldLocations,
	const TArray<float>& Radii,
	const TArray<FVector>& WorldImpulses)
{
	using namespace MilkyBodyInstancePrivate;

	USceneComponent* Comp = Cast<USceneComponent>(MeshComponent.Get());
	if (!Comp)
	{
		return;
	}
	const FTransform& Xform = Comp->GetComponentTransform();
	const uint32 SourceId = GetSourceId(Source);
	const uint64 Frame = GFrameCounter;

	const int32 Num = FMath::Min(
		FMath::Min(ContactIds.Num(), WorldLocations.Num()),
		FMath::Min(Radii.Num(), WorldImpulses.Num()));
	for (int32 i = 0; i < Num; ++i)
	{
		MilkyBody::FExternalContact& Contact = ExternalContacts.FindOrAdd(MakeExternalKey(SourceId, ContactIds[i]));
		Contact.LocalPos = ToLocalPosition(Xform, WorldLocations[i]);
		Contact.LocalImpulse = ToLocalVector(Xform, WorldImpulses[i]);
		Contact.LocalRadius = ToLocalRadius(Xform, Radii[i]);
		Contact.LastUpdateFrame = Frame;
		Contact.Spring.Target = 1.0f;
	}

	// Contacts of this source that weren't supplied this call have lifted off.
	for (TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
	{
		if (GetExternalKeySource(Pair.Key) == SourceId && Pair.Value.LastUpdateFrame != Frame)
		{
			Pair.Value.Spring.Target = 0.0f;
		}
	}
}

void UMilkyBodyDeformerInstance::ReleaseExternalContacts(const UObject* Source)
{
	using namespace MilkyBodyInstancePrivate;

	const uint32 SourceId = GetSourceId(Source);
	for (TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
	{
		if (GetExternalKeySource(Pair.Key) == SourceId)
		{
			Pair.Value.Spring.Target = 0.0f;
		}
	}
}

void UMilkyBodyDeformerInstance::BeginExternalMultiHold(
	const TArray<FVector>& WorldLocations,
	const TArray<float>& Radii,
	const TArray<FVector>& WorldImpulses)
{
	TArray<int32> ContactIds;
	ContactIds.Reserve(WorldLocations.Num());
	for (int32 i = 0; i < WorldLocations.Num(); ++i)
	{
		ContactIds.Add(i);
	}
	UpdateExternalContacts(nullptr, ContactIds, WorldLocations, Radii, WorldImpulses);
}

void UMilkyBodyDeformerInstance::EndExternalMultiHold()
{
	ReleaseExternalContacts(nullptr);
}

void UMilkyBodyDeformerInstance::ReleaseAll(float FadeOutSeconds)
{
	const bool bInstant = FadeOutSeconds <= 0.0f;

	ReleaseHoldPush(FadeOutSeconds);

	for (MilkyBody::FImpulseContact& Impulse : Impulses)
	{
		Impulse.Spring.Target = 0.0f;
		Impulse.ReleaseTime = SimTime;
	}
	if (bInstant)
	{
		Impulses.Reset();
	}

	EndKneading();
	EndMomi();
	if (bInstant)
	{
		Kneading.Spring.SnapToZero();
		Kneading.bActive = false;
		Momi.Spring.SnapToZero();
		Momi.bActive = false;
	}

	for (TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
	{
		Pair.Value.Spring.Target = 0.0f;
	}
	if (bInstant)
	{
		ExternalContacts.Reset();
	}
}

float UMilkyBodyDeformerInstance::GetPuniAmount() const
{
	float MaxAmount = Hold.Spring.Amount;
	for (const MilkyBody::FImpulseContact& Impulse : Impulses)
	{
		MaxAmount = FMath::Max(MaxAmount, Impulse.Spring.Amount);
	}
	if (Kneading.bActive)
	{
		MaxAmount = FMath::Max(MaxAmount, Kneading.Spring.Amount);
	}
	if (Momi.bActive)
	{
		MaxAmount = FMath::Max(MaxAmount, Momi.Spring.Amount);
	}
	for (const TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
	{
		MaxAmount = FMath::Max(MaxAmount, Pair.Value.Spring.Amount);
	}
	return FMath::Max(MaxAmount, 0.0f);
}

bool UMilkyBodyDeformerInstance::IsPuniActive() const
{
	if (Hold.bHeld)
	{
		return true;
	}
	for (const MilkyBody::FImpulseContact& Impulse : Impulses)
	{
		if (Impulse.Spring.Target > 0.0f)
		{
			return true;
		}
	}
	if ((Kneading.bActive && Kneading.Spring.Target > 0.0f) || (Momi.bActive && Momi.Spring.Target > 0.0f))
	{
		return true;
	}
	for (const TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
	{
		if (Pair.Value.Spring.Target > 0.0f)
		{
			return true;
		}
	}
	return false;
}

bool UMilkyBodyDeformerInstance::HasAnyContact() const
{
	return Hold.bHeld
		|| !Hold.Spring.IsSettled()
		|| Impulses.Num() > 0
		|| Kneading.bActive
		|| Momi.bActive
		|| ExternalContacts.Num() > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick (game thread): spring simulation, slot packing, debug draw
// ─────────────────────────────────────────────────────────────────────────────

bool UMilkyBodyDeformerInstance::IsTickable() const
{
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !IsValid(this) || !DeformerAsset || !MeshComponent.IsValid())
	{
		return false;
	}
	return HasAnyContact() || bSlotsActiveLastTick;
}

UWorld* UMilkyBodyDeformerInstance::GetTickableGameObjectWorld() const
{
	const UMeshComponent* Comp = MeshComponent.Get();
	return Comp ? Comp->GetWorld() : nullptr;
}

TStatId UMilkyBodyDeformerInstance::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMilkyBodyDeformerInstance, STATGROUP_Tickables);
}

void UMilkyBodyDeformerInstance::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_MilkyBody_Tick);
	using namespace MilkyBodyInstancePrivate;

	USkinnedMeshComponent* SkinnedComp = GetSkinnedComponent();
	const UMilkyBodyDeformer* Asset = DeformerAsset;
	if (!SkinnedComp || !Asset)
	{
		return;
	}

	const float Dt = FMath::Clamp(DeltaTime, 0.0f, MaxTickDelta);
	SimTime += Dt;

	const float SpringK = FMath::Clamp(Asset->Stiffness, 0.0f, 1.0f) * 2000.0f;
	const float SpringC = FMath::Clamp(Asset->Damping, 0.0f, 1.0f) * 200.0f;

	// --- Single hold ---
	if (Hold.bHeld
		&& Asset->HoldAutoReleaseSeconds > 0.0f
		&& (SimTime - Hold.LastRefreshTime) > Asset->HoldAutoReleaseSeconds)
	{
		// The caller stopped refreshing (e.g. the cursor left the mesh): spring back.
		ReleaseHoldPush(/*FadeOutSeconds=*/1.0f);
	}
	Hold.Spring.Step(Dt, SpringK, SpringC);
	{
		// Drag-pull follow. HoldFollowSmoothing was tuned as a per-frame lerp at
		// 60 Hz; convert it to the equivalent rate for the actual frame time.
		const float FollowAlpha = FMath::Clamp(Asset->HoldFollowSmoothing, 0.0f, 1.0f);
		const float FrameAlpha = (FollowAlpha >= 1.0f)
			? 1.0f
			: 1.0f - FMath::Pow(1.0f - FollowAlpha, Dt * 60.0f);
		Hold.CurrentLocalOffset = FMath::Lerp(Hold.CurrentLocalOffset, Hold.TargetLocalOffset, FrameAlpha);

		const float MaxOffset = Hold.LocalRadius * FMath::Clamp(Asset->HoldMaxFollowOffsetRatio, 0.0f, 1.0f);
		const float OffsetLen = Hold.CurrentLocalOffset.Length();
		if (OffsetLen > MaxOffset && OffsetLen > 1e-6f)
		{
			Hold.CurrentLocalOffset *= (MaxOffset / OffsetLen);
		}
	}
	if (!Hold.bHeld && Hold.Spring.IsSettled())
	{
		Hold.Spring.SnapToZero();
		Hold.CurrentLocalOffset = FVector3f::ZeroVector;
	}

	// --- One-shot impulses ---
	for (int32 i = Impulses.Num() - 1; i >= 0; --i)
	{
		MilkyBody::FImpulseContact& Impulse = Impulses[i];
		if (SimTime >= Impulse.ReleaseTime)
		{
			Impulse.Spring.Target = 0.0f;
		}
		Impulse.Spring.Step(Dt, SpringK, SpringC);
		if (Impulse.Spring.IsSettled())
		{
			Impulses.RemoveAt(i);
		}
	}

	// --- Kneading / Momi ---
	if (Kneading.bActive)
	{
		Kneading.Time += Dt;
		Kneading.Spring.Step(Dt, SpringK, SpringC);
		if (Kneading.Spring.IsSettled())
		{
			Kneading.Spring.SnapToZero();
			Kneading.bActive = false;
		}
	}
	if (Momi.bActive)
	{
		Momi.Time += Dt;
		Momi.Spring.Step(Dt, SpringK, SpringC);
		if (Momi.Spring.IsSettled())
		{
			Momi.Spring.SnapToZero();
			Momi.bActive = false;
		}
	}

	// --- External contacts: anything not refreshed this frame lifts off ---
	const uint64 Frame = GFrameCounter;
	for (auto It = ExternalContacts.CreateIterator(); It; ++It)
	{
		MilkyBody::FExternalContact& Contact = It.Value();
		if (Contact.LastUpdateFrame != Frame)
		{
			Contact.Spring.Target = 0.0f;
		}
		Contact.Spring.Step(Dt, SpringK, SpringC);
		if (Contact.Spring.IsSettled())
		{
			It.RemoveCurrent();
		}
	}

	const bool bDebugDraw = Asset->bEnableDebugDraw || IsDebugDrawForced();
	int32 NumSlots = 0;
	{
		FScopeLock Lock(&PendingSlotsLock);
		BuildSlots(SkinnedComp->GetComponentTransform(), bDebugDraw, bDebugDraw ? SkinnedComp->GetWorld() : nullptr);
		NumSlots = PendingPosAndRadius.Num();
	}
	INC_DWORD_STAT_BY(STAT_MilkyBody_ActiveSlots, NumSlots);

	const bool bSlotsActive = NumSlots > 0;
	if (bSlotsActive || bSlotsActiveLastTick)
	{
		// Ensure EnqueueWork runs this frame even when the mesh isn't animating,
		// otherwise a dent on a static pose would freeze mid-spring.
		SkinnedComp->MarkRenderDynamicDataDirty();
	}
	bSlotsActiveLastTick = bSlotsActive;
}

void UMilkyBodyDeformerInstance::BuildSlots(const FTransform& ComponentToWorld, bool bDebugDraw, UWorld* DebugWorld)
{
	using namespace MilkyBodyInstancePrivate;

	PendingPosAndRadius.Reset();
	PendingImpulse.Reset();

	const int32 MaxSlots = FMilkyBodySkinCS::MaxPuniSlots;
	const float Scale = GetUniformScale(ComponentToWorld);
	UWorld* DrawWorld = bDebugDraw ? DebugWorld : nullptr;

	auto AddSlot = [&](const FVector3f& LocalPos, float LocalRadius, const FVector3f& LocalImpulse, float Amount, const FColor& DebugColor)
	{
		if (PendingPosAndRadius.Num() >= MaxSlots || LocalRadius <= 0.0f || FMath::Abs(Amount) < MinSlotAmount)
		{
			return;
		}
		const FVector3f ScaledImpulse = LocalImpulse * Amount;
		PendingPosAndRadius.Add(FVector4f(LocalPos.X, LocalPos.Y, LocalPos.Z, LocalRadius));
		PendingImpulse.Add(FVector4f(ScaledImpulse.X, ScaledImpulse.Y, ScaledImpulse.Z, 0.0f));

		if (DrawWorld)
		{
			const FVector WorldPos = ComponentToWorld.TransformPosition(FVector(LocalPos.X, LocalPos.Y, LocalPos.Z));
			DrawDebugSphere(DrawWorld, WorldPos, LocalRadius * Scale, 12, DebugColor, false, -1.0f, 0, 1.0f);
		}
	};

	// Single hold / drag: the follow offset is added to the impulse as a
	// tangential pull while the falloff region stays anchored.
	AddSlot(Hold.AnchorLocalPos, Hold.LocalRadius, Hold.AnchorLocalImpulse + Hold.CurrentLocalOffset, Hold.Spring.Amount, FColor::Cyan);

	for (const MilkyBody::FImpulseContact& Impulse : Impulses)
	{
		AddSlot(Impulse.LocalPos, Impulse.LocalRadius, Impulse.LocalImpulse, Impulse.Spring.Amount, FColor::Yellow);
	}

	if (Kneading.bActive)
	{
		// Each finger wanders on the tangent plane with incommensurate sin/cos
		// frequencies so they don't sync up.
		for (int32 i = 0; i < Kneading.NumPoints; ++i)
		{
			const float fi = (float)i;
			const float Seed = Kneading.Seed + fi * 17.31f;
			const float FreqU = Kneading.MotionSpeed * (0.7f + 0.3f * FMath::Sin(Seed));
			const float FreqV = Kneading.MotionSpeed * (0.9f + 0.3f * FMath::Cos(Seed * 1.31f));
			const float PhaseU = Seed * 2.39f;       // golden-ratio-ish stride
			const float PhaseV = Seed * 4.13f + 1.7f;

			const float u = FMath::Sin(Kneading.Time * FreqU + PhaseU);
			const float v = FMath::Cos(Kneading.Time * FreqV + PhaseV);
			const FVector3f Wander =
				Kneading.TangentU * (u * Kneading.LocalScatterRadius) +
				Kneading.TangentV * (v * Kneading.LocalScatterRadius);

			AddSlot(Kneading.CenterLocal + Wander, Kneading.LocalEachRadius, Kneading.ImpulseLocal, Kneading.Spring.Amount, FColor::Magenta);
		}
	}

	if (Momi.bActive)
	{
		// Points evenly spaced around a circular orbit with per-point speed
		// jitter, radius wobble and depth pulsing.
		for (int32 i = 0; i < Momi.NumPoints; ++i)
		{
			const float fi = (float)i;
			const float BasePhase = (fi / (float)Momi.NumPoints) * 2.0f * PI;
			const float PointSeed = Momi.Seed + fi * 13.37f;
			const float SpeedJitter = 1.0f + Momi.SpeedVariation * FMath::Sin(PointSeed * 3.71f);
			const float Angle = BasePhase + Momi.Time * Momi.OrbitSpeed * SpeedJitter;

			const float RadiusWobble = 1.0f + 0.15f * FMath::Sin(Momi.Time * 2.3f + PointSeed * 1.17f);
			const float EffectiveOrbitR = Momi.LocalOrbitRadius * RadiusWobble;
			const FVector3f Orbit =
				Momi.TangentU * (FMath::Cos(Angle) * EffectiveOrbitR) +
				Momi.TangentV * (FMath::Sin(Angle) * EffectiveOrbitR);

			const float DepthPulse = 1.0f + Momi.DepthVariation *
				FMath::Sin(Momi.Time * Momi.OrbitSpeed * 1.7f + PointSeed * 2.53f);

			AddSlot(Momi.CenterLocal + Orbit, Momi.LocalEachRadius, Momi.ImpulseLocal, Momi.Spring.Amount * DepthPulse, FColor::Orange);
		}
	}

	if (ExternalContacts.Num() > 0)
	{
		// When over budget, keep the most strongly pressed contacts.
		TArray<const MilkyBody::FExternalContact*, TInlineAllocator<32>> Sorted;
		for (const TPair<uint64, MilkyBody::FExternalContact>& Pair : ExternalContacts)
		{
			Sorted.Add(&Pair.Value);
		}
		if (PendingPosAndRadius.Num() + Sorted.Num() > MaxSlots)
		{
			Sorted.Sort([](const MilkyBody::FExternalContact& A, const MilkyBody::FExternalContact& B)
			{
				return FMath::Abs(A.Spring.Amount) > FMath::Abs(B.Spring.Amount);
			});
		}
		for (const MilkyBody::FExternalContact* Contact : Sorted)
		{
			AddSlot(Contact->LocalPos, Contact->LocalRadius, Contact->LocalImpulse, Contact->Spring.Amount, FColor::Green);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Mask (bone softness)
// ─────────────────────────────────────────────────────────────────────────────

void UMilkyBodyDeformerInstance::UpdateSoftnessCache(USkinnedMeshComponent* SkinnedComp, int32 LODIndex)
{
	const UMilkyBodyDeformer* Asset = DeformerAsset;
	USkinnedAsset* SkinnedAsset = SkinnedComp ? SkinnedComp->GetSkinnedAsset() : nullptr;
	if (!Asset)
	{
		return;
	}

	const float DefaultSoftness = FMath::Clamp(Asset->DefaultBoneSoftness, 0.0f, 1.0f);
	uint32 Hash = GetTypeHash(DefaultSoftness);
	for (const FMilkyBodyBoneSoftness& Entry : Asset->BoneSoftness)
	{
		Hash = HashCombine(Hash, GetTypeHash(Entry.BoneName));
		Hash = HashCombine(Hash, GetTypeHash(Entry.Softness));
		Hash = HashCombine(Hash, GetTypeHash((uint8)(Entry.bIncludeChildren ? 1 : 0)));
	}

	if (SoftnessCache.SkinnedAsset == SkinnedAsset
		&& SoftnessCache.LODIndex == LODIndex
		&& SoftnessCache.SettingsHash == Hash)
	{
		return;
	}

	SoftnessCache.SkinnedAsset = SkinnedAsset;
	SoftnessCache.LODIndex = LODIndex;
	SoftnessCache.SettingsHash = Hash;
	SoftnessCache.bUseBoneSoftness = false;
	SoftnessCache.PerSection.Reset();

	if (!SkinnedAsset || Asset->BoneSoftness.Num() == 0)
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = SkinnedAsset->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	TMap<int32, const FMilkyBodyBoneSoftness*> EntriesByBone;
	for (const FMilkyBodyBoneSoftness& Entry : Asset->BoneSoftness)
	{
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(Entry.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogMilkyBody, Warning,
				TEXT("[MilkyBody] BoneSoftness: bone '%s' not found on '%s'; entry ignored."),
				*Entry.BoneName.ToString(), *GetNameSafe(SkinnedAsset));
			continue;
		}
		EntriesByBone.Add(BoneIndex, &Entry);
	}
	if (EntriesByBone.Num() == 0)
	{
		return;
	}

	// Resolve each bone: its own entry, else the nearest ancestor entry with
	// bIncludeChildren, else the default.
	TArray<float> SoftnessPerBone;
	SoftnessPerBone.SetNumUninitialized(NumBones);
	for (int32 Bone = 0; Bone < NumBones; ++Bone)
	{
		float Softness = DefaultSoftness;
		for (int32 Walk = Bone; Walk != INDEX_NONE; Walk = RefSkeleton.GetParentIndex(Walk))
		{
			if (const FMilkyBodyBoneSoftness* const* Found = EntriesByBone.Find(Walk))
			{
				if (Walk == Bone || (*Found)->bIncludeChildren)
				{
					Softness = FMath::Clamp((*Found)->Softness, 0.0f, 1.0f);
					break;
				}
			}
		}
		SoftnessPerBone[Bone] = Softness;
	}

	FSkeletalMeshRenderData* RenderData = SkinnedComp->GetSkeletalMeshRenderData();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return;
	}
	const FSkeletalMeshLODRenderData& LodRenderData = RenderData->LODRenderData[LODIndex];

	// Fake bone matrices per section: 3 float4 rows per section-local bone,
	// row0.w = softness. Blended by skin weights in the shader.
	TSharedRef<TArray<TArray<FVector4f>>, ESPMode::ThreadSafe> PerSection =
		MakeShared<TArray<TArray<FVector4f>>, ESPMode::ThreadSafe>();
	PerSection->SetNum(LodRenderData.RenderSections.Num());
	for (int32 SectionIndex = 0; SectionIndex < LodRenderData.RenderSections.Num(); ++SectionIndex)
	{
		const TArray<FBoneIndexType>& BoneMap = LodRenderData.RenderSections[SectionIndex].BoneMap;
		TArray<FVector4f>& Rows = (*PerSection)[SectionIndex];
		Rows.SetNumZeroed(BoneMap.Num() * 3);
		for (int32 LocalBone = 0; LocalBone < BoneMap.Num(); ++LocalBone)
		{
			const int32 RefBone = BoneMap[LocalBone];
			const float Softness = SoftnessPerBone.IsValidIndex(RefBone) ? SoftnessPerBone[RefBone] : DefaultSoftness;
			Rows[LocalBone * 3] = FVector4f(0.0f, 0.0f, 0.0f, Softness);
		}
	}

	SoftnessCache.PerSection = PerSection;
	SoftnessCache.bUseBoneSoftness = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU dispatch
// ─────────────────────────────────────────────────────────────────────────────

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

		// Mask
		TSharedPtr<const TArray<TArray<FVector4f>>, ESPMode::ThreadSafe> SoftnessPerSection;
		float DefaultBoneSoftness = 1.0f;
		FRHIShaderResourceView* VertexColorsSRV = nullptr;
		uint32 ColorIndexMask = 0;
		uint32 VertexColorMaskChannel = 0;
		uint32 bInvertVertexColorMask = 0;
	};

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

		Ctx.GeometryDepthScale = FMath::Clamp(InAsset->GeometryDepthScale, 0.0f, 1.0f);
		Ctx.ShadingDepthScale  = FMath::Clamp(InAsset->ShadingDepthScale,  0.0f, 1.0f);

		// Vertex-color mask source. Meshes without vertex colors have an empty
		// color buffer; the caller disables the channel in that case.
		const FColorVertexBuffer& ColorBuffer = LodRenderData.StaticVertexBuffers.ColorVertexBuffer;
		Ctx.VertexColorsSRV = (ColorBuffer.GetNumVertices() > 0) ? ColorBuffer.GetColorComponentsSRV() : nullptr;
		Ctx.ColorIndexMask = Ctx.VertexColorsSRV ? ~0u : 0u;
		Ctx.VertexColorMaskChannel = (uint32)InAsset->VertexColorMaskChannel;
		Ctx.bInvertVertexColorMask = InAsset->bInvertVertexColorMask ? 1u : 0u;
	}
} // namespace MilkyBodyPrivate

void UMilkyBodyDeformerInstance::EnqueueWork(const FEnqueueWorkDesc& InDesc)
{
	SCOPE_CYCLE_COUNTER(STAT_MilkyBody_EnqueueWork);
	using namespace MilkyBodyPrivate;
	using namespace MilkyBodyInstancePrivate;

	if (CVarMilkyBodyEnable.GetValueOnAnyThread() == 0)
	{
		EnqueueFallback(InDesc.FallbackDelegate);
		return;
	}

	USkinnedMeshComponent* SkinnedComp = GetSkinnedComponent();
	const bool bCompNotReady =
		!IsValid(SkinnedComp)
		|| SkinnedComp->IsBeingDestroyed()
		|| !SkinnedComp->IsRegistered()
		|| !SkinnedComp->IsRenderStateCreated()
		|| SkinnedComp->IsRenderStateDirty();
	if (bCompNotReady || !SkinnedComp->MeshObject || !DeformerAsset)
	{
		EnqueueFallback(InDesc.FallbackDelegate);
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
			EnqueueFallback(InDesc.FallbackDelegate);
			return;
		}
	}

	FDispatchContext Ctx;
	PopulateDispatchContext(Ctx, SkinnedComp, DeformerAsset);

	if (!Ctx.MeshObject || Ctx.NumVertices == 0 || !Ctx.StaticPositionsSRV || !Ctx.StaticTangentsSRV || Ctx.Sections.Num() == 0)
	{
		EnqueueFallback(InDesc.FallbackDelegate);
		return;
	}

	// Slots were simulated and packed in Tick (component space).
	{
		FScopeLock Lock(&PendingSlotsLock);
		const int32 NumSlots = FMath::Min(
			FMath::Min(PendingPosAndRadius.Num(), PendingImpulse.Num()),
			(int32)FMilkyBodySkinCS::MaxPuniSlots);
		for (int32 i = 0; i < NumSlots; ++i)
		{
			Ctx.PushPosAndRadius[i] = PendingPosAndRadius[i];
			Ctx.PushImpulse[i] = PendingImpulse[i];
		}
		Ctx.NumActivePuniSlots = (uint32)NumSlots;
	}

	// Mask
	if (Ctx.VertexColorMaskChannel != 0 && !Ctx.VertexColorsSRV)
	{
		if (!bWarnedNoVertexColors)
		{
			bWarnedNoVertexColors = true;
			UE_LOG(LogMilkyBody, Warning,
				TEXT("[MilkyBody] VertexColorMaskChannel is set but '%s' has no vertex colors; the vertex-color mask is ignored."),
				*GetNameSafe(SkinnedComp->GetSkinnedAsset()));
		}
		Ctx.VertexColorMaskChannel = 0;
	}
	UpdateSoftnessCache(SkinnedComp, Ctx.LODIndex);
	Ctx.DefaultBoneSoftness = FMath::Clamp(DeformerAsset->DefaultBoneSoftness, 0.0f, 1.0f);
	if (SoftnessCache.bUseBoneSoftness)
	{
		Ctx.SoftnessPerSection = SoftnessCache.PerSection;
	}

	ENQUEUE_RENDER_COMMAND(MilkyBodyDispatch)(
		[Ctx = MoveTemp(Ctx),
		 FallbackDelegate = InDesc.FallbackDelegate]
		(FRHICommandListImmediate& RHICmdList) mutable
		{
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

			// Bound when a section has no softness data (the shader skips it then).
			FRDGBufferSRVRef DummySoftnessSRV = nullptr;
			auto GetDummySoftnessSRV = [&GraphBuilder, &DummySoftnessSRV]()
			{
				if (!DummySoftnessSRV)
				{
					static const FVector4f ZeroRows[3] = { FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0), FVector4f(0, 0, 0, 0) };
					FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 3), TEXT("MilkyBodySoftnessDummy"));
					GraphBuilder.QueueBufferUpload(DummyBuffer, ZeroRows, sizeof(ZeroRows), ERDGInitialDataFlags::NoCopy);
					DummySoftnessSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(DummyBuffer, PF_A32B32G32R32F));
				}
				return DummySoftnessSRV;
			};

			// =========== Skin & Dent dispatch per section ===========
			{
				RDG_EVENT_SCOPE(GraphBuilder, "MilkyBody");
				for (const FSectionDispatchData& Sec : Ctx.Sections)
				{
					FRHIShaderResourceView* BoneBufferSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
						Ctx.MeshObject, Ctx.LODIndex, Sec.SectionIndex, /*bPreviousFrame=*/false);
					const bool bValidBones = BoneBufferSRV != nullptr
						&& Sec.WeightStreamSRV != nullptr
						&& (!Sec.bUnlimitedBoneInfluence || Sec.WeightLookupSRV != nullptr);

					// Per-section softness rows (uploaded each frame; a few KB at most).
					FRDGBufferSRVRef SoftnessSRV = nullptr;
					bool bUseBoneSoftness = false;
					if (Ctx.SoftnessPerSection.IsValid()
						&& Ctx.SoftnessPerSection->IsValidIndex((int32)Sec.SectionIndex)
						&& (*Ctx.SoftnessPerSection)[Sec.SectionIndex].Num() > 0)
					{
						const TArray<FVector4f>& Rows = (*Ctx.SoftnessPerSection)[Sec.SectionIndex];
						FRDGBufferRef SoftnessBuffer = GraphBuilder.CreateBuffer(
							FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Rows.Num()), TEXT("MilkyBodySoftness"));
						// The shared array outlives GraphBuilder.Execute (held by Ctx).
						GraphBuilder.QueueBufferUpload(SoftnessBuffer, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::NoCopy);
						SoftnessSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SoftnessBuffer, PF_A32B32G32R32F));
						bUseBoneSoftness = true;
					}
					else
					{
						SoftnessSRV = GetDummySoftnessSRV();
					}

					FMilkyBodySkinCS::FParameters* Params = GraphBuilder.AllocParameters<FMilkyBodySkinCS::FParameters>();
					Params->NumVertices = Ctx.NumVertices;
					Params->NumBoneInfluences = Sec.NumBoneInfluences;
					Params->InputWeightStride = Sec.InputWeightStride;
					Params->InputWeightIndexSize = Sec.InputWeightIndexSize;
					Params->SectionVertexOffset = Sec.BaseVertexIndex;
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
					Params->bUseBoneSoftness = bUseBoneSoftness ? 1u : 0u;
					Params->DefaultBoneSoftness = Ctx.DefaultBoneSoftness;
					Params->SoftnessBoneMatrices = SoftnessSRV;
					Params->VertexColors = Ctx.VertexColorsSRV ? Ctx.VertexColorsSRV : NullSRVBinding;
					Params->ColorIndexMask = Ctx.ColorIndexMask;
					Params->VertexColorMaskChannel = Ctx.VertexColorMaskChannel;
					Params->bInvertVertexColorMask = Ctx.bInvertVertexColorMask;

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
				}
			} // RDG_EVENT_SCOPE

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
