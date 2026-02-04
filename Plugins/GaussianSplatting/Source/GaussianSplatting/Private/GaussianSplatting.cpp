// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatting.h"
#include "GaussianSplatViewExtension.h"
#include "GaussianSplatRenderer.h"
#include "GaussianSplatSceneProxy.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "SceneViewExtension.h"
#include "Misc/CoreDelegates.h"
#include "Engine/Engine.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "ScreenPass.h"

#define LOCTEXT_NAMESPACE "FGaussianSplattingModule"

// Helper to get the renderer module
static IRendererModule& GetRendererModuleRef()
{
	return FModuleManager::GetModuleChecked<IRendererModule>("Renderer");
}

void FGaussianSplattingModule::StartupModule()
{
	// Register the shader directory so we can use our custom shaders
	FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("GaussianSplatting"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GaussianSplatting"), PluginShaderDir);

	// Create and register the view extension for Gaussian Splat rendering
	ViewExtension = FSceneViewExtensions::NewExtension<FGaussianSplatViewExtension>();

	if (!ViewExtension.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatting: Failed to create ViewExtension!"));
	}

	// Register post-opaque render delegate for rendering
	PostOpaqueRenderDelegateHandle = GetRendererModuleRef().RegisterPostOpaqueRenderDelegate(
		FPostOpaqueRenderDelegate::CreateRaw(this, &FGaussianSplattingModule::OnPostOpaqueRender_RenderThread));

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatting module started. Shader directory: %s"), *PluginShaderDir);
}

void FGaussianSplattingModule::OnPostOpaqueRender_RenderThread(FPostOpaqueRenderParameters& Parameters)
{
	// Get registered proxies from the view extension
	FGaussianSplatViewExtension* Ext = FGaussianSplatViewExtension::Get();
	if (!Ext)
	{
		return;
	}

	// Access the parameters - UE5.6 uses FRDGBuilder
	if (!Parameters.GraphBuilder || !Parameters.View)
	{
		return;
	}

	FRDGBuilder& GraphBuilder = *Parameters.GraphBuilder;
	const FViewInfo& View = *Parameters.View;

	// Get registered proxies
	TArray<FGaussianSplatSceneProxy*> Proxies;
	Ext->GetRegisteredProxies(Proxies);

	// Debug logging
	static int32 PostOpaqueLogCounter = 0;
	bool bLogThisFrame = (PostOpaqueLogCounter < 5) || (PostOpaqueLogCounter % 60 == 0);
	PostOpaqueLogCounter++;

	if (bLogThisFrame)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnPostOpaqueRender_RenderThread: Proxies.Num()=%d"), Proxies.Num());
	}

	if (Proxies.Num() == 0)
	{
		return;
	}

	// Store view pointer for lambda capture (FViewInfo inherits from FSceneView)
	const FSceneView* SceneView = reinterpret_cast<const FSceneView*>(Parameters.View);

	// Get the color texture from parameters - we need to render to this
	FRDGTexture* ColorTexture = Parameters.ColorTexture;
	FRDGTexture* DepthTexture = Parameters.DepthTexture;

	if (!ColorTexture)
	{
		UE_LOG(LogTemp, Warning, TEXT("OnPostOpaqueRender: No ColorTexture available!"));
		return;
	}

	// Create render pass parameters with the scene color as render target
	FRenderTargetParameters* PassParameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(ColorTexture, ERenderTargetLoadAction::ELoad);
	if (DepthTexture)
	{
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			DepthTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthRead_StencilRead
		);
	}

	// Add a render pass for Gaussian splats with proper render target binding
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GaussianSplatRendering"),
		PassParameters,
		ERDGPassFlags::Raster,
		[SceneView, Proxies, bLogThisFrame](FRHICommandListImmediate& RHICmdList)
		{
			if (!SceneView)
			{
				return;
			}

			SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatRendering);

			for (FGaussianSplatSceneProxy* Proxy : Proxies)
			{
				if (!Proxy)
				{
					continue;
				}

				// Check if proxy is shown in this view (respects visibility flags)
				if (!Proxy->IsShown(SceneView))
				{
					continue;
				}

				// Try to initialize color texture SRV if not already done (deferred init)
				Proxy->TryInitializeColorTexture(RHICmdList);

				FGaussianSplatGPUResources* GPUResources = Proxy->GetGPUResources();
				bool bDebugBypass = Proxy->IsDebugBypassViewData();
				bool bDebugFixedSize = Proxy->IsDebugFixedSizeQuads();
				bool bDebugWorldPos = Proxy->IsDebugWorldPositionTest();

				// Debug logging
				if (bLogThisFrame)
				{
					bool bHasGPURes = GPUResources != nullptr;
					bool bIsValid = GPUResources ? GPUResources->IsValid() : false;
					bool bHasColorTex = GPUResources ? GPUResources->ColorTextureSRV.IsValid() : false;
					bool bHasIndexBuf = GPUResources ? GPUResources->IndexBuffer.IsValid() : false;
					bool bHasViewDataBuf = GPUResources ? GPUResources->ViewDataBuffer.IsValid() : false;
					int32 SplatCount = GPUResources ? GPUResources->GetSplatCount() : 0;
					UE_LOG(LogTemp, Warning, TEXT("  PostOpaque Proxy: bDebugBypass=%d, bDebugFixedSize=%d, GPURes=%d, IsValid=%d, ColorTex=%d, IndexBuf=%d, ViewDataBuf=%d, SplatCount=%d"),
						bDebugBypass, bDebugFixedSize, bHasGPURes, bIsValid, bHasColorTex, bHasIndexBuf, bHasViewDataBuf, SplatCount);
				}

				// Validation depends on debug mode:
				// - Normal mode: needs full IsValid() (including ColorTexture)
				// - DebugFixedSize: needs compute buffers but can skip ColorTexture for colors (uses debug colors)
				// - DebugBypass: only needs index buffer (skips compute entirely)
				// - DebugWorldPos: only needs index buffer (renders hardcoded world positions)

				if (bDebugBypass || bDebugWorldPos)
				{
					// Debug bypass/world pos: only need index buffer
					if (!GPUResources || !GPUResources->IndexBuffer.IsValid())
					{
						if (bLogThisFrame)
						{
							UE_LOG(LogTemp, Warning, TEXT("  Debug bypass/worldpos but no index buffer available"));
						}
						continue;
					}
				}
				else if (bDebugFixedSize)
				{
					// Debug fixed size: need compute buffers but ColorTexture is optional
					// We'll use default color if ColorTexture isn't available
					if (!GPUResources || !GPUResources->IndexBuffer.IsValid() ||
						!GPUResources->ViewDataBuffer.IsValid() || GPUResources->GetSplatCount() <= 0)
					{
						if (bLogThisFrame)
						{
							UE_LOG(LogTemp, Warning, TEXT("  Debug fixed size but missing required buffers"));
						}
						continue;
					}
				}
				else
				{
					// Normal mode: require full validation
					if (!GPUResources || !GPUResources->IsValid())
					{
						continue;
					}
				}

				// Skip frustum culling for debug modes (bypass and world pos test)
				if (!bDebugBypass && !bDebugWorldPos)
				{
					// Check visibility - frustum culling
					const FBoxSphereBounds& Bounds = Proxy->GetBounds();
					if (!SceneView->ViewFrustum.IntersectBox(Bounds.Origin, Bounds.BoxExtent))
					{
						continue;
					}
				}

				// Get transform
				FMatrix LocalToWorld = Proxy->GetLocalToWorld();

				// Render this proxy with all parameters including debug modes
				FGaussianSplatRenderer::Render(
					RHICmdList,
					*SceneView,
					GPUResources,
					LocalToWorld,
					Proxy->GetSplatCount(),
					Proxy->GetSHOrder(),
					Proxy->GetOpacityScale(),
					Proxy->GetSplatScale(),
					Proxy->IsDebugFixedSizeQuads(),
					bDebugBypass,
					bDebugWorldPos,
					Proxy->GetDebugQuadSize()
				);
			}
		}
	);
}

void FGaussianSplattingModule::ShutdownModule()
{
	// Unregister post-opaque render delegate
	if (PostOpaqueRenderDelegateHandle.IsValid())
	{
		GetRendererModuleRef().RemovePostOpaqueRenderDelegate(PostOpaqueRenderDelegateHandle);
		PostOpaqueRenderDelegateHandle.Reset();
	}

	// Clear the view extension
	ViewExtension.Reset();
}

FGaussianSplattingModule& FGaussianSplattingModule::Get()
{
	return FModuleManager::LoadModuleChecked<FGaussianSplattingModule>("GaussianSplatting");
}

bool FGaussianSplattingModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded("GaussianSplatting");
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplattingModule, GaussianSplatting)
