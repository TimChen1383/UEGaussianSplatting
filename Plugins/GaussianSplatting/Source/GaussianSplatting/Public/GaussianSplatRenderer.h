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
		float SplatScale
	);

	/**
	 * Dispatch the view data calculation compute shader
	 * @param bUseLODRendering If true, skip splats covered by parent LOD clusters
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
		bool bHasColorTexture = true,
		bool bUseLODRendering = false
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
	 * Dispatch radix sort for back-to-front ordering
	 */
	static void DispatchRadixSort(
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
		int32 SplatCount
	);

	/**
	 * Dispatch cluster culling compute shader (Nanite-style optimization)
	 * Tests cluster bounding spheres against view frustum
	 * @param bUseLODRendering If true, track unique LOD clusters for later rendering
	 * @return Number of visible clusters (for statistics)
	 */
	static int32 DispatchClusterCulling(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		bool bUseLODRendering = false
	);

	/**
	 * Dispatch LOD view data calculation compute shader
	 * Processes LOD splats for clusters that use simplified representations
	 *
	 * @param LODSplatStartIndex Start index in LOD splat buffer
	 * @param LODSplatCount Number of LOD splats to process
	 * @param OutputStartIndex Start index in ViewDataBuffer (appends after regular splats)
	 */
	static void DispatchCalcLODViewData(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		uint32 LODSplatStartIndex,
		uint32 LODSplatCount,
		uint32 OutputStartIndex,
		float OpacityScale,
		float SplatScale
	);

	/**
	 * Dispatch LOD splat rendering for all selected LOD clusters
	 * Reads the LOD cluster list from the GPU, processes each cluster's LOD splats
	 * DEPRECATED: Use DispatchCalcLODViewDataGPUDriven instead
	 */
	static void DispatchLODSplatRendering(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		float OpacityScale,
		float SplatScale
	);

	/**
	 * Dispatch GPU-driven LOD view data calculation
	 * Processes ALL LOD splats on GPU, rejects non-selected ones - no CPU readback needed
	 */
	static void DispatchCalcLODViewDataGPUDriven(
		FRHICommandListImmediate& RHICmdList,
		const FSceneView& View,
		FGaussianSplatGPUResources* GPUResources,
		const FMatrix& LocalToWorld,
		float OpacityScale,
		float SplatScale
	);

	/**
	 * Dispatch shader to update indirect draw args with LOD splat count
	 */
	static void DispatchUpdateDrawArgs(
		FRHICommandListImmediate& RHICmdList,
		FGaussianSplatGPUResources* GPUResources
	);

private:
	/** Calculate next power of 2 */
	static uint32 NextPowerOfTwo(uint32 Value);

	/**
	 * Extract frustum planes from view-projection matrix
	 * Planes are in world space, normalized with normal pointing inward
	 * Order: Left, Right, Bottom, Top, Near, Far
	 */
	static void ExtractFrustumPlanes(const FMatrix& ViewProjection, FVector4f OutPlanes[6]);
};
