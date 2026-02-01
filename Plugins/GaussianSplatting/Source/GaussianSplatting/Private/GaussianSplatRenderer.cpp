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
	float SplatScale)
{
	if (!GPUResources || !GPUResources->IsValid() || SplatCount <= 0)
	{
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRendering);

	// Step 1: Calculate view data for each splat
	DispatchCalcViewData(RHICmdList, View, GPUResources, LocalToWorld, SplatCount, SHOrder, OpacityScale, SplatScale);

	// Step 2: Calculate sort distances
	DispatchCalcDistances(RHICmdList, GPUResources, SplatCount);

	// Step 3: Sort splats back-to-front
	DispatchSort(RHICmdList, GPUResources, SplatCount);

	// Step 4: Draw the splats
	DrawSplats(RHICmdList, View, GPUResources, SplatCount);
}

void FGaussianSplatRenderer::DispatchCalcViewData(
	FRHICommandListImmediate& RHICmdList,
	const FSceneView& View,
	FGaussianSplatGPUResources* GPUResources,
	const FMatrix& LocalToWorld,
	int32 SplatCount,
	int32 SHOrder,
	float OpacityScale,
	float SplatScale)
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
	Parameters.ColorTexture = GPUResources->ColorTextureSRV;
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
	Parameters.PositionFormat = 0; // Float32 for now

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
	int32 SplatCount)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatDraw);

	TShaderMapRef<FGaussianSplatVS> VertexShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	TShaderMapRef<FGaussianSplatPS> PixelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!VertexShader.IsValid() || !PixelShader.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Gaussian splat render shaders not valid"));
		return;
	}

	// Transition view data for graphics
	RHICmdList.Transition(FRHITransitionInfo(GPUResources->ViewDataBuffer, ERHIAccess::SRVCompute, ERHIAccess::SRVGraphics));

	// Set up graphics pipeline state
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	// Blend mode: One, InverseSourceAlpha (for correct back-to-front blending with premultiplied alpha)
	GraphicsPSOInit.BlendState = TStaticBlendState<
		CW_RGBA,
		BO_Add, BF_One, BF_InverseSourceAlpha,  // Color: Src + Dst * (1 - SrcAlpha)
		BO_Add, BF_One, BF_InverseSourceAlpha   // Alpha: same
	>::GetRHI();

	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	// Set vertex shader parameters
	FGaussianSplatVS::FParameters VSParameters;
	VSParameters.ViewDataBuffer = GPUResources->ViewDataBufferSRV;
	VSParameters.SortKeysBuffer = GPUResources->SortKeysBufferSRV;
	VSParameters.SplatCount = SplatCount;

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);

	// Pixel shader has no parameters currently
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
