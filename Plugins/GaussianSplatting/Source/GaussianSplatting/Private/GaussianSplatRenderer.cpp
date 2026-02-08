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
		DispatchRadixSort(RHICmdList, GPUResources, DebugSplatCount);

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

	// Camera-static sort skipping: skip entire compute pipeline when nothing has changed
	FMatrix CurrentVP = View.ViewMatrices.GetViewProjectionMatrix();
	bool bCanSkipCompute = GPUResources->bHasCachedSortData &&
		GPUResources->CachedViewProjectionMatrix.Equals(CurrentVP, 0.0f) &&
		GPUResources->CachedLocalToWorld.Equals(LocalToWorld, 0.0f) &&
		GPUResources->CachedOpacityScale == OpacityScale &&
		GPUResources->CachedSplatScale == SplatScale &&
		GPUResources->CachedHasColorTexture == bHasColorTexture;

	if (!bCanSkipCompute)
	{
		// Step 1: Calculate view data for each splat
		DispatchCalcViewData(RHICmdList, View, GPUResources, LocalToWorld, SplatCount, SHOrder, OpacityScale, SplatScale, bHasColorTexture);

		// Step 2: Calculate sort distances
		DispatchCalcDistances(RHICmdList, GPUResources, SplatCount);

		// Step 3: Sort splats back-to-front
		DispatchRadixSort(RHICmdList, GPUResources, SplatCount);

		// Update cache
		GPUResources->CachedViewProjectionMatrix = CurrentVP;
		GPUResources->CachedLocalToWorld = LocalToWorld;
		GPUResources->CachedOpacityScale = OpacityScale;
		GPUResources->CachedSplatScale = SplatScale;
		GPUResources->CachedHasColorTexture = bHasColorTexture;
		GPUResources->bHasCachedSortData = true;
	}

	// Step 4: Draw the splats (always — uses cached buffers when compute is skipped)
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

	// Dispatch only for actual SplatCount — no power-of-2 padding needed for radix sort
	const uint32 ThreadGroupSize = 256;
	const uint32 NumGroups = FMath::DivideAndRoundUp((uint32)SplatCount, ThreadGroupSize);

	SetComputePipelineState(RHICmdList, ComputeShader.GetComputeShader());
	SetShaderParameters(RHICmdList, ComputeShader, ComputeShader.GetComputeShader(), Parameters);
	RHICmdList.DispatchComputeShader(NumGroups, 1, 1);
	UnsetShaderUAVs(RHICmdList, ComputeShader, ComputeShader.GetComputeShader());
}

void FGaussianSplatRenderer::DispatchRadixSort(
	FRHICommandListImmediate& RHICmdList,
	FGaussianSplatGPUResources* GPUResources,
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRadixSort);

	TShaderMapRef<FRadixSortCountCS> CountShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortPrefixSumCS> PrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortDigitPrefixSumCS> DigitPrefixSumShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FRadixSortScatterCS> ScatterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!CountShader.IsValid() || !PrefixSumShader.IsValid() ||
		!DigitPrefixSumShader.IsValid() || !ScatterShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Radix sort shaders not valid"));
		return;
	}

	// Radix sort works with any count — no power-of-2 padding needed
	uint32 SortCount = (uint32)SplatCount;
	uint32 NumTiles = FMath::DivideAndRoundUp(SortCount, 1024u);

	// Distance buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef DistUAVs[2] = {
		GPUResources->SortDistanceBufferUAV,
		GPUResources->SortDistanceBufferAltUAV
	};
	FBufferRHIRef DistBuffers[2] = {
		GPUResources->SortDistanceBuffer,
		GPUResources->SortDistanceBufferAlt
	};

	// Key buffers: [0] = primary, [1] = alt
	FUnorderedAccessViewRHIRef KeyUAVs[2] = {
		GPUResources->SortKeysBufferUAV,
		GPUResources->SortKeysBufferAltUAV
	};
	FBufferRHIRef KeyBuffers[2] = {
		GPUResources->SortKeysBuffer,
		GPUResources->SortKeysBufferAlt
	};

	// Ensure alt buffers are in UAVCompute state
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortDistanceBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->SortKeysBufferAlt, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::Unknown, ERHIAccess::UAVCompute));

	// 4 radix passes: bits 0-7, 8-15, 16-23, 24-31
	for (uint32 Pass = 0; Pass < 4; Pass++)
	{
		uint32 RadixShift = Pass * 8;
		uint32 SrcIdx = Pass & 1;
		uint32 DstIdx = 1 - SrcIdx;

		// --- CountCS: build per-tile histograms ---
		{
			FRadixSortCountCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;

			SetComputePipelineState(RHICmdList, CountShader.GetComputeShader());
			SetShaderParameters(RHICmdList, CountShader, CountShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, CountShader, CountShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- PrefixSumCS: exclusive prefix sum per digit across tiles ---
		{
			FRadixSortPrefixSumCS::FParameters Params;
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.NumTiles = NumTiles;

			SetComputePipelineState(RHICmdList, PrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(256, 1, 1);
			UnsetShaderUAVs(RHICmdList, PrefixSumShader, PrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- DigitPrefixSumCS: exclusive prefix sum across digit totals ---
		{
			FRadixSortDigitPrefixSumCS::FParameters Params;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;

			SetComputePipelineState(RHICmdList, DigitPrefixSumShader.GetComputeShader());
			SetShaderParameters(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(1, 1, 1);
			UnsetShaderUAVs(RHICmdList, DigitPrefixSumShader, DigitPrefixSumShader.GetComputeShader());
		}

		// UAV barrier
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixDigitOffsetBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->RadixHistogramBuffer, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));

		// --- ScatterCS: scatter keys+values to sorted positions ---
		{
			FRadixSortScatterCS::FParameters Params;
			Params.SrcKeys = DistUAVs[SrcIdx];
			Params.SrcVals = KeyUAVs[SrcIdx];
			Params.DstKeys = DistUAVs[DstIdx];
			Params.DstVals = KeyUAVs[DstIdx];
			Params.HistogramBuffer = GPUResources->RadixHistogramBufferUAV;
			Params.DigitOffsetBuffer = GPUResources->RadixDigitOffsetBufferUAV;
			Params.RadixShift = RadixShift;
			Params.Count = SortCount;
			Params.NumTiles = NumTiles;

			SetComputePipelineState(RHICmdList, ScatterShader.GetComputeShader());
			SetShaderParameters(RHICmdList, ScatterShader, ScatterShader.GetComputeShader(), Params);
			RHICmdList.DispatchComputeShader(NumTiles, 1, 1);
			UnsetShaderUAVs(RHICmdList, ScatterShader, ScatterShader.GetComputeShader());
		}

		// UAV barriers between passes
		RHICmdList.Transition(FRHITransitionInfo(DistBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		RHICmdList.Transition(FRHITransitionInfo(KeyBuffers[DstIdx], ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
	}

	// After 4 passes (even count), result is back in primary buffers [0]
	// Transition SortKeysBuffer for rendering
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

	// Transition view data buffer for graphics reads.
	// Use Unknown source: handles both fresh compute (SRVCompute) and cached (SRVGraphics) paths.
	if (!bDebugBypassViewData && !bDebugWorldPositionTest && GPUResources->ViewDataBuffer.IsValid())
	{
		RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::Unknown, ERHIAccess::SRVGraphics));
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
