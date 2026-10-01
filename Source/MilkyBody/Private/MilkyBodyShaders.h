#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHIUtilities.h"
#include "RenderGraphResources.h"

// Maximum number of simultaneous puni dents per dispatch. Shared with the
// shader through the MAX_PUNI_SLOTS define so the two never drift apart.
#define MILKYBODY_MAX_PUNI_SLOTS 32

// Linear Blend Skinning compute shader.
// Produces intermediate skinned positions (whole-LOD buffer) one section at a time.
class FMilkyBodySkinCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMilkyBodySkinCS);
	SHADER_USE_PARAMETER_STRUCT(FMilkyBodySkinCS, FGlobalShader);

public:
	static constexpr uint32 ThreadGroupSize = 64;
	static constexpr int32 MaxPuniSlots = MILKYBODY_MAX_PUNI_SLOTS;

	class FEnableDeformerBones : SHADER_PERMUTATION_BOOL("ENABLE_DEFORMER_BONES");
	class FUnlimitedBoneInfluence : SHADER_PERMUTATION_BOOL("GPUSKIN_UNLIMITED_BONE_INFLUENCE");
	class FBoneIndexUint16 : SHADER_PERMUTATION_BOOL("GPUSKIN_BONE_INDEX_UINT16");
	class FBoneWeightsUint16 : SHADER_PERMUTATION_BOOL("GPUSKIN_BONE_WEIGHTS_UINT16");
	using FPermutationDomain = TShaderPermutationDomain<FEnableDeformerBones, FUnlimitedBoneInfluence, FBoneIndexUint16, FBoneWeightsUint16>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumVertices)
		SHADER_PARAMETER(uint32, NumBoneInfluences)
		SHADER_PARAMETER(uint32, InputWeightStride)
		SHADER_PARAMETER(uint32, InputWeightIndexSize)
		SHADER_PARAMETER(uint32, SectionVertexOffset)
		SHADER_PARAMETER(uint32, SectionNumVertices)
		SHADER_PARAMETER_SRV(Buffer<float>, StaticPositions)
		SHADER_PARAMETER_SRV(Buffer<SNORM float4>, StaticTangents)
		SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)
		SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightStream)
		SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightLookupStream)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutSkinnedPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<SNORM float4>, OutSkinnedTangents)
		// Multi-slot puni state (component space). Hold / impulses / kneading /
		// momi / external contacts are packed into slots 0..NumActivePuniSlots-1.
		SHADER_PARAMETER_ARRAY(FVector4f, PushPosAndRadius, [MILKYBODY_MAX_PUNI_SLOTS])
		SHADER_PARAMETER_ARRAY(FVector4f, PushImpulse, [MILKYBODY_MAX_PUNI_SLOTS])
		SHADER_PARAMETER(uint32, NumActivePuniSlots)
		SHADER_PARAMETER(float, GeometryDepthScale)
		SHADER_PARAMETER(float, ShadingDepthScale)
		// Deformation mask (softness). Per-bone softness is fed through the
		// engine's weight-blend path as a fake bone-matrix buffer whose row0.w
		// holds the softness, so Σ(weight × softness) falls out of the blend.
		SHADER_PARAMETER(uint32, bUseBoneSoftness)
		SHADER_PARAMETER(float, DefaultBoneSoftness)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SoftnessBoneMatrices)
		// Optional vertex-color channel multiplied into the softness.
		// Channel: 0 = off, 1 = R, 2 = G, 3 = B, 4 = A.
		SHADER_PARAMETER_SRV(Buffer<float4>, VertexColors)
		SHADER_PARAMETER(uint32, ColorIndexMask)
		SHADER_PARAMETER(uint32, VertexColorMaskChannel)
		SHADER_PARAMETER(uint32, bInvertVertexColorMask)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("MAX_PUNI_SLOTS"), MaxPuniSlots);
	}
};
