#include "MilkyBodyCPUSkinning.h"

#include "MilkyBodyDeformerInstance.h"
#include "MilkyBodyPrivate.h"

#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/World.h"
#include "RawIndexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalRenderPublic.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectIterator.h"

DECLARE_CYCLE_STAT(TEXT("CPU Skinning"), STAT_MilkyBody_CPUSkinning, STATGROUP_MilkyBody);

namespace MilkyBodyCPUSkinningPrivate
{
	// Game-thread only caches.
	static TMap<TPair<FObjectKey, int32>, float> GWindingSignCache;
	static TSet<FObjectKey> GWarnedNoCPUAccess;

	static bool HasCPUReadableData(USkinnedMeshComponent* Comp, FSkeletalMeshLODRenderData& LODData, int32 LODIndex)
	{
		USkinnedAsset* Asset = Comp->GetSkinnedAsset();
		bool bReadable = LODData.StaticVertexBuffers.PositionVertexBuffer.GetVertexData() != nullptr
			&& LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() > 0;

		// In cooked builds the CPU copies are discarded unless the LOD opts in.
		if (bReadable && FPlatformProperties::RequiresCookedData())
		{
			const FSkeletalMeshLODInfo* LODInfo = Asset ? Asset->GetLODInfo(LODIndex) : nullptr;
			bReadable = LODInfo && LODInfo->bAllowCPUAccess;
		}

		if (!bReadable && Asset && !GWarnedNoCPUAccess.Contains(FObjectKey(Asset)))
		{
			GWarnedNoCPUAccess.Add(FObjectKey(Asset));
			UE_LOG(LogMilkyBody, Warning,
				TEXT("[MilkyBody] '%s' LOD%d has no CPU-readable vertex data. Polygon traces and BoneSpherePuni surface snapping "
				     "are skipped for it. Enable 'Allow CPU Access' on that LOD of the Skeletal Mesh for packaged builds."),
				*Asset->GetName(), LODIndex);
		}
		return bReadable;
	}

	// Compares face winding against the authored vertex normals (ref pose) so
	// we know which side of a triangle is outside. Rigid skinning preserves the
	// orientation, so one evaluation per mesh LOD is enough.
	static float ComputeWindingSign(const FSkeletalMeshLODRenderData& LODData, const FRawStaticIndexBuffer16or32Interface& IndexBuffer)
	{
		const FPositionVertexBuffer& PositionBuffer = LODData.StaticVertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& TangentBuffer = LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
		const uint32 NumVertices = PositionBuffer.GetNumVertices();
		if (TangentBuffer.GetNumVertices() != NumVertices)
		{
			return 1.0f;
		}

		const int32 NumTriangles = IndexBuffer.Num() / 3;
		const int32 Stride = FMath::Max(1, NumTriangles / 512);
		double Accum = 0.0;
		for (int32 Tri = 0; Tri < NumTriangles; Tri += Stride)
		{
			const uint32 I0 = IndexBuffer.Get(Tri * 3 + 0);
			const uint32 I1 = IndexBuffer.Get(Tri * 3 + 1);
			const uint32 I2 = IndexBuffer.Get(Tri * 3 + 2);
			if (I0 >= NumVertices || I1 >= NumVertices || I2 >= NumVertices)
			{
				continue;
			}
			const FVector3f P0 = PositionBuffer.VertexPosition(I0);
			const FVector3f P1 = PositionBuffer.VertexPosition(I1);
			const FVector3f P2 = PositionBuffer.VertexPosition(I2);
			const FVector3f FaceNormal = FVector3f::CrossProduct(P1 - P0, P2 - P0);
			const FVector3f VertexNormalSum =
				FVector3f(TangentBuffer.VertexTangentZ(I0)) +
				FVector3f(TangentBuffer.VertexTangentZ(I1)) +
				FVector3f(TangentBuffer.VertexTangentZ(I2));
			Accum += FVector3f::DotProduct(FaceNormal.GetSafeNormal(), VertexNormalSum);
		}
		return (Accum >= 0.0) ? 1.0f : -1.0f;
	}
}

int32 MilkyBody::FCPUSkinnedMesh::NumIndices() const
{
	return IndexBuffer ? IndexBuffer->Num() : 0;
}

uint32 MilkyBody::FCPUSkinnedMesh::GetIndex(int32 i) const
{
	return IndexBuffer->Get(i);
}

bool MilkyBody::BuildCPUSkinnedMesh(USkinnedMeshComponent* Comp, FCPUSkinnedMesh& Out)
{
	SCOPE_CYCLE_COUNTER(STAT_MilkyBody_CPUSkinning);
	using namespace MilkyBodyCPUSkinningPrivate;

	Out = FCPUSkinnedMesh();
	if (!Comp || !Comp->GetSkinnedAsset())
	{
		return false;
	}

	FSkeletalMeshRenderData* RenderData = Comp->GetSkeletalMeshRenderData();
	const int32 LODIndex = Comp->MeshObject ? Comp->MeshObject->GetLOD() : Comp->GetPredictedLODLevel();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return false;
	}

	FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (!SkinWeightBuffer || !IndexBuffer || IndexBuffer->Num() == 0)
	{
		return false;
	}
	if (!HasCPUReadableData(Comp, LODData, LODIndex))
	{
		return false;
	}

	TArray<FMatrix44f> RefToLocals;
	Comp->CacheRefToLocalMatrices(RefToLocals);
	USkinnedMeshComponent::ComputeSkinnedPositions(Comp, Out.Positions, RefToLocals, LODData, *SkinWeightBuffer);
	if (Out.Positions.Num() == 0)
	{
		return false;
	}

	const TPair<FObjectKey, int32> WindingKey(FObjectKey(Comp->GetSkinnedAsset()), LODIndex);
	if (const float* CachedSign = GWindingSignCache.Find(WindingKey))
	{
		Out.WindingSign = *CachedSign;
	}
	else
	{
		Out.WindingSign = ComputeWindingSign(LODData, *IndexBuffer);
		GWindingSignCache.Add(WindingKey, Out.WindingSign);
	}

	Out.IndexBuffer = IndexBuffer;
	Out.ComponentToWorld = Comp->GetComponentTransform();
	Out.LODIndex = LODIndex;
	return true;
}

void MilkyBody::GatherSkinnedMeshesInWorld(UWorld* World, bool bRequireMilkyBody, TArray<USkinnedMeshComponent*>& Out)
{
	Out.Reset();
	if (!World)
	{
		return;
	}

	for (TObjectIterator<USkinnedMeshComponent> It; It; ++It)
	{
		USkinnedMeshComponent* Comp = *It;
		if (!IsValid(Comp)
			|| Comp->GetWorld() != World
			|| !Comp->GetSkinnedAsset()
			|| !Comp->IsRegistered()
			|| !Comp->IsVisible())
		{
			continue;
		}
		if (bRequireMilkyBody && !Cast<UMilkyBodyDeformerInstance>(Comp->GetMeshDeformerInstance()))
		{
			continue;
		}
		Out.Add(Comp);
	}
}
