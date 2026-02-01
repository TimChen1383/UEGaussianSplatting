// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "GaussianDataTypes.h"
#include "GaussianSplatAsset.generated.h"

/**
 * Asset containing Gaussian Splatting data loaded from PLY files
 * Stores compressed splat data optimized for GPU rendering
 */
UCLASS(BlueprintType, hidecategories = Object)
class GAUSSIANSPLATTING_API UGaussianSplatAsset : public UObject
{
	GENERATED_BODY()

public:
	UGaussianSplatAsset();

	//~ Begin UObject Interface
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	/** Get the number of splats in this asset */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	int32 GetSplatCount() const { return SplatCount; }

	/** Get the bounding box of all splats */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	FBox GetBounds() const { return BoundingBox; }

	/** Get estimated memory usage in bytes */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	int64 GetMemoryUsage() const;

	/** Check if asset has valid data */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	bool IsValid() const { return SplatCount > 0 && PositionData.Num() > 0; }

public:
	/** Total number of splats */
	UPROPERTY(VisibleAnywhere, Category = "Info")
	int32 SplatCount = 0;

	/** World-space bounding box of all splats */
	UPROPERTY(VisibleAnywhere, Category = "Info")
	FBox BoundingBox;

	/** Position compression format */
	UPROPERTY(VisibleAnywhere, Category = "Format")
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

	/** Color compression format */
	UPROPERTY(VisibleAnywhere, Category = "Format")
	EGaussianColorFormat ColorFormat = EGaussianColorFormat::Float16x4;

	/** Spherical harmonics compression format */
	UPROPERTY(VisibleAnywhere, Category = "Format")
	EGaussianSHFormat SHFormat = EGaussianSHFormat::Float16;

	/** Number of SH bands stored (0-3) */
	UPROPERTY(VisibleAnywhere, Category = "Format")
	int32 SHBands = 3;

	/** Compressed position data */
	UPROPERTY()
	TArray<uint8> PositionData;

	/** Compressed rotation + scale data */
	UPROPERTY()
	TArray<uint8> OtherData;

	/** Compressed spherical harmonics data */
	UPROPERTY()
	TArray<uint8> SHData;

	/** Chunk quantization info (one per 256 splats) */
	UPROPERTY()
	TArray<FGaussianChunkInfo> ChunkData;

	/** Color texture (Morton-swizzled, 2048 x N) */
	UPROPERTY()
	TObjectPtr<UTexture2D> ColorTexture;

	/** Source file path (for reimport) */
	UPROPERTY(VisibleAnywhere, Category = "Import")
	FString SourceFilePath;

	/** Quality level used during import */
	UPROPERTY(VisibleAnywhere, Category = "Import")
	EGaussianQualityLevel ImportQuality = EGaussianQualityLevel::Medium;

public:
	/**
	 * Initialize asset from raw splat data
	 * @param InSplats Raw splat data from PLY file
	 * @param InQuality Compression quality level
	 */
	void InitializeFromSplatData(const TArray<FGaussianSplatData>& InSplats, EGaussianQualityLevel InQuality);

	/** Get bytes per splat for position data based on format */
	static int32 GetPositionBytesPerSplat(EGaussianPositionFormat Format);

	/** Get bytes per splat for color data based on format */
	static int32 GetColorBytesPerSplat(EGaussianColorFormat Format);

	/** Get bytes per splat for SH data based on format */
	static int32 GetSHBytesPerSplat(EGaussianSHFormat Format, int32 Bands);

private:
	/** Compress and store position data */
	void CompressPositions(const TArray<FGaussianSplatData>& InSplats);

	/** Compress and store rotation/scale data */
	void CompressRotationScale(const TArray<FGaussianSplatData>& InSplats);

	/** Create color texture with Morton swizzling */
	void CreateColorTexture(const TArray<FGaussianSplatData>& InSplats);

	/** Compress and store SH data */
	void CompressSH(const TArray<FGaussianSplatData>& InSplats);

	/** Calculate bounding box from splat data */
	void CalculateBounds(const TArray<FGaussianSplatData>& InSplats);

	/** Calculate chunk quantization bounds */
	void CalculateChunkBounds(const TArray<FGaussianSplatData>& InSplats);
};
