// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatViewExtension.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatRenderer.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Instance = nullptr;

FGaussianSplatViewExtension::FGaussianSplatViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	Instance = this;
}

FGaussianSplatViewExtension::~FGaussianSplatViewExtension()
{
	if (Instance == this)
	{
		Instance = nullptr;
	}
}

FGaussianSplatViewExtension* FGaussianSplatViewExtension::Get()
{
	return Instance;
}

void FGaussianSplatViewExtension::GetRegisteredProxies(TArray<FGaussianSplatSceneProxy*>& OutProxies) const
{
	FScopeLock Lock(&ProxyLock);
	OutProxies = RegisteredProxies;
}

bool FGaussianSplatViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	FScopeLock Lock(&ProxyLock);
	return RegisteredProxies.Num() > 0;
}

void FGaussianSplatViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::RegisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.AddUnique(Proxy);
	}
}

void FGaussianSplatViewExtension::UnregisterProxy(FGaussianSplatSceneProxy* Proxy)
{
	if (Proxy)
	{
		FScopeLock Lock(&ProxyLock);
		RegisteredProxies.Remove(Proxy);
	}
}

void FGaussianSplatViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
}

void FGaussianSplatViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
}

void FGaussianSplatViewExtension::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	FScopeLock Lock(&ProxyLock);

	if (RegisteredProxies.Num() == 0)
	{
		return;
	}

	// Add a pass to render Gaussian splats
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("GaussianSplatRendering"),
		ERDGPassFlags::Raster | ERDGPassFlags::NeverCull,
		[this, &InView](FRHICommandListImmediate& RHICmdList)
		{
			RenderGaussianSplats_RenderThread(RHICmdList, InView);
		}
	);
}

void FGaussianSplatViewExtension::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessingInputs& Inputs)
{
	// Rendering is done in OnPostOpaqueRender delegate instead
}

void FGaussianSplatViewExtension::RenderGaussianSplats_RenderThread(FRHICommandListImmediate& RHICmdList, const FSceneView& View)
{
	SCOPED_DRAW_EVENT(RHICmdList, GaussianSplatViewExtension);

	// Note: ProxyLock should already be held by caller

	for (FGaussianSplatSceneProxy* Proxy : RegisteredProxies)
	{
		if (!Proxy)
		{
			continue;
		}

		FGaussianSplatGPUResources* GPUResources = Proxy->GetGPUResources();
		if (!GPUResources || !GPUResources->IsValid())
		{
			continue;
		}

		// Check visibility - basic frustum check
		const FBoxSphereBounds& Bounds = Proxy->GetBounds();
		if (!View.ViewFrustum.IntersectBox(Bounds.Origin, Bounds.BoxExtent))
		{
			continue;
		}

		// Get transform
		FMatrix LocalToWorld = Proxy->GetLocalToWorld();

		// Render this proxy
		FGaussianSplatRenderer::Render(
			RHICmdList,
			View,
			GPUResources,
			LocalToWorld,
			Proxy->GetSplatCount(),
			Proxy->GetSHOrder(),
			Proxy->GetOpacityScale(),
			Proxy->GetSplatScale()
		);
	}
}
