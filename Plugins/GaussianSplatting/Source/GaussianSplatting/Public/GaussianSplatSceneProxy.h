// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GaussianDataTypes.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHIResources.h"

class UGaussianSplatComponent;
class UGaussianSplatAsset;

/**
 * GPU resources for Gaussian Splatting rendering
 */
class FGaussianSplatGPUResources : public FRenderResource
{
public:
	FGaussianSplatGPUResources();
	virtual ~FGaussianSplatGPUResources();

	/** Initialize resources from asset data */
	void Initialize(UGaussianSplatAsset* Asset);

	/** Check if resources are valid */
	bool IsValid() const { return bInitialized && SplatCount > 0; }

	/** Get number of splats */
	int32 GetSplatCount() const { return SplatCount; }

	//~ Begin FRenderResource Interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	//~ End FRenderResource Interface

public:
	/** Position data buffer (compressed) */
	FBufferRHIRef PositionBuffer;
	FShaderResourceViewRHIRef PositionBufferSRV;

	/** Rotation + Scale data buffer */
	FBufferRHIRef OtherDataBuffer;
	FShaderResourceViewRHIRef OtherDataBufferSRV;

	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** View data buffer (computed per-frame) */
	FBufferRHIRef ViewDataBuffer;
	FUnorderedAccessViewRHIRef ViewDataBufferUAV;
	FShaderResourceViewRHIRef ViewDataBufferSRV;

	/** Sort distance buffer */
	FBufferRHIRef SortDistanceBuffer;
	FUnorderedAccessViewRHIRef SortDistanceBufferUAV;
	FShaderResourceViewRHIRef SortDistanceBufferSRV;

	/** Sort keys buffer (double buffered) */
	FBufferRHIRef SortKeysBuffer;
	FUnorderedAccessViewRHIRef SortKeysBufferUAV;
	FShaderResourceViewRHIRef SortKeysBufferSRV;

	FBufferRHIRef SortKeysBufferAlt;
	FUnorderedAccessViewRHIRef SortKeysBufferAltUAV;
	FShaderResourceViewRHIRef SortKeysBufferAltSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Color texture reference */
	FTextureRHIRef ColorTexture;
	FShaderResourceViewRHIRef ColorTextureSRV;

private:
	/** Create static buffers from asset data */
	void CreateStaticBuffers(FRHICommandListBase& RHICmdList);

	/** Create dynamic buffers for per-frame data */
	void CreateDynamicBuffers(FRHICommandListBase& RHICmdList);

	/** Create index buffer for quad rendering */
	void CreateIndexBuffer(FRHICommandListBase& RHICmdList);

private:
	/** Cached asset data for initialization */
	TArray<uint8> CachedPositionData;
	TArray<uint8> CachedOtherData;
	TArray<uint8> CachedSHData;
	TArray<FGaussianChunkInfo> CachedChunkData;

	int32 SplatCount = 0;
	bool bInitialized = false;
};

/**
 * Scene proxy for rendering Gaussian Splatting
 */
class FGaussianSplatSceneProxy : public FPrimitiveSceneProxy
{
public:
	FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent);
	virtual ~FGaussianSplatSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface
	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	//~ End FPrimitiveSceneProxy Interface

	/** Get GPU resources */
	FGaussianSplatGPUResources* GetGPUResources() const { return GPUResources; }

	/** Get splat count */
	int32 GetSplatCount() const { return SplatCount; }

	/** Get rendering parameters */
	int32 GetSHOrder() const { return SHOrder; }
	float GetOpacityScale() const { return OpacityScale; }
	float GetSplatScale() const { return SplatScale; }
	bool IsWireframe() const { return bWireframe; }

private:
	/** GPU resources */
	FGaussianSplatGPUResources* GPUResources = nullptr;

	/** Cached asset for initialization */
	UGaussianSplatAsset* CachedAsset = nullptr;

	/** Rendering parameters */
	int32 SplatCount = 0;
	int32 SHOrder = 3;
	float OpacityScale = 1.0f;
	float SplatScale = 1.0f;
	bool bWireframe = false;
	bool bEnableFrustumCulling = true;
};
