// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	bUseAsOccluder = false;
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);

	// Enable dynamic rendering
	Mobility = EComponentMobility::Movable;
}

void UGaussianSplatComponent::PostLoad()
{
	Super::PostLoad();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatAsset))
	{
		OnAssetChanged();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SHOrder) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale))
	{
		MarkRenderStateDirty();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGaussianSplatComponent::OnRegister()
{
	UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: OnRegister called!"));
	Super::OnRegister();

	if (SplatAsset)
	{
		bBoundsCached = false;
	}
}

void UGaussianSplatComponent::OnUnregister()
{
	UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: OnUnregister called!"));
	Super::OnUnregister();
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
	UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: CreateSceneProxy called!"));

	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return nullptr;
	}

	// Don't create proxy for preview worlds (Blueprint editor, etc.)
	UWorld* World = GetWorld();
	if (World)
	{
		EWorldType::Type WorldType = World->WorldType;
		if (WorldType == EWorldType::EditorPreview || WorldType == EWorldType::GamePreview)
		{
			return nullptr;
		}
	}

	return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (SplatAsset && SplatAsset->IsValid())
	{
		FBox LocalBox = SplatAsset->GetBounds();

		// Transform to world space
		FBox WorldBox = LocalBox.TransformBy(LocalToWorld);

		return FBoxSphereBounds(WorldBox);
	}

	// Return small default bounds if no asset
	return FBoxSphereBounds(FVector::ZeroVector, FVector(100.0f), 100.0f);
}

void UGaussianSplatComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// Gaussian splatting doesn't use traditional materials
	// But we might add a material for composite pass later
}

void UGaussianSplatComponent::SetSplatAsset(UGaussianSplatAsset* NewAsset)
{
	if (SplatAsset != NewAsset)
	{
		SplatAsset = NewAsset;
		OnAssetChanged();
	}
}

int32 UGaussianSplatComponent::GetSplatCount() const
{
	return SplatAsset ? SplatAsset->GetSplatCount() : 0;
}

void UGaussianSplatComponent::OnAssetChanged()
{
	bBoundsCached = false;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::MarkRenderStateDirty()
{
	UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: MarkRenderStateDirty called!"));

	MarkRenderDynamicDataDirty();

	if (IsRegistered())
	{
		Super::MarkRenderStateDirty();
	}
}
