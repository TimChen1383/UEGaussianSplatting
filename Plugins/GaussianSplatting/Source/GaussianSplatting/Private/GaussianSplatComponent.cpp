// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

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
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bWireframe) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bDebugFixedSizeQuads) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bDebugBypassViewData) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bDebugWorldPositionTest) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DebugQuadSize))
	{
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bShowDebugPoints) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, DebugPointSize) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxDebugPoints))
	{
		RebuildDebugPoints();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGaussianSplatComponent::OnRegister()
{
	Super::OnRegister();

	if (SplatAsset)
	{
		bBoundsCached = false;
	}

	// Create debug points if enabled
	if (bShowDebugPoints)
	{
		RebuildDebugPoints();
	}
}

void UGaussianSplatComponent::OnUnregister()
{
	DestroyDebugPointsComponent();
	Super::OnUnregister();
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
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

	// Rebuild debug points if enabled
	if (bShowDebugPoints)
	{
		RebuildDebugPoints();
	}
}

void UGaussianSplatComponent::MarkRenderStateDirty()
{
	MarkRenderDynamicDataDirty();

	if (IsRegistered())
	{
		Super::MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::RebuildDebugPoints()
{
	if (bShowDebugPoints)
	{
		CreateDebugPointsComponent();
		UpdateDebugPointInstances();
	}
	else
	{
		DestroyDebugPointsComponent();
	}
}

void UGaussianSplatComponent::CreateDebugPointsComponent()
{
	if (DebugPointsISMC)
	{
		return; // Already exists
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Create the instanced static mesh component
	DebugPointsISMC = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	if (!DebugPointsISMC)
	{
		return;
	}

	// Load a simple cube mesh for debug visualization
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh)
	{
		DebugPointsISMC->SetStaticMesh(CubeMesh);
	}

	// Setup the component
	DebugPointsISMC->SetMobility(EComponentMobility::Movable);
	DebugPointsISMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DebugPointsISMC->SetCastShadow(false);
	DebugPointsISMC->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
	DebugPointsISMC->RegisterComponent();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatComponent: Created debug points ISMC"));
}

void UGaussianSplatComponent::DestroyDebugPointsComponent()
{
	if (DebugPointsISMC)
	{
		DebugPointsISMC->DestroyComponent();
		DebugPointsISMC = nullptr;
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatComponent: Destroyed debug points ISMC"));
	}
}

void UGaussianSplatComponent::UpdateDebugPointInstances()
{
	if (!DebugPointsISMC || !SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	// Clear existing instances
	DebugPointsISMC->ClearInstances();

	// Get decompressed positions from asset
	TArray<FVector> Positions = SplatAsset->GetDecompressedPositions();

	if (Positions.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatComponent: No positions found in asset"));
		return;
	}

	// Determine how many points to show
	int32 NumPointsToShow = Positions.Num();
	if (MaxDebugPoints > 0 && MaxDebugPoints < NumPointsToShow)
	{
		NumPointsToShow = MaxDebugPoints;
	}

	// The cube mesh is 100 units by default, so we need to scale it down
	// DebugPointSize is in world units, cube is 100 units, so scale = DebugPointSize / 100
	const float BaseScale = DebugPointSize / 100.0f;
	const FVector ScaleVector(BaseScale, BaseScale, BaseScale);

	// Reserve instance data
	TArray<FTransform> InstanceTransforms;
	InstanceTransforms.Reserve(NumPointsToShow);

	// Add instances for each point
	for (int32 i = 0; i < NumPointsToShow; i++)
	{
		FTransform InstanceTransform;
		InstanceTransform.SetLocation(Positions[i]);
		InstanceTransform.SetScale3D(ScaleVector);
		InstanceTransforms.Add(InstanceTransform);
	}

	// Batch add all instances
	DebugPointsISMC->AddInstances(InstanceTransforms, false);

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatComponent: Created %d debug point instances (total splats: %d)"),
		NumPointsToShow, Positions.Num());
}
