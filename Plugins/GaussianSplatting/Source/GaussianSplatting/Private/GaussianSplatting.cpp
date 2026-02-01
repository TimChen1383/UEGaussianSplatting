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
		[SceneView, Proxies](FRHICommandListImmediate& RHICmdList)
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

				FGaussianSplatGPUResources* GPUResources = Proxy->GetGPUResources();
				if (!GPUResources || !GPUResources->IsValid())
				{
					continue;
				}

				// Check visibility - frustum culling
				const FBoxSphereBounds& Bounds = Proxy->GetBounds();
				if (!SceneView->ViewFrustum.IntersectBox(Bounds.Origin, Bounds.BoxExtent))
				{
					continue;
				}

				// Get transform
				FMatrix LocalToWorld = Proxy->GetLocalToWorld();

				// Render this proxy
				FGaussianSplatRenderer::Render(
					RHICmdList,
					*SceneView,
					GPUResources,
					LocalToWorld,
					Proxy->GetSplatCount(),
					Proxy->GetSHOrder(),
					Proxy->GetOpacityScale(),
					Proxy->GetSplatScale()
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
