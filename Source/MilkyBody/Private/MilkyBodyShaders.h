#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RHIUtilities.h"
#include "RenderGraphResources.h"

// Linear Blend Skinning compute shader.
// Produces intermediate skinned positions (whole-LOD buffer) one section at a time.
class FMilkyBodySkinCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMilkyBodySkinCS);
	SHADER_USE_PARAMETER_STRUCT(FMilkyBodySkinCS, FGlobalShader);

public:
	static constexpr uint32 ThreadGroupSize = 64;

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
		SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)
		SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightStream)
		SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightLookupStream)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutSkinnedPositions)
		SHADER_PARAMETER(FVector4f, PushPosAndRadius)
		SHADER_PARAMETER(FVector4f, PushImpulse)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
	}
};


