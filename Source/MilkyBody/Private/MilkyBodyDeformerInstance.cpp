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

	PushLocalPos = FVector3f(LocalPos);
	PushLocalImpulse = FVector3f(LocalImpulse);
	PushRadius = FMath::Max(Radius, 0.0f);
	
	// Apply a sudden impulse to velocity to kick off the jiggle seamlessly without snapping
	TargetPushAmount = 0.0f; 
	PushVelocity += 10.0f; // Give it a swift kick
	bPushHeld = false;
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
	PushLocalPos = FVector3f(Xform.InverseTransformPosition(WorldLocation));
	PushLocalImpulse = FVector3f(Xform.InverseTransformVectorNoScale(WorldImpulse));
	PushRadius = FMath::Max(Radius, 0.0f);
	bPushHeld = true;
	TargetPushAmount = 1.0f;
}

void UMilkyBodyDeformerInstance::ReleaseHoldPush(float FadeOutSeconds)
{
	if (!bPushHeld)
	{
		return;
	}
	bPushHeld = false;
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
		FSkeletalMeshObject* MeshObject = nullptr;
		int32 LODIndex = 0;
		uint32 NumVertices = 0;
		FRHIShaderResourceView* StaticPositionsSRV = nullptr;
		TArray<FSectionDispatchData> Sections;
		FVector4f PushPosAndRadius = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		FVector4f PushImpulse = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
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

	FDispatchContext Ctx;
	PopulateDispatchContext(Ctx, SkinnedComp, DeformerAsset);

	if (!Ctx.MeshObject || Ctx.NumVertices == 0 || !Ctx.StaticPositionsSRV || Ctx.Sections.Num() == 0)
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

	// Only dispatch push uniforms if we actually have some squish going on
	if (FMath::Abs(CurrentPushAmount) > 0.001f || FMath::Abs(PushVelocity) > 0.001f)
	{
		Ctx.PushPosAndRadius = FVector4f(PushLocalPos.X, PushLocalPos.Y, PushLocalPos.Z, PushRadius);
		Ctx.PushImpulse = FVector4f(
			PushLocalImpulse.X * CurrentPushAmount,
			PushLocalImpulse.Y * CurrentPushAmount,
			PushLocalImpulse.Z * CurrentPushAmount,
			0.0f);
	}

	if (Ctx.bEnableDebugDraw && FMath::Abs(CurrentPushAmount) > 0.001f)
	{
		UWorld* World = SkinnedComp->GetWorld();
		if (World)
		{
			FVector WorldLocation = SkinnedComp->GetComponentTransform().TransformPosition(FVector(PushLocalPos.X, PushLocalPos.Y, PushLocalPos.Z));
			DrawDebugSphere(World, WorldLocation, PushRadius, 16, FColor::Cyan, false, -1.0f, 0, 1.0f);
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

			FRDGBufferUAVRef PassthroughUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PassthroughPos, PF_R32_FLOAT));
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
				Params->BoneMatrices = BoneBufferSRV ? BoneBufferSRV : NullSRVBinding;
				Params->InputWeightStream = Sec.WeightStreamSRV ? Sec.WeightStreamSRV : NullSRVBinding;
				Params->InputWeightLookupStream = Sec.WeightLookupSRV ? Sec.WeightLookupSRV : NullSRVBinding;
				Params->OutSkinnedPositions = PassthroughUAV;
				Params->PushPosAndRadius = Ctx.PushPosAndRadius;
				Params->PushImpulse = Ctx.PushImpulse;

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
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition;
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
