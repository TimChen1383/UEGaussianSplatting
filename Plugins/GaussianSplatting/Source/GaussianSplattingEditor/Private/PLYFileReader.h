// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.h"

/**
 * PLY file header information
 */
struct FPLYHeader
{
	/** Total number of vertices/splats */
	int32 VertexCount = 0;

	/** Is binary little endian format */
	bool bBinaryLittleEndian = true;

	/** Property names in order */
	TArray<FString> PropertyNames;

	/** Property name to byte offset mapping */
	TMap<FString, int32> PropertyOffsets;

	/** Bytes per vertex */
	int32 VertexStride = 0;

	/** Header end position in file */
	int64 DataOffset = 0;
};

/**
 * Utility class for reading PLY files containing Gaussian Splatting data
 * Supports the standard PLY format from 3D Gaussian Splatting training
 */
class FPLYFileReader
{
public:
	/**
	 * Read PLY file and extract Gaussian splat data
	 * @param FilePath Path to the .ply file
	 * @param OutSplats Output array of splat data
	 * @param OutError Error message if reading failed
	 * @return True if successful
	 */
	static bool ReadPLYFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError);

	/**
	 * Check if a file is a valid PLY file
	 * @param FilePath Path to the file
	 * @return True if the file appears to be a valid PLY file
	 */
	static bool IsValidPLYFile(const FString& FilePath);

private:
	/**
	 * Parse the PLY header to extract format information
	 * @param FileData Raw file data
	 * @param OutHeader Parsed header information
	 * @param OutError Error message if parsing failed
	 * @return True if successful
	 */
	static bool ParseHeader(const TArray<uint8>& FileData, FPLYHeader& OutHeader, FString& OutError);

	/**
	 * Read vertex data from the PLY file
	 * @param FileData Raw file data
	 * @param Header Parsed header information
	 * @param OutSplats Output array of splat data
	 * @param OutError Error message if reading failed
	 * @return True if successful
	 */
	static bool ReadVertexData(const TArray<uint8>& FileData, const FPLYHeader& Header, TArray<FGaussianSplatData>& OutSplats, FString& OutError);

	/**
	 * Linearize splat data from raw PLY values
	 * - Normalizes quaternion
	 * - Applies exp to scale (PLY stores log-scale)
	 * - Applies sigmoid to opacity
	 * @param Splat Splat data to linearize (modified in place)
	 */
	static void LinearizeSplatData(FGaussianSplatData& Splat);

	/**
	 * Get property value from vertex data
	 * @param VertexData Pointer to vertex data
	 * @param Header Header with property offsets
	 * @param PropertyName Name of the property to read
	 * @param DefaultValue Default value if property not found
	 * @return Property value as float
	 */
	static float GetPropertyFloat(const uint8* VertexData, const FPLYHeader& Header, const FString& PropertyName, float DefaultValue = 0.0f);
};
