// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianDataTypes.h"
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class FGaussianSplatSceneProxy;
class UInstancedStaticMeshComponent;

/**
 * Component for rendering Gaussian Splatting assets in the scene
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), hidecategories = (Collision, Physics, Navigation))
class GAUSSIANSPLATTING_API UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent Interface

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UPrimitiveComponent Interface

	/** Set the Gaussian Splat asset to render */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	void SetSplatAsset(UGaussianSplatAsset* NewAsset);

	/** Get the currently assigned asset */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	UGaussianSplatAsset* GetSplatAsset() const { return SplatAsset; }

	/** Get the number of splats being rendered */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	int32 GetSplatCount() const;

public:
	/** The Gaussian Splat asset to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** Spherical Harmonic order to use for rendering (0-3). Higher = more color detail but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "0", ClampMax = "3"))
	int32 SHOrder = 3;

	/** Sort splats every N frames. 1 = every frame. Higher values reduce GPU cost but may cause artifacts during fast camera movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SortEveryNthFrame = 1;

	/** Global opacity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float OpacityScale = 1.0f;

	/** Scale multiplier for splat sizes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float SplatScale = 1.0f;

	/** Enable frustum culling for better performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance")
	bool bEnableFrustumCulling = true;

	/** Render in wireframe mode (debug) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bWireframe = false;

	/** Show debug points at splat positions using instanced cubes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bShowDebugPoints = false;

	/** Size of debug point cubes in world units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug", meta = (ClampMin = "0.01", ClampMax = "100.0", EditCondition = "bShowDebugPoints"))
	float DebugPointSize = 1.0f;

	/** Maximum number of debug points to display (for performance). Set to 0 for all points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug", meta = (ClampMin = "0", EditCondition = "bShowDebugPoints"))
	int32 MaxDebugPoints = 10000;

	/** Debug mode: Render fixed-size colored quads instead of gaussian splats. Uses ViewDataBuffer positions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bDebugFixedSizeQuads = false;

	/** Debug mode: Bypass ViewDataBuffer entirely and render a grid of quads at fixed screen positions.
	 *  This tests if the draw call and rendering pipeline work at all.
	 *  If this works but bDebugFixedSizeQuads doesn't, the issue is in CalcViewData. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bDebugBypassViewData = false;

	/** Debug mode: Render quads at known world positions (origin + ±X/Y/Z axes) to test matrix transformation.
	 *  WHITE=origin, RED=+X, GREEN=+Y, BLUE=+Z (dark variants for negative).
	 *  Place the component at world origin and observe where quads appear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug")
	bool bDebugWorldPositionTest = false;

	/** Size of debug quads in NDC space (0.0-1.0). 0.01 = small dots, 0.05 = medium squares. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Debug", meta = (ClampMin = "0.001", ClampMax = "0.5", EditCondition = "bDebugFixedSizeQuads"))
	float DebugQuadSize = 0.01f;

	/** Rebuild the debug point visualization */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Debug")
	void RebuildDebugPoints();

protected:
	/** Called when the asset changes */
	void OnAssetChanged();

	/** Mark the render state as dirty */
	void MarkRenderStateDirty();

private:
	/** Cached bounds */
	mutable FBoxSphereBounds CachedBounds;
	mutable bool bBoundsCached = false;

	/** Instanced mesh component for debug point visualization */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> DebugPointsISMC;

	/** Creates the debug points instanced mesh component */
	void CreateDebugPointsComponent();

	/** Destroys the debug points instanced mesh component */
	void DestroyDebugPointsComponent();

	/** Updates the debug point instances based on current settings */
	void UpdateDebugPointInstances();
};
