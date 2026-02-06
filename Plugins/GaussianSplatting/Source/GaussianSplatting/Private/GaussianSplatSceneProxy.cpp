// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/Texture2D.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "SceneView.h"
#include "SceneManagement.h"

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatGPUResources

FGaussianSplatGPUResources::FGaussianSplatGPUResources()
{
}

FGaussianSplatGPUResources::~FGaussianSplatGPUResources()
{
}

void FGaussianSplatGPUResources::Initialize(UGaussianSplatAsset* Asset)
{
	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	SplatCount = Asset->GetSplatCount();

	// Store the position format from the asset (critical for shader to read correctly)
	PositionFormat = Asset->PositionFormat;

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatGPUResources::Initialize - PositionFormat: %d (0=Float32, 1=Norm16)"),
		static_cast<int32>(PositionFormat));

	// Cache data for RHI initialization
	CachedPositionData = Asset->PositionData;
	CachedOtherData = Asset->OtherData;
	CachedSHData = Asset->SHData;
	CachedChunkData = Asset->ChunkData;

	// Initialize render resource
	if (!bInitialized)
	{
		InitResource(FRHICommandListImmediate::Get());
		bInitialized = true;
	}
}

void FGaussianSplatGPUResources::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (SplatCount <= 0)
	{
		return;
	}

	CreateStaticBuffers(RHICmdList);
	CreateDynamicBuffers(RHICmdList);
	CreateIndexBuffer(RHICmdList);
}

void FGaussianSplatGPUResources::ReleaseRHI()
{
	PositionBuffer.SafeRelease();
	PositionBufferSRV.SafeRelease();
	OtherDataBuffer.SafeRelease();
	OtherDataBufferSRV.SafeRelease();
	SHBuffer.SafeRelease();
	SHBufferSRV.SafeRelease();
	ChunkBuffer.SafeRelease();
	ChunkBufferSRV.SafeRelease();
	ViewDataBuffer.SafeRelease();
	ViewDataBufferUAV.SafeRelease();
	ViewDataBufferSRV.SafeRelease();
	SortDistanceBuffer.SafeRelease();
	SortDistanceBufferUAV.SafeRelease();
	SortDistanceBufferSRV.SafeRelease();
	SortKeysBuffer.SafeRelease();
	SortKeysBufferUAV.SafeRelease();
	SortKeysBufferSRV.SafeRelease();
	SortKeysBufferAlt.SafeRelease();
	SortKeysBufferAltUAV.SafeRelease();
	SortKeysBufferAltSRV.SafeRelease();
	IndexBuffer.SafeRelease();
	ColorTexture.SafeRelease();
	ColorTextureSRV.SafeRelease();
	DummyWhiteTexture.SafeRelease();
	DummyWhiteTextureSRV.SafeRelease();
	DebugPositionBuffer.SafeRelease();
	DebugPositionBufferSRV.SafeRelease();
	DebugOtherDataBuffer.SafeRelease();
	DebugOtherDataBufferSRV.SafeRelease();

	bInitialized = false;
}

void FGaussianSplatGPUResources::CreateStaticBuffers(FRHICommandListBase& RHICmdList)
{
	// Position buffer
	if (CachedPositionData.Num() > 0)
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianPositionBuffer"),
			CachedPositionData.Num(),
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		PositionBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(PositionBuffer, 0, CachedPositionData.Num(), RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedPositionData.GetData(), CachedPositionData.Num());
		RHICmdList.UnlockBuffer(PositionBuffer);

		PositionBufferSRV = RHICmdList.CreateShaderResourceView(
			PositionBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
	}

	// Other data buffer (rotation + scale)
	if (CachedOtherData.Num() > 0)
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianOtherDataBuffer"),
			CachedOtherData.Num(),
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		OtherDataBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(OtherDataBuffer, 0, CachedOtherData.Num(), RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedOtherData.GetData(), CachedOtherData.Num());
		RHICmdList.UnlockBuffer(OtherDataBuffer);

		OtherDataBufferSRV = RHICmdList.CreateShaderResourceView(
			OtherDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
	}

	// SH buffer
	if (CachedSHData.Num() > 0)
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSHBuffer"),
			CachedSHData.Num(),
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SHBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(SHBuffer, 0, CachedSHData.Num(), RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedSHData.GetData(), CachedSHData.Num());
		RHICmdList.UnlockBuffer(SHBuffer);

		SHBufferSRV = RHICmdList.CreateShaderResourceView(
			SHBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
	}

	// Chunk buffer - Always create at least a dummy buffer for shader binding
	// (Even though we always use Float32 positions now, shader still expects this parameter)
	{
		uint32 ChunkCount = CachedChunkData.Num();
		if (ChunkCount == 0)
		{
			ChunkCount = 1;  // Create dummy entry
		}

		const uint32 ChunkSize = ChunkCount * sizeof(FGaussianChunkInfo);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianChunkBuffer"),
			ChunkSize,
			sizeof(FGaussianChunkInfo),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ChunkBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ChunkBuffer, 0, ChunkSize, RLM_WriteOnly);
		if (CachedChunkData.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedChunkData.GetData(), ChunkSize);
		}
		else
		{
			FMemory::Memzero(Data, ChunkSize);  // Zero-initialize dummy entry
		}
		RHICmdList.UnlockBuffer(ChunkBuffer);

		ChunkBufferSRV = RHICmdList.CreateShaderResourceView(
			ChunkBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianChunkInfo)));
	}

	// Clear cached data
	CachedPositionData.Empty();
	CachedOtherData.Empty();
	CachedSHData.Empty();
	CachedChunkData.Empty();
}

void FGaussianSplatGPUResources::CreateDynamicBuffers(FRHICommandListBase& RHICmdList)
{
	// View data buffer (per-frame computed data)
	{
		const uint32 BufferSize = SplatCount * sizeof(FGaussianSplatViewData);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianViewDataBuffer"),
			BufferSize,
			sizeof(FGaussianSplatViewData),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		ViewDataBuffer = RHICmdList.CreateBuffer(Desc);

		ViewDataBufferUAV = RHICmdList.CreateUnorderedAccessView(
			ViewDataBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianSplatViewData)));
		ViewDataBufferSRV = RHICmdList.CreateShaderResourceView(
			ViewDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianSplatViewData)));
	}

	// Sort distance buffer
	{
		const uint32 BufferSize = SplatCount * sizeof(uint32);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSortDistanceBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SortDistanceBuffer = RHICmdList.CreateBuffer(Desc);

		SortDistanceBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortDistanceBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SortDistanceBufferSRV = RHICmdList.CreateShaderResourceView(
			SortDistanceBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}

	// Sort keys buffer (double buffered for radix sort)
	{
		const uint32 BufferSize = SplatCount * sizeof(uint32);

		FRHIBufferCreateDesc Desc1 = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSortKeysBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SortKeysBuffer = RHICmdList.CreateBuffer(Desc1);
		SortKeysBufferUAV = RHICmdList.CreateUnorderedAccessView(
			SortKeysBuffer, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SortKeysBufferSRV = RHICmdList.CreateShaderResourceView(
			SortKeysBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));

		FRHIBufferCreateDesc Desc2 = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSortKeysBufferAlt"),
			BufferSize,
			sizeof(uint32),
			BUF_UnorderedAccess | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::UAVCompute);
		SortKeysBufferAlt = RHICmdList.CreateBuffer(Desc2);
		SortKeysBufferAltUAV = RHICmdList.CreateUnorderedAccessView(
			SortKeysBufferAlt, FRHIViewDesc::CreateBufferUAV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SortKeysBufferAltSRV = RHICmdList.CreateShaderResourceView(
			SortKeysBufferAlt, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
	}
}

void FGaussianSplatGPUResources::CreateIndexBuffer(FRHICommandListBase& RHICmdList)
{
	// 6 indices per quad (2 triangles): 0,1,2, 1,3,2
	TArray<uint16> Indices = { 0, 1, 2, 1, 3, 2 };

	FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
		TEXT("GaussianSplatIndexBuffer"),
		Indices.Num() * sizeof(uint16),
		sizeof(uint16),
		BUF_Static | BUF_IndexBuffer)
		.SetInitialState(ERHIAccess::VertexOrIndexBuffer);
	IndexBuffer = RHICmdList.CreateBuffer(Desc);

	void* Data = RHICmdList.LockBuffer(IndexBuffer, 0, Indices.Num() * sizeof(uint16), RLM_WriteOnly);
	FMemory::Memcpy(Data, Indices.GetData(), Indices.Num() * sizeof(uint16));
	RHICmdList.UnlockBuffer(IndexBuffer);

	// Create dummy white texture for fallback when ColorTexture isn't available
	CreateDummyWhiteTexture(RHICmdList);

	// Create debug position buffer for world position test mode
	CreateDebugPositionBuffer(RHICmdList);
}

void FGaussianSplatGPUResources::CreateDummyWhiteTexture(FRHICommandListBase& RHICmdList)
{
	// Create a 1x1 white texture
	const FRHITextureCreateDesc TextureDesc =
		FRHITextureCreateDesc::Create2D(TEXT("GaussianSplatDummyWhiteTexture"), 1, 1, PF_R8G8B8A8)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);

	DummyWhiteTexture = RHICreateTexture(TextureDesc);

	// Fill with white color
	uint32 WhitePixel = 0xFFFFFFFF;  // RGBA = (255, 255, 255, 255)
	FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
	RHIUpdateTexture2D(DummyWhiteTexture, 0, Region, sizeof(uint32), (const uint8*)&WhitePixel);

	// Create SRV
	DummyWhiteTextureSRV = RHICmdList.CreateShaderResourceView(
		DummyWhiteTexture,
		FRHIViewDesc::CreateTextureSRV()
			.SetDimension(ETextureDimension::Texture2D));
}

void FGaussianSplatGPUResources::CreateDebugPositionBuffer(FRHICommandListBase& RHICmdList)
{
	// Define 7 test positions in local/world space
	// These match the hardcoded values that were in the shader before
	const int32 DebugSplatCount = 7;

	// Position buffer: 3 floats * 4 bytes = 12 bytes per splat
	TArray<FVector3f> DebugPositions;
	DebugPositions.Add(FVector3f(0.0f, 0.0f, 0.0f));      // Origin - WHITE
	DebugPositions.Add(FVector3f(100.0f, 0.0f, 0.0f));    // +X - RED
	DebugPositions.Add(FVector3f(-100.0f, 0.0f, 0.0f));   // -X - DARK RED
	DebugPositions.Add(FVector3f(0.0f, 100.0f, 0.0f));    // +Y - GREEN
	DebugPositions.Add(FVector3f(0.0f, -100.0f, 0.0f));   // -Y - DARK GREEN
	DebugPositions.Add(FVector3f(0.0f, 0.0f, 100.0f));    // +Z - BLUE
	DebugPositions.Add(FVector3f(0.0f, 0.0f, -100.0f));   // -Z - DARK BLUE

	// Create position buffer
	{
		const uint32 BufferSize = DebugSplatCount * sizeof(FVector3f);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianDebugPositionBuffer"),
			BufferSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		DebugPositionBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(DebugPositionBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, DebugPositions.GetData(), BufferSize);
		RHICmdList.UnlockBuffer(DebugPositionBuffer);

		DebugPositionBufferSRV = RHICmdList.CreateShaderResourceView(
			DebugPositionBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
	}

	// Create "other data" buffer (rotation + scale)
	// OtherData layout: 4 floats rotation (quaternion) + 3 floats scale = 28 bytes per splat
	// Using raw floats to ensure exact binary layout matching shader expectations
	{
		// 7 floats per splat: rotation (x,y,z,w) + scale (x,y,z)
		TArray<float> DebugOtherData;
		DebugOtherData.Reserve(DebugSplatCount * 7);

		for (int32 i = 0; i < DebugSplatCount; ++i)
		{
			// Identity quaternion: (x=0, y=0, z=0, w=1)
			DebugOtherData.Add(0.0f);  // Rotation X
			DebugOtherData.Add(0.0f);  // Rotation Y
			DebugOtherData.Add(0.0f);  // Rotation Z
			DebugOtherData.Add(1.0f);  // Rotation W
			// Unit scale
			DebugOtherData.Add(1.0f);  // Scale X
			DebugOtherData.Add(1.0f);  // Scale Y
			DebugOtherData.Add(1.0f);  // Scale Z
		}

		const uint32 BufferSize = DebugOtherData.Num() * sizeof(float);  // 7 floats * 4 bytes * 7 splats = 196 bytes
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianDebugOtherDataBuffer"),
			BufferSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		DebugOtherDataBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(DebugOtherDataBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, DebugOtherData.GetData(), BufferSize);
		RHICmdList.UnlockBuffer(DebugOtherDataBuffer);

		DebugOtherDataBufferSRV = RHICmdList.CreateShaderResourceView(
			DebugOtherDataBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
	}

	UE_LOG(LogTemp, Log, TEXT("Created debug position buffer with %d test splats"), DebugSplatCount);
}

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatSceneProxy

FGaussianSplatSceneProxy::FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent)
	: FPrimitiveSceneProxy(InComponent)
	, CachedAsset(InComponent->SplatAsset)
	, SplatCount(InComponent->SplatAsset ? InComponent->SplatAsset->GetSplatCount() : 0)
	, SHOrder(InComponent->SHOrder)
	, OpacityScale(InComponent->OpacityScale)
	, SplatScale(InComponent->SplatScale)
	, bWireframe(InComponent->bWireframe)
	, bEnableFrustumCulling(InComponent->bEnableFrustumCulling)
	, bDebugFixedSizeQuads(InComponent->bDebugFixedSizeQuads)
	, bDebugBypassViewData(InComponent->bDebugBypassViewData)
	, bDebugWorldPositionTest(InComponent->bDebugWorldPositionTest)
	, DebugQuadSize(InComponent->DebugQuadSize)
{
	bWillEverBeLit = false;
}

FGaussianSplatSceneProxy::~FGaussianSplatSceneProxy()
{
}

SIZE_T FGaussianSplatSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

uint32 FGaussianSplatSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize();
}

FPrimitiveViewRelevance FGaussianSplatSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = false; // Gaussian splats don't cast shadows (yet)
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = true;
	Result.bUsesLightingChannels = false;
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();

	return Result;
}

void FGaussianSplatSceneProxy::GetDynamicMeshElements(
	const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	// Gaussian splatting uses custom rendering via PostOpaqueRender delegate
	// Only draw debug bounds when wireframe mode is enabled or when selected
	if (!bWireframe && !IsSelected())
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);

			// Draw bounds when selected
			if (IsSelected())
			{
				RenderBounds(PDI, ViewFamily.EngineShowFlags, GetBounds(), true);
			}

			// Draw wireframe box when wireframe mode is enabled
			if (bWireframe)
			{
				const FBoxSphereBounds& ProxyBounds = GetBounds();
				DrawWireBox(PDI, FBox(ProxyBounds.Origin - ProxyBounds.BoxExtent, ProxyBounds.Origin + ProxyBounds.BoxExtent),
					FColor::Cyan, SDPG_World);
			}
		}
	}
}

void FGaussianSplatSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	if (CachedAsset && CachedAsset->IsValid())
	{
		GPUResources = new FGaussianSplatGPUResources();
		GPUResources->Initialize(CachedAsset);

		// Get color texture reference
		if (CachedAsset->ColorTexture)
		{
			// Diagnostic: Check texture state
			FTexturePlatformData* PlatformData = CachedAsset->ColorTexture->GetPlatformData();
			int64 BulkDataSize = 0;
			if (PlatformData && PlatformData->Mips.Num() > 0)
			{
				BulkDataSize = PlatformData->Mips[0].BulkData.GetBulkDataSize();
			}

			UE_LOG(LogTemp, Warning, TEXT("CreateRenderThreadResources: ColorTexture exists, SizeX=%d, SizeY=%d, PlatformData=%p, NumMips=%d, BulkDataSize=%lld"),
				CachedAsset->ColorTexture->GetSizeX(),
				CachedAsset->ColorTexture->GetSizeY(),
				PlatformData,
				PlatformData ? PlatformData->Mips.Num() : 0,
				BulkDataSize);

			// If platform data exists but no resource, try to create it
			FTextureResource* TextureResource = CachedAsset->ColorTexture->GetResource();
			if (!TextureResource && PlatformData && BulkDataSize > 0)
			{
				UE_LOG(LogTemp, Warning, TEXT("CreateRenderThreadResources: Calling UpdateResource() to create texture resource"));
				CachedAsset->ColorTexture->UpdateResource();
				// Re-check after UpdateResource
				TextureResource = CachedAsset->ColorTexture->GetResource();
			}

			if (TextureResource && TextureResource->TextureRHI)
			{
				GPUResources->ColorTexture = TextureResource->TextureRHI;
				GPUResources->ColorTextureSRV = RHICmdList.CreateShaderResourceView(
					GPUResources->ColorTexture,
					FRHIViewDesc::CreateTextureSRV()
						.SetDimension(ETextureDimension::Texture2D));
				UE_LOG(LogTemp, Warning, TEXT("CreateRenderThreadResources: ColorTexture SRV created successfully"));
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("CreateRenderThreadResources: ColorTexture resource not ready yet (Resource=%p, TextureRHI=%p)"),
					TextureResource,
					TextureResource ? TextureResource->TextureRHI.GetReference() : nullptr);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatSceneProxy: ColorTexture is null in asset"));
		}

		// Register with view extension for rendering
		FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
		if (ViewExtension)
		{
			ViewExtension->RegisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatSceneProxy: Registered with ViewExtension"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("GaussianSplatSceneProxy: ViewExtension is NULL! Cannot register for rendering."));
		}
	}
}

void FGaussianSplatSceneProxy::DestroyRenderThreadResources()
{
	// Unregister from view extension
	FGaussianSplatViewExtension* ViewExtension = FGaussianSplatViewExtension::Get();
	if (ViewExtension)
	{
		ViewExtension->UnregisterProxy(const_cast<FGaussianSplatSceneProxy*>(this));
	}

	if (GPUResources)
	{
		GPUResources->ReleaseResource();
		delete GPUResources;
		GPUResources = nullptr;
	}
}

void FGaussianSplatSceneProxy::TryInitializeColorTexture(FRHICommandListBase& RHICmdList)
{
	if (!GPUResources || GPUResources->ColorTextureSRV.IsValid())
	{
		// Already initialized or no resources
		return;
	}

	if (!CachedAsset || !CachedAsset->ColorTexture)
	{
		// Log once why we can't initialize
		static bool bLoggedMissingAsset = false;
		if (!bLoggedMissingAsset)
		{
			UE_LOG(LogTemp, Warning, TEXT("TryInitializeColorTexture: CachedAsset=%p, ColorTexture=%p"),
				CachedAsset, CachedAsset ? CachedAsset->ColorTexture.Get() : nullptr);
			bLoggedMissingAsset = true;
		}
		return;
	}

	FTextureResource* TextureResource = CachedAsset->ColorTexture->GetResource();

	// Log diagnostic info about texture state
	static int32 LogCount = 0;
	if (LogCount < 10) // Only log first 10 attempts
	{
		// Check platform data and mip info
		FTexturePlatformData* PlatformData = CachedAsset->ColorTexture->GetPlatformData();
		int64 BulkDataSize = 0;
		if (PlatformData && PlatformData->Mips.Num() > 0)
		{
			BulkDataSize = PlatformData->Mips[0].BulkData.GetBulkDataSize();
		}

		UE_LOG(LogTemp, Warning, TEXT("TryInitializeColorTexture [%d]: Resource=%p, TextureRHI=%p, SizeX=%d, SizeY=%d, PlatformData=%p, BulkDataSize=%lld"),
			LogCount,
			TextureResource,
			TextureResource ? TextureResource->TextureRHI.GetReference() : nullptr,
			CachedAsset->ColorTexture->GetSizeX(),
			CachedAsset->ColorTexture->GetSizeY(),
			PlatformData,
			BulkDataSize);

		// If resource doesn't exist but platform data does, try to create it
		if (!TextureResource && PlatformData && BulkDataSize > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("TryInitializeColorTexture: Calling UpdateResource() to create texture resource"));
			CachedAsset->ColorTexture->UpdateResource();
		}

		LogCount++;
	}

	if (TextureResource && TextureResource->TextureRHI)
	{
		GPUResources->ColorTexture = TextureResource->TextureRHI;
		GPUResources->ColorTextureSRV = RHICmdList.CreateShaderResourceView(
			GPUResources->ColorTexture,
			FRHIViewDesc::CreateTextureSRV()
				.SetDimension(ETextureDimension::Texture2D));

		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatSceneProxy: ColorTexture SRV initialized (deferred) - SUCCESS!"));
	}
}
