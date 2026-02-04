// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "GaussianDataTypes.h"

/**
 * Compute shader for calculating view-dependent data for each Gaussian splat
 * This runs per-frame to transform splats to screen space and evaluate spherical harmonics
 */
class FGaussianSplatCalcViewDataCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCalcViewDataCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCalcViewDataCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(ByteAddressBuffer, PositionBuffer)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, OtherDataBuffer)
		SHADER_PARAMETER_SRV(ByteAddressBuffer, SHBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianChunkInfo>, ChunkBuffer)
		SHADER_PARAMETER_SRV(Texture2D, ColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ColorSampler)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FMatrix44f, WorldToClip)
		SHADER_PARAMETER(FMatrix44f, WorldToView)
		SHADER_PARAMETER(FVector3f, CameraPosition)
		SHADER_PARAMETER(FVector2f, ScreenSize)
		SHADER_PARAMETER(FVector2f, FocalLength)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(uint32, SHOrder)
		SHADER_PARAMETER(float, OpacityScale)
		SHADER_PARAMETER(float, SplatScale)
		SHADER_PARAMETER(FIntPoint, ColorTextureSize)
		SHADER_PARAMETER(uint32, PositionFormat)
		SHADER_PARAMETER(uint32, UseDefaultColor)  // 1 = use default color (no texture), 0 = use texture
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};

/**
 * Compute shader for calculating sort distances (depth) for each splat
 */
class FGaussianSplatCalcDistancesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatCalcDistancesCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatCalcDistancesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DistanceBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, KeyBuffer)
		SHADER_PARAMETER(uint32, SplatCount)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};

/**
 * Vertex shader for rendering Gaussian splats as quads
 */
class FGaussianSplatVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(StructuredBuffer<FGaussianSplatViewData>, ViewDataBuffer)
		SHADER_PARAMETER_SRV(StructuredBuffer<uint>, SortKeysBuffer)
		SHADER_PARAMETER(uint32, SplatCount)
		SHADER_PARAMETER(uint32, DebugMode) // 0=normal, 1=fixed-size quads, 2=bypass grid, 3=world pos test
		SHADER_PARAMETER(float, DebugSplatSize) // Size in NDC space when debug mode is enabled
		SHADER_PARAMETER(FMatrix44f, DebugWorldToClip) // For DebugMode 3: WorldToClip matrix
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Pixel shader for rendering Gaussian splats with gaussian falloff
 */
class FGaussianSplatPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Debug mode is passed from vertex shader via interpolants
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * Simple bitonic sort compute shader for sorting splats by depth
 * This is simpler than radix sort but still efficient for GPU
 */
class FGaussianSplatBitonicSortCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianSplatBitonicSortCS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianSplatBitonicSortCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, DistanceBuffer)
		SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, KeyBuffer)
		SHADER_PARAMETER(uint32, Level)
		SHADER_PARAMETER(uint32, LevelMask)
		SHADER_PARAMETER(uint32, Width)
		SHADER_PARAMETER(uint32, Height)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 256);
	}
};
