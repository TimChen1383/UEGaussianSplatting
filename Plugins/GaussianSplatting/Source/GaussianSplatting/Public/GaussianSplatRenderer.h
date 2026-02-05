// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"

class FGaussianSplatSceneProxy;
class FGaussianSplatGPUResources;

/**
 * Handles the rendering of Gaussian Splats
 * Orchestrates compute passes for view calculation, sorting, and final rendering
 */
class GAUSSIANSPLATTING_API FGaussianSplatRenderer
{
public:
	FGaussianSplatRenderer();
	~FGaussianSplatRenderer();

	/**
	 * Render Gaussian splats for a scene proxy
	 * Called from the render thread
	 */
	static void Render(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		bool bDebugFixedSizeQuads = false,
		bool bDebugBypassViewData = false,
		bool bDebugWorldPositionTest = false,
		float DebugQuadSize = 0.01f
	);

	/**
	 * Dispatch the view data calculation compute shader
	 */
	static void DispatchCalcViewData(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 SplatCount,
		int32 SHOrder,
		float OpacityScale,
		float SplatScale,
		bool bHasColorTexture = true
	);

	/**
	 * Dispatch the view data calculation compute shader using DEBUG position buffer
	 * Used for testing the full CPU→GPU pipeline with known test positions
	 */
	static void DispatchCalcViewDataDebug(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		int32 DebugSplatCount,
		float SplatScale
	);

	/**
	 * Dispatch the distance calculation compute shader
	 */
	static void DispatchCalcDistances(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount
	);

	/**
	 * Dispatch bitonic sort for back-to-front ordering
	 */
	static void DispatchSort(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount
	);

	/**
	 * Draw the Gaussian splats
	 */
	static void DrawSplats(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		int32 SplatCount,
		bool bDebugFixedSizeQuads = false,
		bool bDebugBypassViewData = false,
		bool bDebugWorldPositionTest = false,
		float DebugQuadSize = 0.01f,
		const FMatrix* WorldToClip = nullptr
	);

private:
	/** Calculate next power of 2 */
	static uint32 NextPowerOfTwo(uint32 Value);
};
