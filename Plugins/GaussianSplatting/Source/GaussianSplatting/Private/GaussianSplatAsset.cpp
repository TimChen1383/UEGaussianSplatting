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

	Ar << SplatCount;
	Ar << BoundingBox;
	Ar << PositionFormat;
	Ar << ColorFormat;
	Ar << SHFormat;
	Ar << SHBands;
	Ar << PositionData;
	Ar << OtherData;
	Ar << SHData;
	Ar << ChunkData;
	Ar << SourceFilePath;
	Ar << ImportQuality;
	Ar << ColorTextureData;
	Ar << ColorTextureWidth;
	Ar << ColorTextureHeight;
}

void UGaussianSplatAsset::PostLoad()
{
	Super::PostLoad();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - SplatCount=%d, ColorTextureData.Num()=%d, Width=%d, Height=%d"),
		SplatCount, ColorTextureData.Num(), ColorTextureWidth, ColorTextureHeight);

	// Recreate the ColorTexture from the stored raw data
	if (ColorTextureData.Num() > 0 && ColorTextureWidth > 0 && ColorTextureHeight > 0)
	{
		CreateColorTextureFromData();
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - ColorTexture recreated successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatAsset::PostLoad - No ColorTextureData to restore (might be old asset format)"));
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

	TotalBytes += PositionData.Num();
	TotalBytes += OtherData.Num();
	TotalBytes += SHData.Num();
	TotalBytes += ChunkData.Num() * sizeof(FGaussianChunkInfo);
	TotalBytes += ColorTextureData.Num();

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

	// Set formats based on quality level
	switch (InQuality)
	{
	case EGaussianQualityLevel::VeryHigh:
		PositionFormat = EGaussianPositionFormat::Float32;
		ColorFormat = EGaussianColorFormat::Float32x4;
		SHFormat = EGaussianSHFormat::Float32;
		break;
	case EGaussianQualityLevel::High:
		PositionFormat = EGaussianPositionFormat::Norm16;
		ColorFormat = EGaussianColorFormat::Float16x4;
		SHFormat = EGaussianSHFormat::Float16;
		break;
	case EGaussianQualityLevel::Medium:
		PositionFormat = EGaussianPositionFormat::Norm16;
		ColorFormat = EGaussianColorFormat::Norm8x4;
		SHFormat = EGaussianSHFormat::Norm11;
		break;
	case EGaussianQualityLevel::Low:
		PositionFormat = EGaussianPositionFormat::Norm11;
		ColorFormat = EGaussianColorFormat::Norm8x4;
		SHFormat = EGaussianSHFormat::Norm6;
		break;
	case EGaussianQualityLevel::VeryLow:
		PositionFormat = EGaussianPositionFormat::Norm6;
		ColorFormat = EGaussianColorFormat::BC7;
		SHFormat = EGaussianSHFormat::Cluster4k;
		break;
	}

	// Calculate bounds first (needed for chunk quantization)
	CalculateBounds(InSplats);
	CalculateChunkBounds(InSplats);

	// Compress data
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

	if (SplatCount == 0 || PositionData.Num() == 0)
	{
		return Positions;
	}

	Positions.SetNum(SplatCount);
	const uint8* DataPtr = PositionData.GetData();
	const int32 BytesPerSplat = GetPositionBytesPerSplat(PositionFormat);

	for (int32 i = 0; i < SplatCount; i++)
	{
		FVector3f Position;

		switch (PositionFormat)
		{
		case EGaussianPositionFormat::Float32:
		{
			const float* FloatPtr = reinterpret_cast<const float*>(DataPtr + i * BytesPerSplat);
			Position.X = FloatPtr[0];
			Position.Y = FloatPtr[1];
			Position.Z = FloatPtr[2];
			break;
		}
		case EGaussianPositionFormat::Norm16:
		{
			// Get chunk bounds for dequantization
			const int32 ChunkIdx = i / GaussianSplattingConstants::SplatsPerChunk;
			if (ChunkIdx < ChunkData.Num())
			{
				const FGaussianChunkInfo& Chunk = ChunkData[ChunkIdx];

				const uint16* ShortPtr = reinterpret_cast<const uint16*>(DataPtr + i * BytesPerSplat);
				float NormX = static_cast<float>(ShortPtr[0]) / 65535.0f;
				float NormY = static_cast<float>(ShortPtr[1]) / 65535.0f;
				float NormZ = static_cast<float>(ShortPtr[2]) / 65535.0f;

				// Dequantize using chunk bounds
				Position.X = Chunk.PosMinMaxX.X + NormX * (Chunk.PosMinMaxX.Y - Chunk.PosMinMaxX.X);
				Position.Y = Chunk.PosMinMaxY.X + NormY * (Chunk.PosMinMaxY.Y - Chunk.PosMinMaxY.X);
				Position.Z = Chunk.PosMinMaxZ.X + NormZ * (Chunk.PosMinMaxZ.Y - Chunk.PosMinMaxZ.X);
			}
			break;
		}
		case EGaussianPositionFormat::Norm11:
		case EGaussianPositionFormat::Norm6:
		{
			// TODO: These formats currently fall back to Float32 in compression
			// Once implemented, add decompression here
			const float* FloatPtr = reinterpret_cast<const float*>(DataPtr + i * 12);
			Position.X = FloatPtr[0];
			Position.Y = FloatPtr[1];
			Position.Z = FloatPtr[2];
			break;
		}
		}

		Positions[i] = FVector(Position);
	}

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
	const int32 BytesPerSplat = GetPositionBytesPerSplat(PositionFormat);
	PositionData.SetNum(SplatCount * BytesPerSplat);

	uint8* DataPtr = PositionData.GetData();

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];

		switch (PositionFormat)
		{
		case EGaussianPositionFormat::Float32:
		{
			float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * BytesPerSplat);
			FloatPtr[0] = Splat.Position.X;
			FloatPtr[1] = Splat.Position.Y;
			FloatPtr[2] = Splat.Position.Z;
			break;
		}
		case EGaussianPositionFormat::Norm16:
		{
			// Get chunk bounds for quantization
			const int32 ChunkIdx = i / GaussianSplattingConstants::SplatsPerChunk;
			const FGaussianChunkInfo& Chunk = ChunkData[ChunkIdx];

			// Normalize to [0, 1] range within chunk
			float NormX = (Chunk.PosMinMaxX.Y - Chunk.PosMinMaxX.X) > SMALL_NUMBER ?
				(Splat.Position.X - Chunk.PosMinMaxX.X) / (Chunk.PosMinMaxX.Y - Chunk.PosMinMaxX.X) : 0.5f;
			float NormY = (Chunk.PosMinMaxY.Y - Chunk.PosMinMaxY.X) > SMALL_NUMBER ?
				(Splat.Position.Y - Chunk.PosMinMaxY.X) / (Chunk.PosMinMaxY.Y - Chunk.PosMinMaxY.X) : 0.5f;
			float NormZ = (Chunk.PosMinMaxZ.Y - Chunk.PosMinMaxZ.X) > SMALL_NUMBER ?
				(Splat.Position.Z - Chunk.PosMinMaxZ.X) / (Chunk.PosMinMaxZ.Y - Chunk.PosMinMaxZ.X) : 0.5f;

			uint16* ShortPtr = reinterpret_cast<uint16*>(DataPtr + i * BytesPerSplat);
			ShortPtr[0] = static_cast<uint16>(FMath::Clamp(NormX * 65535.0f, 0.0f, 65535.0f));
			ShortPtr[1] = static_cast<uint16>(FMath::Clamp(NormY * 65535.0f, 0.0f, 65535.0f));
			ShortPtr[2] = static_cast<uint16>(FMath::Clamp(NormZ * 65535.0f, 0.0f, 65535.0f));
			break;
		}
		case EGaussianPositionFormat::Norm11:
		case EGaussianPositionFormat::Norm6:
		{
			// TODO: Implement packed formats
			// For now, fall back to float32
			float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * 12);
			FloatPtr[0] = Splat.Position.X;
			FloatPtr[1] = Splat.Position.Y;
			FloatPtr[2] = Splat.Position.Z;
			break;
		}
		}
	}
}

void UGaussianSplatAsset::CompressRotationScale(const TArray<FGaussianSplatData>& InSplats)
{
	// Rotation: 4 floats (quaternion) = 16 bytes or packed to 4 bytes (10.10.10.2)
	// Scale: 3 floats = 12 bytes or packed
	// For now, use full precision: 28 bytes per splat

	const int32 BytesPerSplat = 28; // 16 (quat) + 12 (scale)
	OtherData.SetNum(SplatCount * BytesPerSplat);

	uint8* DataPtr = OtherData.GetData();

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
	ColorTextureData.SetNum(NumBytes);

	FFloat16Color* PixelData = reinterpret_cast<FFloat16Color*>(ColorTextureData.GetData());

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
			FVector3f Color = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);

			FFloat16Color& Pixel = PixelData[TexY * ColorTextureWidth + TexX];
			Pixel.R = FFloat16(FMath::Clamp(Color.X, 0.0f, 1.0f));
			Pixel.G = FFloat16(FMath::Clamp(Color.Y, 0.0f, 1.0f));
			Pixel.B = FFloat16(FMath::Clamp(Color.Z, 0.0f, 1.0f));
			Pixel.A = FFloat16(FMath::Clamp(Splat.Opacity, 0.0f, 1.0f));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureData: Stored %d bytes of pixel data"), NumBytes);
}

void UGaussianSplatAsset::CreateColorTextureFromData()
{
	if (ColorTextureData.Num() == 0 || ColorTextureWidth <= 0 || ColorTextureHeight <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateColorTextureFromData: No data to create texture from"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Creating %dx%d texture from stored data"),
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

	// Copy the stored pixel data into the mip's bulk data
	const int32 NumBytes = ColorTextureData.Num();
	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(NumBytes);
	FMemory::Memcpy(MipData, ColorTextureData.GetData(), NumBytes);
	Mip->BulkData.Unlock();

	// Create the GPU resource
	ColorTexture->UpdateResource();

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Texture created and resource updated"));
}

void UGaussianSplatAsset::CompressSH(const TArray<FGaussianSplatData>& InSplats)
{
	if (SHBands == 0)
	{
		// No additional SH data needed (DC stored in color texture)
		SHData.Empty();
		return;
	}

	const int32 NumCoeffs = (SHBands == 1) ? 3 : (SHBands == 2) ? 8 : 15;

	// Always store as Float16 for now (TODO: implement Norm11/Norm6 compression)
	// Calculate actual bytes needed: NumCoeffs * 3 channels * 2 bytes per FFloat16
	const int32 BytesPerSplat = NumCoeffs * 3 * sizeof(FFloat16);
	SHData.SetNum(SplatCount * BytesPerSplat);

	FFloat16* HalfPtr = reinterpret_cast<FFloat16*>(SHData.GetData());

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

	// Update format to reflect actual storage
	SHFormat = EGaussianSHFormat::Float16;
}
