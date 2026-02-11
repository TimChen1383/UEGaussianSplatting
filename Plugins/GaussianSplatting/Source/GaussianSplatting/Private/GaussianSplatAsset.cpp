// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAsset.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

UGaussianSplatAsset::UGaussianSplatAsset()
{
	BoundingBox.Init();
}

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	int32 Version = 1;  // Default to V1 for old assets

	if (Ar.IsSaving())
	{
		// When saving, write magic + version header
		int32 Magic = GAUSSIAN_SPLAT_ASSET_MAGIC;
		Version = GAUSSIAN_SPLAT_ASSET_VERSION;
		Ar << Magic;
		Ar << Version;
	}
	else if (Ar.IsLoading())
	{
		// When loading, check for magic header to detect version
		int32 FirstValue = 0;
		Ar << FirstValue;

		if (FirstValue == GAUSSIAN_SPLAT_ASSET_MAGIC)
		{
			// New format with magic header
			Ar << Version;
		}
		else
		{
			// Old V1 format - FirstValue is actually SplatCount
			Version = 1;
			SplatCount = FirstValue;
		}
	}

	if (Version >= 2)
	{
		// Version 2+: Read/write metadata normally
		Ar << SplatCount;
		Ar << BoundingBox;
		Ar << PositionFormat;
		Ar << ColorFormat;
		Ar << SHFormat;
		Ar << SHBands;
		Ar << SourceFilePath;
		Ar << ImportQuality;
		Ar << ColorTextureWidth;
		Ar << ColorTextureHeight;
		Ar << ChunkData;

		// Use bulk data for large arrays
		// These are stored in separate .ubulk files and can be memory-mapped
		PositionBulkData.Serialize(Ar, this);
		OtherBulkData.Serialize(Ar, this);
		SHBulkData.Serialize(Ar, this);
		ColorTextureBulkData.Serialize(Ar, this);
	}
	else
	{
		// Version 1: Legacy TArray format
		// Note: SplatCount was already read above as FirstValue
		Ar << BoundingBox;
		Ar << PositionFormat;
		Ar << ColorFormat;
		Ar << SHFormat;
		Ar << SHBands;

		TArray<uint8> LegacyPositionData;
		TArray<uint8> LegacyOtherData;
		TArray<uint8> LegacySHData;
		TArray<uint8> LegacyColorTextureData;

		Ar << LegacyPositionData;
		Ar << LegacyOtherData;
		Ar << LegacySHData;
		Ar << ChunkData;
		Ar << SourceFilePath;
		Ar << ImportQuality;
		Ar << LegacyColorTextureData;
		Ar << ColorTextureWidth;
		Ar << ColorTextureHeight;

		if (Ar.IsLoading())
		{
			// Convert legacy data to bulk data
			if (LegacyPositionData.Num() > 0)
			{
				PositionBulkData.Lock(LOCK_READ_WRITE);
				void* Data = PositionBulkData.Realloc(LegacyPositionData.Num());
				FMemory::Memcpy(Data, LegacyPositionData.GetData(), LegacyPositionData.Num());
				PositionBulkData.Unlock();
			}

			if (LegacyOtherData.Num() > 0)
			{
				OtherBulkData.Lock(LOCK_READ_WRITE);
				void* Data = OtherBulkData.Realloc(LegacyOtherData.Num());
				FMemory::Memcpy(Data, LegacyOtherData.GetData(), LegacyOtherData.Num());
				OtherBulkData.Unlock();
			}

			if (LegacySHData.Num() > 0)
			{
				SHBulkData.Lock(LOCK_READ_WRITE);
				void* Data = SHBulkData.Realloc(LegacySHData.Num());
				FMemory::Memcpy(Data, LegacySHData.GetData(), LegacySHData.Num());
				SHBulkData.Unlock();
			}

			if (LegacyColorTextureData.Num() > 0)
			{
				ColorTextureBulkData.Lock(LOCK_READ_WRITE);
				void* Data = ColorTextureBulkData.Realloc(LegacyColorTextureData.Num());
				FMemory::Memcpy(Data, LegacyColorTextureData.GetData(), LegacyColorTextureData.Num());
				ColorTextureBulkData.Unlock();
			}

			UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset: Converted legacy v1 asset to v2 bulk data format"));
		}
	}
}

void UGaussianSplatAsset::PostLoad()
{
	Super::PostLoad();

	const int64 ColorDataSize = ColorTextureBulkData.GetBulkDataSize();
	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - SplatCount=%d, ColorTextureBulkData.Size=%lld, Width=%d, Height=%d"),
		SplatCount, ColorDataSize, ColorTextureWidth, ColorTextureHeight);

	// Recreate the ColorTexture from the stored bulk data
	if (ColorDataSize > 0 && ColorTextureWidth > 0 && ColorTextureHeight > 0)
	{
		CreateColorTextureFromData();
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - ColorTexture recreated successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatAsset::PostLoad - No ColorTextureBulkData to restore (might be old asset format)"));
	}
}

#if WITH_EDITOR
void UGaussianSplatAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

int64 UGaussianSplatAsset::GetMemoryUsage() const
{
	int64 TotalBytes = 0;

	TotalBytes += PositionBulkData.GetBulkDataSize();
	TotalBytes += OtherBulkData.GetBulkDataSize();
	TotalBytes += SHBulkData.GetBulkDataSize();
	TotalBytes += ChunkData.Num() * sizeof(FGaussianChunkInfo);
	TotalBytes += ColorTextureBulkData.GetBulkDataSize();

	if (ColorTexture)
	{
		TotalBytes += ColorTexture->CalcTextureMemorySizeEnum(TMC_ResidentMips);
	}

	return TotalBytes;
}

void UGaussianSplatAsset::InitializeFromSplatData(const TArray<FGaussianSplatData>& InSplats, EGaussianQualityLevel InQuality)
{
	SplatCount = InSplats.Num();
	ImportQuality = InQuality;

	if (SplatCount == 0)
	{
		return;
	}

	// Simplified format: Always use Float32 for positions (most reliable)
	// This avoids complex quantization/dequantization that can cause precision issues
	PositionFormat = EGaussianPositionFormat::Float32;
	ColorFormat = EGaussianColorFormat::Float16x4;  // Good balance for colors
	SHFormat = EGaussianSHFormat::Float16;          // Good balance for SH

	// Calculate bounds (still useful for culling)
	CalculateBounds(InSplats);

	// Store position data (always Float32 now)
	CompressPositions(InSplats);
	CompressRotationScale(InSplats);
	CreateColorTextureData(InSplats);  // Store raw data for serialization
	CreateColorTextureFromData();       // Create the runtime texture
	CompressSH(InSplats);

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset: Initialized with %d splats, memory: %lld bytes"),
		SplatCount, GetMemoryUsage());
}

int32 UGaussianSplatAsset::GetPositionBytesPerSplat(EGaussianPositionFormat Format)
{
	switch (Format)
	{
	case EGaussianPositionFormat::Float32: return 12; // 3 x 4 bytes
	case EGaussianPositionFormat::Norm16:  return 6;  // 3 x 2 bytes
	case EGaussianPositionFormat::Norm11:  return 4;  // 11+10+11 bits packed
	case EGaussianPositionFormat::Norm6:   return 2;  // 6+5+5 bits packed
	default: return 12;
	}
}

int32 UGaussianSplatAsset::GetColorBytesPerSplat(EGaussianColorFormat Format)
{
	switch (Format)
	{
	case EGaussianColorFormat::Float32x4: return 16; // 4 x 4 bytes
	case EGaussianColorFormat::Float16x4: return 8;  // 4 x 2 bytes
	case EGaussianColorFormat::Norm8x4:   return 4;  // 4 x 1 byte
	case EGaussianColorFormat::BC7:       return 1;  // Approximate
	default: return 8;
	}
}

int32 UGaussianSplatAsset::GetSHBytesPerSplat(EGaussianSHFormat Format, int32 Bands)
{
	// Each band adds more coefficients: band0=1, band1=4, band2=9, band3=16 total
	int32 NumCoeffs = 0;
	switch (Bands)
	{
	case 0: NumCoeffs = 0; break;  // DC only (stored in color)
	case 1: NumCoeffs = 3; break;  // 3 coeffs
	case 2: NumCoeffs = 8; break;  // 3 + 5 coeffs
	case 3: NumCoeffs = 15; break; // 3 + 5 + 7 coeffs
	default: NumCoeffs = 15; break;
	}

	// Each coefficient has 3 color channels
	int32 TotalValues = NumCoeffs * 3;

	switch (Format)
	{
	case EGaussianSHFormat::Float32: return TotalValues * 4;
	case EGaussianSHFormat::Float16: return TotalValues * 2;
	case EGaussianSHFormat::Norm11:  return (TotalValues * 11 + 7) / 8; // Approximate
	case EGaussianSHFormat::Norm6:   return (TotalValues * 6 + 7) / 8;  // Approximate
	default: return TotalValues * 2; // Assume Float16
	}
}

void UGaussianSplatAsset::CalculateBounds(const TArray<FGaussianSplatData>& InSplats)
{
	BoundingBox.Init();

	for (const FGaussianSplatData& Splat : InSplats)
	{
		BoundingBox += FVector(Splat.Position);
	}
}

TArray<FVector> UGaussianSplatAsset::GetDecompressedPositions() const
{
	TArray<FVector> Positions;

	const int64 DataSize = PositionBulkData.GetBulkDataSize();
	if (SplatCount == 0 || DataSize == 0)
	{
		return Positions;
	}

	Positions.SetNum(SplatCount);

	// Lock bulk data for reading
	const uint8* DataPtr = static_cast<const uint8*>(PositionBulkData.LockReadOnly());

	// Simplified: Always Float32 format (12 bytes per position)
	const int32 BytesPerSplat = 12;

	for (int32 i = 0; i < SplatCount; i++)
	{
		const float* FloatPtr = reinterpret_cast<const float*>(DataPtr + i * BytesPerSplat);
		Positions[i] = FVector(FloatPtr[0], FloatPtr[1], FloatPtr[2]);
	}

	PositionBulkData.Unlock();

	return Positions;
}

void UGaussianSplatAsset::CalculateChunkBounds(const TArray<FGaussianSplatData>& InSplats)
{
	const int32 NumChunks = FMath::DivideAndRoundUp(SplatCount, GaussianSplattingConstants::SplatsPerChunk);
	ChunkData.SetNum(NumChunks);

	for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ChunkIdx++)
	{
		const int32 StartIdx = ChunkIdx * GaussianSplattingConstants::SplatsPerChunk;
		const int32 EndIdx = FMath::Min(StartIdx + GaussianSplattingConstants::SplatsPerChunk, SplatCount);

		FGaussianChunkInfo& Chunk = ChunkData[ChunkIdx];

		// Initialize with first splat values
		if (StartIdx < InSplats.Num())
		{
			const FGaussianSplatData& First = InSplats[StartIdx];
			Chunk.PosMinMaxX = FVector2f(First.Position.X, First.Position.X);
			Chunk.PosMinMaxY = FVector2f(First.Position.Y, First.Position.Y);
			Chunk.PosMinMaxZ = FVector2f(First.Position.Z, First.Position.Z);
		}

		// Find min/max for this chunk
		for (int32 i = StartIdx; i < EndIdx; i++)
		{
			const FGaussianSplatData& Splat = InSplats[i];

			Chunk.PosMinMaxX.X = FMath::Min(Chunk.PosMinMaxX.X, Splat.Position.X);
			Chunk.PosMinMaxX.Y = FMath::Max(Chunk.PosMinMaxX.Y, Splat.Position.X);
			Chunk.PosMinMaxY.X = FMath::Min(Chunk.PosMinMaxY.X, Splat.Position.Y);
			Chunk.PosMinMaxY.Y = FMath::Max(Chunk.PosMinMaxY.Y, Splat.Position.Y);
			Chunk.PosMinMaxZ.X = FMath::Min(Chunk.PosMinMaxZ.X, Splat.Position.Z);
			Chunk.PosMinMaxZ.Y = FMath::Max(Chunk.PosMinMaxZ.Y, Splat.Position.Z);
		}
	}
}

void UGaussianSplatAsset::CompressPositions(const TArray<FGaussianSplatData>& InSplats)
{
	// Simplified: Always use Float32 format (12 bytes per position)
	const int32 BytesPerSplat = 12;
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	PositionBulkData.Lock(LOCK_READ_WRITE);
	uint8* DataPtr = static_cast<uint8*>(PositionBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * BytesPerSplat);
		FloatPtr[0] = Splat.Position.X;
		FloatPtr[1] = Splat.Position.Y;
		FloatPtr[2] = Splat.Position.Z;
	}

	PositionBulkData.Unlock();

	// Set bulk data flags for optimal storage
	PositionBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
}

void UGaussianSplatAsset::CompressRotationScale(const TArray<FGaussianSplatData>& InSplats)
{
	// Rotation: 4 floats (quaternion) = 16 bytes or packed to 4 bytes (10.10.10.2)
	// Scale: 3 floats = 12 bytes or packed
	// For now, use full precision: 28 bytes per splat

	const int32 BytesPerSplat = 28; // 16 (quat) + 12 (scale)
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	OtherBulkData.Lock(LOCK_READ_WRITE);
	uint8* DataPtr = static_cast<uint8*>(OtherBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * BytesPerSplat);

		// Quaternion (normalized)
		FQuat4f NormQuat = GaussianSplattingUtils::NormalizeQuat(Splat.Rotation);
		FloatPtr[0] = NormQuat.X;
		FloatPtr[1] = NormQuat.Y;
		FloatPtr[2] = NormQuat.Z;
		FloatPtr[3] = NormQuat.W;

		// Scale
		FloatPtr[4] = Splat.Scale.X;
		FloatPtr[5] = Splat.Scale.Y;
		FloatPtr[6] = Splat.Scale.Z;
	}

	OtherBulkData.Unlock();

	// Set bulk data flags for optimal storage
	OtherBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
}

void UGaussianSplatAsset::CreateColorTextureData(const TArray<FGaussianSplatData>& InSplats)
{
	// Store texture dimensions
	ColorTextureWidth = GaussianSplattingConstants::ColorTextureWidth;
	ColorTextureHeight = FMath::DivideAndRoundUp(SplatCount, ColorTextureWidth);

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureData: Creating %dx%d data for %d splats"),
		ColorTextureWidth, ColorTextureHeight, SplatCount);

	// Allocate raw pixel data (FFloat16Color = 8 bytes per pixel: R16G16B16A16)
	const int32 NumBytes = ColorTextureWidth * ColorTextureHeight * sizeof(FFloat16Color);

	// Lock bulk data for writing
	ColorTextureBulkData.Lock(LOCK_READ_WRITE);
	FFloat16Color* PixelData = reinterpret_cast<FFloat16Color*>(ColorTextureBulkData.Realloc(NumBytes));

	// Initialize to black
	FMemory::Memzero(PixelData, NumBytes);

	// Fill with color data using Morton swizzling
	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];

		// Calculate Morton-swizzled texture coordinate
		int32 TexX, TexY;
		GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTextureWidth, TexX, TexY);

		if (TexY < ColorTextureHeight)
		{
			// Convert SH DC to color
			// IMPORTANT: Do NOT clamp RGB - SH coefficients can produce values outside [0,1]
			// Float16 texture format can store HDR values, clamping would cause color errors
			FVector3f Color = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);

			FFloat16Color& Pixel = PixelData[TexY * ColorTextureWidth + TexX];
			Pixel.R = FFloat16(Color.X);  // No clamping - allow HDR values
			Pixel.G = FFloat16(Color.Y);
			Pixel.B = FFloat16(Color.Z);
			Pixel.A = FFloat16(FMath::Clamp(Splat.Opacity, 0.0f, 1.0f));  // Opacity is always [0,1]
		}
	}

	ColorTextureBulkData.Unlock();

	// Set bulk data flags for optimal storage
	ColorTextureBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureData: Stored %d bytes of pixel data"), NumBytes);
}

void UGaussianSplatAsset::CreateColorTextureFromData()
{
	const int64 BulkDataSize = ColorTextureBulkData.GetBulkDataSize();
	if (BulkDataSize == 0 || ColorTextureWidth <= 0 || ColorTextureHeight <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateColorTextureFromData: No data to create texture from"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Creating %dx%d texture from stored bulk data"),
		ColorTextureWidth, ColorTextureHeight);

	// Create texture - use Transient flag since this is recreated at runtime
	ColorTexture = NewObject<UTexture2D>(this, NAME_None, RF_Transient);

	// Initialize platform data
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = ColorTextureWidth;
	PlatformData->SizeY = ColorTextureHeight;
	PlatformData->PixelFormat = PF_FloatRGBA;
	ColorTexture->SetPlatformData(PlatformData);

	// Configure texture settings
	ColorTexture->SRGB = false;
	ColorTexture->CompressionSettings = TC_HDR;
	ColorTexture->Filter = TF_Nearest;
	ColorTexture->AddressX = TA_Clamp;
	ColorTexture->AddressY = TA_Clamp;
	ColorTexture->NeverStream = true;
	ColorTexture->LODGroup = TEXTUREGROUP_Pixels2D;
	ColorTexture->MipGenSettings = TMGS_NoMipmaps;

	// Create mip 0
	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	PlatformData->Mips.Add(Mip);
	Mip->SizeX = ColorTextureWidth;
	Mip->SizeY = ColorTextureHeight;

	// Lock bulk data for reading and copy to mip
	const void* SrcData = ColorTextureBulkData.LockReadOnly();
	const int32 NumBytes = static_cast<int32>(BulkDataSize);

	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(NumBytes);
	FMemory::Memcpy(MipData, SrcData, NumBytes);
	Mip->BulkData.Unlock();

	ColorTextureBulkData.Unlock();

	// Create the GPU resource
	ColorTexture->UpdateResource();

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Texture created and resource updated"));
}

void UGaussianSplatAsset::CompressSH(const TArray<FGaussianSplatData>& InSplats)
{
	if (SHBands == 0)
	{
		// No additional SH data needed (DC stored in color texture)
		// Clear any existing bulk data
		SHBulkData.RemoveBulkData();
		return;
	}

	const int32 NumCoeffs = (SHBands == 1) ? 3 : (SHBands == 2) ? 8 : 15;

	// Always store as Float16 for now (TODO: implement Norm11/Norm6 compression)
	// Calculate actual bytes needed: NumCoeffs * 3 channels * 2 bytes per FFloat16
	const int32 BytesPerSplat = NumCoeffs * 3 * sizeof(FFloat16);
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	SHBulkData.Lock(LOCK_READ_WRITE);
	FFloat16* HalfPtr = reinterpret_cast<FFloat16*>(SHBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];

		for (int32 c = 0; c < NumCoeffs; c++)
		{
			int32 Idx = i * NumCoeffs * 3 + c * 3;
			HalfPtr[Idx + 0] = FFloat16(Splat.SH[c].X);
			HalfPtr[Idx + 1] = FFloat16(Splat.SH[c].Y);
			HalfPtr[Idx + 2] = FFloat16(Splat.SH[c].Z);
		}
	}

	SHBulkData.Unlock();

	// Set bulk data flags for optimal storage
	SHBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	// Update format to reflect actual storage
	SHFormat = EGaussianSHFormat::Float16;
}

void UGaussianSplatAsset::GetPositionData(TArray<uint8>& OutData) const
{
	const int64 DataSize = PositionBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = PositionBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		PositionBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetOtherData(TArray<uint8>& OutData) const
{
	const int64 DataSize = OtherBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = OtherBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		OtherBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetSHData(TArray<uint8>& OutData) const
{
	const int64 DataSize = SHBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = SHBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		SHBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetColorTextureData(TArray<uint8>& OutData) const
{
	const int64 DataSize = ColorTextureBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = ColorTextureBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		ColorTextureBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}
