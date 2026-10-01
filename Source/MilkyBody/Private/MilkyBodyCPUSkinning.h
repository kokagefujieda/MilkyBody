#pragma once

#include "CoreMinimal.h"

class UWorld;
class USkinnedMeshComponent;
class FRawStaticIndexBuffer16or32Interface;

namespace MilkyBody
{
	// CPU-skinned snapshot of a SkinnedMeshComponent's current LOD, used for
	// polygon-precise traces and fingertip surface snapping.
	struct FCPUSkinnedMesh
	{
		TArray<FVector3f> Positions; // component space
		const FRawStaticIndexBuffer16or32Interface* IndexBuffer = nullptr;
		FTransform ComponentToWorld = FTransform::Identity;
		int32 LODIndex = INDEX_NONE;
		// Multiply Cross(V1 - V0, V2 - V0) by this to get an outward-facing
		// triangle normal (derived once per mesh LOD from the vertex normals).
		float WindingSign = 1.0f;

		int32 NumIndices() const;
		uint32 GetIndex(int32 i) const;
	};

	// Builds the snapshot. Returns false when the data is not readable on the
	// CPU — e.g. a cooked build where the mesh LOD lacks "Allow CPU Access" —
	// and logs a one-time warning per asset in that case.
	bool BuildCPUSkinnedMesh(USkinnedMeshComponent* Comp, FCPUSkinnedMesh& Out);

	// Every registered, visible SkinnedMeshComponent with an asset in World.
	// When bRequireMilkyBody is true only meshes with a MilkyBody deformer
	// instance are returned.
	void GatherSkinnedMeshesInWorld(UWorld* World, bool bRequireMilkyBody, TArray<USkinnedMeshComponent*>& Out);
}
