// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderer.h"
#include "GaussianSplatShaders.h"
#include "GaussianSplatSceneProxy.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "GlobalShader.h"
#include "ShaderParameterUtils.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "RenderCore.h"
#include "CommonRenderResources.h"

FGaussianSplatRenderer::FGaussianSplatRenderer()
{
}

FGaussianSplatRenderer::~FGaussianSplatRenderer()
{
}

uint32 FGaussianSplatRenderer::NextPowerOfTwo(uint32 Value)
{
	Value--;
	Value |= Value >> 1;
	Value |= Value >> 2;
	Value |= Value >> 4;
	Value |= Value >> 8;
	Value |= Value >> 16;
	Value++;
	return Value;
}

void FGaussianSplatRenderer::Render(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 SplatCount,
	int32 SHOrder,
	float OpacityScale,
	float SplatScale,
	bool bDebugFixedSizeQuads,
	bool bDebugBypassViewData,
	bool bDebugWorldPositionTest,
	float DebugQuadSize)
{
	// Debug bypass mode: skip all validation and render a fixed pattern
	if (bDebugBypassViewData)
	{
		// Use a reasonable splat count for debug grid if we don't have real data
		int32 DebugSplatCount = (SplatCount > 0) ? SplatCount : 1000;

		SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDebugBypass);
		DrawSplats(RHICmdList, View, GPUResources, DebugSplatCount, false, true, false, DebugQuadSize, nullptr);
		return;
	}

	// Debug world position test mode: run FULL PIPELINE with debug position data
	// This tests the same CPU→GPU data path as real PLY data
	if (bDebugWorldPositionTest)
	{
		if (!GPUResources || !GPUResources->HasDebugBuffers())
		{
			UE_LOG(LogTemp, Warning, TEXT("Debug world position test: no debug buffers available"));
			return;
		}

		SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDebugWorldPos);

		const int32 DebugSplatCount = 7;

		// Step 1: Calculate view data using DEBUG position buffer (not real PLY data)
		DispatchCalcViewDataDebug(RHICmdList, View, GPUResources, LocalToWorld, DebugSplatCount, SplatScale);

		// Step 2: Calculate sort distances
		DispatchCalcDistances(RHICmdList, GPUResources, DebugSplatCount);

		// Step 3: Sort splats back-to-front
		DispatchSort(RHICmdList, GPUResources, DebugSplatCount);

		// Step 4: Draw the splats using DebugMode 1 (fixed size quads from ViewDataBuffer)
		DrawSplats(RHICmdList, View, GPUResources, DebugSplatCount, true, false, false, DebugQuadSize, nullptr);
		return;
	}

	// Require GPUResources for all non-bypass modes
	if (!GPUResources)
	{
		return;
	}

	if (SplatCount <= 0)
	{
		return;
	}

	// Debug fixed size mode: allow rendering without ColorTexture
	// Normal mode: require full validation including ColorTexture
	if (!bDebugFixedSizeQuads && !GPUResources->IsValid())
	{
		// Normal mode requires ColorTexture for proper colors
		return;
	}

	// For debug fixed size, we need at least the basic buffers
	if (bDebugFixedSizeQuads)
	{
		if (!GPUResources->ViewDataBuffer.IsValid() || !GPUResources->IndexBuffer.IsValid())
		{
			return;
		}
	}

	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRendering);

	// Check if we have a valid ColorTexture for CalcViewData
	bool bHasColorTexture = GPUResources->ColorTextureSRV.IsValid();

	// Step 1: Calculate view data for each splat
	// Pass flag indicating if ColorTexture is available
	DispatchCalcViewData(RHICmdList, View, GPUResources, LocalToWorld, SplatCount, SHOrder, OpacityScale, SplatScale, bHasColorTexture);

	// Step 2: Calculate sort distances
	DispatchCalcDistances(RHICmdList, GPUResources, SplatCount);

	// Step 3: Sort splats back-to-front
	DispatchSort(RHICmdList, GPUResources, SplatCount);

	// Step 4: Draw the splats
	DrawSplats(RHICmdList, View, GPUResources, SplatCount, bDebugFixedSizeQuads, false, false, DebugQuadSize, nullptr);
}

void FGaussianSplatRenderer::DispatchCalcViewData(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 SplatCount,
	int32 SHOrder,
	float OpacityScale,
	float SplatScale,
	bool bHasColorTexture)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewData);

	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition buffers for compute
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;
	Parameters.PositionBuffer = GPUResources->PositionBufferSRV;
	Parameters.OtherDataBuffer = GPUResources->OtherDataBufferSRV;
	Parameters.SHBuffer = GPUResources->SHBufferSRV;
	Parameters.ChunkBuffer = GPUResources->ChunkBufferSRV;
	Parameters.ColorTexture = GPUResources->GetColorTextureSRVOrDummy();  // Uses dummy texture if real one not available
	Parameters.ColorSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferUAV;

	// Matrices
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
	Parameters.WorldToView = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	// Screen info
	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	// Focal length approximation from projection matrix
	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount = SplatCount;
	Parameters.SHOrder = SHOrder;
	Parameters.OpacityScale = OpacityScale;
	Parameters.SplatScale = SplatScale;
	Parameters.ColorTextureSize = FIntPoint(GaussianSplattingConstants::ColorTextureWidth,
		FMath::DivideAndRoundUp(SplatCount, GaussianSplattingConstants::ColorTextureWidth));
	Parameters.PositionFormat = 0;  // Always Float32 (simplified format)
	Parameters.UseDefaultColor = bHasColorTexture ? 0 : 1;  // Use default color if no texture

	// Dispatch compute shader
	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition buffer for next stage
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcViewDataDebug(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 DebugSplatCount,
	float SplatScale)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcViewDataDebug);

	TShaderMapRef<FGaussianSplatCalcViewDataCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcViewDataCS shader not valid"));
		return;
	}

	// Transition buffers for compute
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcViewDataCS::FParameters Parameters;

	// Use DEBUG buffers instead of real PLY data buffers
	Parameters.PositionBuffer = GPUResources->DebugPositionBufferSRV;
	Parameters.OtherDataBuffer = GPUResources->DebugOtherDataBufferSRV;

	// These are not used for debug mode, but shader requires them - use real or dummy
	Parameters.SHBuffer = GPUResources->SHBufferSRV.IsValid() ? GPUResources->SHBufferSRV : GPUResources->DebugOtherDataBufferSRV;
	Parameters.ChunkBuffer = GPUResources->ChunkBufferSRV;
	Parameters.ColorTexture = GPUResources->GetColorTextureSRVOrDummy();
	Parameters.ColorSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferUAV;

	// Matrices
	Parameters.LocalToWorld = FMatrix44f(LocalToWorld);
	Parameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetViewProjectionMatrix());
	Parameters.WorldToView = FMatrix44f(View.ViewMatrices.GetViewMatrix());
	Parameters.CameraPosition = FVector3f(View.ViewMatrices.GetViewOrigin());

	// Screen info
	FIntRect ViewRect = View.UnscaledViewRect;
	Parameters.ScreenSize = FVector2f(ViewRect.Width(), ViewRect.Height());

	// Focal length approximation from projection matrix
	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	Parameters.FocalLength = FVector2f(
		ProjMatrix.M[0][0] * ViewRect.Width() * 0.5f,
		ProjMatrix.M[1][1] * ViewRect.Height() * 0.5f
	);

	Parameters.SplatCount = DebugSplatCount;
	Parameters.SHOrder = 0;  // No SH for debug
	Parameters.OpacityScale = 1.0f;
	Parameters.SplatScale = SplatScale;
	Parameters.ColorTextureSize = FIntPoint(1, 1);  // Dummy
	Parameters.PositionFormat = 0;  // Float32 format for debug buffer
	Parameters.UseDefaultColor = 1;  // Use default white color

	// Dispatch compute shader
	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)DebugSplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

	// Transition buffer for next stage
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute));
}

void FGaussianSplatRenderer::DispatchCalcDistances(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatCalcDistances);

	TShaderMapRef<FGaussianSplatCalcDistancesCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatCalcDistancesCS shader not valid"));
		return;
	}

	// Transition buffers
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	FGaussianSplatCalcDistancesCS::FParameters Parameters;
	Parameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	Parameters.DistanceBuffer = GPUResources->SortDistanceBufferUAV;
	Parameters.KeyBuffer = GPUResources->SortKeysBufferUAV;
	Parameters.SplatCount = SplatCount;

	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
}

void FGaussianSplatRenderer::DispatchSort(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatSort);

	TShaderMapRef<FGaussianSplatBitonicSortCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!ComputeShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianSplatBitonicSortCS shader not valid"));
		return;
	}

	// Pad to power of 2 for bitonic sort
	uint32 PaddedCount = NextPowerOfTwo(SplatCount);

	// Bitonic sort requires log2(N) stages
	uint32 NumStages = FMath::FloorLog2(PaddedCount);

	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp(PaddedCount / 2, ThreadGroupSize);

	for (uint32 Stage = 0; Stage < NumStages; Stage++)
	{
		for (uint32 Pass = 0; Pass <= Stage; Pass++)
		{
			FGaussianSplatBitonicSortCS::FParameters Parameters;
			Parameters.DistanceBuffer = GPUResources->SortDistanceBufferUAV;
			Parameters.KeyBuffer = GPUResources->SortKeysBufferUAV;
			Parameters.Level = Stage - Pass;
			Parameters.LevelMask = (1 << (Stage - Pass)) - 1;
			Parameters.Width = PaddedCount;
			Parameters.Height = 1;

			SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
			RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
			UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());

			// Memory barrier between passes
			RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
			RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		}
	}

	// Transition for rendering
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBuffer, ERHIAccess::UAVCompute, ERHIAccess::SRVGraphics));
}

void FGaussianSplatRenderer::DrawSplats(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount,
	bool bDebugFixedSizeQuads,
	bool bDebugBypassViewData,
	bool bDebugWorldPositionTest,
	float DebugQuadSize,
	const FMatrix* WorldToClip)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDraw);

	// Debug logging to verify this function is being called
	static int32 LogCounter = 0;
	if (LogCounter++ % 60 == 0)  // Log once per second at 60fps
	{
		int32 DebugMode = bDebugWorldPositionTest ? 3 : (bDebugBypassViewData ? 2 : (bDebugFixedSizeQuads ? 1 : 0));
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat DrawSplats: SplatCount=%d, DebugMode=%d, GPUResources=%p"),
			SplatCount, DebugMode, GPUResources);
	}

	// For debug bypass mode, we need at least the index buffer
	if (!GPUResources || !GPUResources->IndexBuffer.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat DrawSplats: No index buffer available"));
		return;
	}

	TShaderMapRef<FGaussianSplatVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSplatPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Gaussian splat render shaders not valid"));
		return;
	}

	// Only transition view data buffer if we're not in bypass/world-pos mode and have valid buffer
	if (!bDebugBypassViewData && !bDebugWorldPositionTest && GPUResources->ViewDataBuffer.IsValid())
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::SRVCompute, ERHIAccess::SRVGraphics));
	}

	// Set up graphics pipeline state
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	// Use different blend modes for debug vs normal rendering
	if (bDebugFixedSizeQuads || bDebugBypassViewData || bDebugWorldPositionTest)
	{
		// Debug mode: simple opaque rendering (no blending) for clear visibility
		GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	}
	else
	{
		// Blend mode: Standard premultiplied alpha "over" for back-to-front compositing
		// result = src + dst * (1 - srcAlpha)
		// This properly attenuates the background behind splats
		GraphicsPSOInit.BlendState = TStaticBlendState<
			CW_RGBA,
			BO_Add, BF_One, BF_InverseSourceAlpha,  // Color: Src + Dst * (1 - SrcAlpha)
			BO_Add, BF_One, BF_InverseSourceAlpha   // Alpha: same
		>::GetRHI();
	}

	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	// Set vertex shader parameters
	// DebugMode: 0=normal, 1=fixed-size quads at ViewDataBuffer positions, 2=bypass ViewDataBuffer entirely, 3=world position test
	FGaussianSplatVS::FParameters VSParameters;
	// For debug bypass mode, these SRVs may be null - the shader won't read them
	VSParameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GPUResources->SortKeysBufferSRV;
	VSParameters.SplatCount = SplatCount;
	VSParameters.DebugMode = bDebugWorldPositionTest ? 3 : (bDebugBypassViewData ? 2 : (bDebugFixedSizeQuads ? 1 : 0));
	VSParameters.DebugSplatSize = DebugQuadSize;
	// Pass WorldToClip matrix for DebugMode 3 (world position test)
	if (WorldToClip)
	{
		VSParameters.DebugWorldToClip = FMatrix44f(*WorldToClip);
	}
	else
	{
		VSParameters.DebugWorldToClip = FMatrix44f::Identity;
	}

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);

	// Pixel shader parameters (debug mode is passed via vertex interpolants)
	FGaussianSplatPS::FParameters PSParameters;
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

	// Draw instanced quads
	// 4 vertices per quad, SplatCount instances
	// Using index buffer: 6 indices per quad (2 triangles)
	RHICmdList.SetStreamSource(0, nullptr, 0);
	RHICmdList.DrawIndexedPrimitive(
		GPUResources->IndexBuffer,
		0,  // BaseVertexIndex
		0,  // FirstInstance
		4,  // NumVertices
		0,  // StartIndex
		2,  // NumPrimitives (2 triangles per quad)
		SplatCount  // NumInstances
	);
}
