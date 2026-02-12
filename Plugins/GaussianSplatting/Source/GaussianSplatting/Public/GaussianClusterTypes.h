// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianClusterTypes.generated.h"

/**
 * Constants for Gaussian Splatting clustering (Nanite-style optimization)
 */
namespace GaussianClusterConstants
{
	/** Default number of splats per leaf cluster */
	constexpr int32 DefaultSplatsPerCluster = 128;

	/** Maximum children per cluster node (for tree balance) */
	constexpr int32 MaxChildrenPerCluster = 8;

	/** Invalid cluster ID marker */
	constexpr uint32 InvalidClusterID = 0xFFFFFFFF;

	/** Root cluster parent ID marker */
	constexpr uint32 RootParentID = 0xFFFFFFFF;
}

/**
 * Simplified/merged splat data for LOD representation
 * Smaller than FGaussianSplatData - only stores essential rendering data
 */
USTRUCT()
struct FGaussianLODSplat
{
	GENERATED_BODY()

	/** World position */
	UPROPERTY()
	FVector3f Position = FVector3f::ZeroVector;

	/** Orientation quaternion */
	UPROPERTY()
	FQuat4f Rotation = FQuat4f::Identity;

	/** 3D scale factors */
	UPROPERTY()
	FVector3f Scale = FVector3f::OneVector;

	/** Alpha opacity [0,1] */
	UPROPERTY()
	float Opacity = 1.0f;

	/** Base color (RGB, linear) */
	UPROPERTY()
	FVector3f Color = FVector3f(1.0f, 1.0f, 1.0f);

	/** Serialization */
	friend FArchive& operator<<(FArchive& Ar, FGaussianLODSplat& Splat)
	{
		Ar << Splat.Position;
		Ar << Splat.Rotation;
		Ar << Splat.Scale;
		Ar << Splat.Opacity;
		Ar << Splat.Color;
		return Ar;
	}
};

/**
 * Single cluster in the hierarchical structure
 * Represents a group of spatially-local splats with bounding volume
 *
 * Hierarchy structure:
 *   Level 0 (leaves): Original splats grouped into clusters
 *   Level 1+: Parent clusters containing merged/simplified splats
 *   Root: Single cluster covering entire point cloud
 */
USTRUCT()
struct FGaussianCluster
{
	GENERATED_BODY()

	/** Unique identifier for this cluster */
	UPROPERTY()
	uint32 ClusterID = GaussianClusterConstants::InvalidClusterID;

	/** Parent cluster ID (InvalidClusterID for root) */
	UPROPERTY()
	uint32 ParentClusterID = GaussianClusterConstants::RootParentID;

	/** Child cluster IDs (empty for leaf clusters) */
	UPROPERTY()
	TArray<uint32> ChildClusterIDs;

	/** LOD level (0 = finest/leaf, increases toward root) */
	UPROPERTY()
	uint32 LODLevel = 0;

	/** Axis-aligned bounding box minimum */
	UPROPERTY()
	FVector3f BoundsMin = FVector3f::ZeroVector;

	/** Axis-aligned bounding box maximum */
	UPROPERTY()
	FVector3f BoundsMax = FVector3f::ZeroVector;

	/** Bounding sphere center */
	UPROPERTY()
	FVector3f BoundingSphereCenter = FVector3f::ZeroVector;

	/** Bounding sphere radius */
	UPROPERTY()
	float BoundingSphereRadius = 0.0f;

	/** Start index into the splat array for this cluster's splats */
	UPROPERTY()
	uint32 SplatStartIndex = 0;

	/** Number of splats in this cluster (for leaf) or total descendant splats (for parent) */
	UPROPERTY()
	uint32 SplatCount = 0;

	/**
	 * Maximum screen-space error if this cluster's LOD is used instead of children
	 * Measured in world units - projected to screen pixels at runtime
	 * For leaf clusters, this is 0 (no simplification error)
	 */
	UPROPERTY()
	float MaxError = 0.0f;

	/**
	 * Start index into LODSplats array for this cluster's simplified splats
	 * Only valid for non-leaf clusters (LODLevel > 0)
	 */
	UPROPERTY()
	uint32 LODSplatStartIndex = 0;

	/**
	 * Number of LOD splats for this cluster
	 * For leaf clusters, this is 0 (use original splats)
	 */
	UPROPERTY()
	uint32 LODSplatCount = 0;

	/** Check if this is a leaf cluster (no children) */
	bool IsLeaf() const { return ChildClusterIDs.Num() == 0; }

	/** Check if this is the root cluster (no parent) */
	bool IsRoot() const { return ParentClusterID == GaussianClusterConstants::RootParentID; }

	/** Calculate bounding sphere from AABB */
	void ComputeBoundingSphereFromAABB()
	{
		BoundingSphereCenter = (BoundsMin + BoundsMax) * 0.5f;
		BoundingSphereRadius = FVector3f::Dist(BoundingSphereCenter, BoundsMax);
	}

	/** Expand AABB to include a point */
	void ExpandBounds(const FVector3f& Point)
	{
		BoundsMin = FVector3f(
			FMath::Min(BoundsMin.X, Point.X),
			FMath::Min(BoundsMin.Y, Point.Y),
			FMath::Min(BoundsMin.Z, Point.Z)
		);
		BoundsMax = FVector3f(
			FMath::Max(BoundsMax.X, Point.X),
			FMath::Max(BoundsMax.Y, Point.Y),
			FMath::Max(BoundsMax.Z, Point.Z)
		);
	}

	/** Expand AABB to include another cluster's bounds */
	void ExpandBounds(const FGaussianCluster& Other)
	{
		ExpandBounds(Other.BoundsMin);
		ExpandBounds(Other.BoundsMax);
	}

	/** Initialize bounds to inverse extremes (ready for expansion) */
	void ResetBounds()
	{
		BoundsMin = FVector3f(MAX_FLT, MAX_FLT, MAX_FLT);
		BoundsMax = FVector3f(-MAX_FLT, -MAX_FLT, -MAX_FLT);
	}

	/** Serialization */
	friend FArchive& operator<<(FArchive& Ar, FGaussianCluster& Cluster)
	{
		Ar << Cluster.ClusterID;
		Ar << Cluster.ParentClusterID;
		Ar << Cluster.ChildClusterIDs;
		Ar << Cluster.LODLevel;
		Ar << Cluster.BoundsMin;
		Ar << Cluster.BoundsMax;
		Ar << Cluster.BoundingSphereCenter;
		Ar << Cluster.BoundingSphereRadius;
		Ar << Cluster.SplatStartIndex;
		Ar << Cluster.SplatCount;
		Ar << Cluster.MaxError;
		Ar << Cluster.LODSplatStartIndex;
		Ar << Cluster.LODSplatCount;
		return Ar;
	}
};

/**
 * GPU-friendly LOD splat data for upload to structured buffer
 * Matches HLSL struct FGaussianGPULODSplat in GaussianDataTypes.ush
 * Total: 48 bytes
 */
USTRUCT()
struct FGaussianGPULODSplat
{
	GENERATED_BODY()

	/** World position (12 bytes) */
	FVector3f Position;

	/** Scale (12 bytes) */
	FVector3f Scale;

	/** Rotation quaternion (16 bytes) */
	FQuat4f Rotation;

	/** Color RGB + Opacity A packed (4 bytes) */
	uint32 ColorOpacityPacked;

	/** Padding to 48 bytes */
	uint32 Padding;

	FGaussianGPULODSplat()
		: Position(FVector3f::ZeroVector)
		, Scale(FVector3f::OneVector)
		, Rotation(FQuat4f::Identity)
		, ColorOpacityPacked(0xFFFFFFFF)  // White, full opacity
		, Padding(0)
	{
	}

	/** Construct from CPU LOD splat data */
	explicit FGaussianGPULODSplat(const FGaussianLODSplat& Splat)
		: Position(Splat.Position)
		, Scale(Splat.Scale)
		, Rotation(Splat.Rotation)
		, Padding(0)
	{
		// Pack color and opacity into single uint32 (RGBA8)
		uint8 R = static_cast<uint8>(FMath::Clamp(Splat.Color.X * 255.0f, 0.0f, 255.0f));
		uint8 G = static_cast<uint8>(FMath::Clamp(Splat.Color.Y * 255.0f, 0.0f, 255.0f));
		uint8 B = static_cast<uint8>(FMath::Clamp(Splat.Color.Z * 255.0f, 0.0f, 255.0f));
		uint8 A = static_cast<uint8>(FMath::Clamp(Splat.Opacity * 255.0f, 0.0f, 255.0f));
		ColorOpacityPacked = (A << 24) | (B << 16) | (G << 8) | R;
	}
};

/**
 * GPU-friendly cluster data for upload to structured buffer
 * Matches HLSL struct in ClusterData.ush
 * Total: 64 bytes (cache-line aligned)
 */
USTRUCT()
struct FGaussianGPUCluster
{
	GENERATED_BODY()

	/** AABB minimum + splat start index packed */
	FVector3f BoundsMin;
	uint32 SplatStartIndex;

	/** AABB maximum + splat count packed */
	FVector3f BoundsMax;
	uint32 SplatCount;

	/** Bounding sphere (xyz=center, w=radius) */
	FVector4f BoundingSphere;

	/** Parent cluster index */
	uint32 ParentIndex;

	/** LOD level */
	uint32 LODLevel;

	/** Max error for LOD selection */
	float MaxError;

	/** LOD splat start index (for non-leaf clusters) */
	uint32 LODSplatStartIndex;

	/** LOD splat count (0 for leaf clusters) */
	uint32 LODSplatCount;

	/** Padding to 80 bytes (next 16-byte alignment) */
	uint32 Padding[3];

	FGaussianGPUCluster()
		: BoundsMin(FVector3f::ZeroVector)
		, SplatStartIndex(0)
		, BoundsMax(FVector3f::ZeroVector)
		, SplatCount(0)
		, BoundingSphere(FVector4f::Zero())
		, ParentIndex(GaussianClusterConstants::InvalidClusterID)
		, LODLevel(0)
		, MaxError(0.0f)
		, LODSplatStartIndex(0)
		, LODSplatCount(0)
	{
		Padding[0] = Padding[1] = Padding[2] = 0;
	}

	/** Construct from CPU cluster data */
	explicit FGaussianGPUCluster(const FGaussianCluster& Cluster)
		: BoundsMin(Cluster.BoundsMin)
		, SplatStartIndex(Cluster.SplatStartIndex)
		, BoundsMax(Cluster.BoundsMax)
		, SplatCount(Cluster.SplatCount)
		, BoundingSphere(FVector4f(
			Cluster.BoundingSphereCenter.X,
			Cluster.BoundingSphereCenter.Y,
			Cluster.BoundingSphereCenter.Z,
			Cluster.BoundingSphereRadius))
		, ParentIndex(Cluster.ParentClusterID)
		, LODLevel(Cluster.LODLevel)
		, MaxError(Cluster.MaxError)
		, LODSplatStartIndex(Cluster.LODSplatStartIndex)
		, LODSplatCount(Cluster.LODSplatCount)
	{
		Padding[0] = Padding[1] = Padding[2] = 0;
	}
};

/**
 * Complete cluster hierarchy for a Gaussian Splat asset
 * Built during import, used at runtime for culling and LOD selection
 */
USTRUCT()
struct FGaussianClusterHierarchy
{
	GENERATED_BODY()

	/** All clusters in the hierarchy (sorted by LOD level, then by cluster ID) */
	UPROPERTY()
	TArray<FGaussianCluster> Clusters;

	/** Number of LOD levels (0 = leaf only, 1+ = has parent levels) */
	UPROPERTY()
	uint32 NumLODLevels = 0;

	/** Number of splats per leaf cluster (used during build) */
	UPROPERTY()
	uint32 SplatsPerCluster = GaussianClusterConstants::DefaultSplatsPerCluster;

	/** Index of root cluster in Clusters array */
	UPROPERTY()
	uint32 RootClusterIndex = GaussianClusterConstants::InvalidClusterID;

	/** Number of leaf clusters */
	UPROPERTY()
	uint32 NumLeafClusters = 0;

	/** Total number of splats covered by this hierarchy */
	UPROPERTY()
	uint32 TotalSplatCount = 0;

	/**
	 * LOD splats for non-leaf clusters
	 * Each non-leaf cluster has simplified splats representing its children
	 * Indexed by FGaussianCluster::LODSplatStartIndex and LODSplatCount
	 */
	UPROPERTY()
	TArray<FGaussianLODSplat> LODSplats;

	/** Total number of LOD splats */
	UPROPERTY()
	uint32 TotalLODSplatCount = 0;

	/** Check if hierarchy has been built */
	bool IsValid() const
	{
		return Clusters.Num() > 0 && RootClusterIndex != GaussianClusterConstants::InvalidClusterID;
	}

	/** Get cluster by ID (linear search - consider building index map for frequent access) */
	const FGaussianCluster* FindClusterByID(uint32 ClusterID) const
	{
		for (const FGaussianCluster& Cluster : Clusters)
		{
			if (Cluster.ClusterID == ClusterID)
			{
				return &Cluster;
			}
		}
		return nullptr;
	}

	/** Get all leaf clusters */
	void GetLeafClusters(TArray<const FGaussianCluster*>& OutLeafClusters) const
	{
		OutLeafClusters.Reset();
		for (const FGaussianCluster& Cluster : Clusters)
		{
			if (Cluster.IsLeaf())
			{
				OutLeafClusters.Add(&Cluster);
			}
		}
	}

	/** Get clusters at specific LOD level */
	void GetClustersAtLOD(uint32 LODLevel, TArray<const FGaussianCluster*>& OutClusters) const
	{
		OutClusters.Reset();
		for (const FGaussianCluster& Cluster : Clusters)
		{
			if (Cluster.LODLevel == LODLevel)
			{
				OutClusters.Add(&Cluster);
			}
		}
	}

	/** Convert to GPU-friendly format */
	void ToGPUClusters(TArray<FGaussianGPUCluster>& OutGPUClusters) const
	{
		OutGPUClusters.Reset(Clusters.Num());
		for (const FGaussianCluster& Cluster : Clusters)
		{
			OutGPUClusters.Add(FGaussianGPUCluster(Cluster));
		}
	}

	/** Convert LOD splats to GPU-friendly format */
	void ToGPULODSplats(TArray<FGaussianGPULODSplat>& OutGPULODSplats) const
	{
		OutGPULODSplats.Reset(LODSplats.Num());
		for (const FGaussianLODSplat& Splat : LODSplats)
		{
			OutGPULODSplats.Add(FGaussianGPULODSplat(Splat));
		}
	}

	/** Clear all hierarchy data */
	void Reset()
	{
		Clusters.Empty();
		LODSplats.Empty();
		NumLODLevels = 0;
		RootClusterIndex = GaussianClusterConstants::InvalidClusterID;
		NumLeafClusters = 0;
		TotalSplatCount = 0;
		TotalLODSplatCount = 0;
	}

	/** Serialization */
	friend FArchive& operator<<(FArchive& Ar, FGaussianClusterHierarchy& Hierarchy)
	{
		Ar << Hierarchy.Clusters;
		Ar << Hierarchy.NumLODLevels;
		Ar << Hierarchy.SplatsPerCluster;
		Ar << Hierarchy.RootClusterIndex;
		Ar << Hierarchy.NumLeafClusters;
		Ar << Hierarchy.TotalSplatCount;
		Ar << Hierarchy.LODSplats;
		Ar << Hierarchy.TotalLODSplatCount;
		return Ar;
	}
};

/**
 * Helper utilities for cluster operations
 */
namespace GaussianClusterUtils
{
	/**
	 * Encode 3D position to Morton code (Z-order curve) for spatial locality
	 * Used during cluster building to group spatially-close splats
	 */
	inline uint64 EncodeMorton3D(const FVector3f& Position, const FVector3f& BoundsMin, const FVector3f& BoundsMax)
	{
		// Normalize position to [0, 1] range
		FVector3f Normalized = (Position - BoundsMin) / (BoundsMax - BoundsMin + FVector3f(SMALL_NUMBER));

		// Quantize to 21 bits per axis (63 bits total, fits in uint64)
		const uint32 MaxVal = (1 << 21) - 1;
		uint32 X = FMath::Clamp(static_cast<uint32>(Normalized.X * MaxVal), 0u, MaxVal);
		uint32 Y = FMath::Clamp(static_cast<uint32>(Normalized.Y * MaxVal), 0u, MaxVal);
		uint32 Z = FMath::Clamp(static_cast<uint32>(Normalized.Z * MaxVal), 0u, MaxVal);

		// Interleave bits: split each coordinate and interleave
		auto SplitBy3 = [](uint32 A) -> uint64
		{
			uint64 X = A & 0x1fffff; // 21 bits
			X = (X | X << 32) & 0x1f00000000ffff;
			X = (X | X << 16) & 0x1f0000ff0000ff;
			X = (X | X << 8) & 0x100f00f00f00f00f;
			X = (X | X << 4) & 0x10c30c30c30c30c3;
			X = (X | X << 2) & 0x1249249249249249;
			return X;
		};

		return SplitBy3(X) | (SplitBy3(Y) << 1) | (SplitBy3(Z) << 2);
	}

	/**
	 * Calculate screen-space projected error in pixels
	 * Used for LOD selection at runtime
	 */
	inline float CalculateProjectedError(
		float WorldSpaceError,
		float DistanceToCamera,
		float ScreenHeight,
		float VerticalFOV)
	{
		if (DistanceToCamera < SMALL_NUMBER)
		{
			return MAX_FLT; // Very close, use finest LOD
		}

		// Project error to screen space
		// PixelError = (WorldError / Distance) * (ScreenHeight / (2 * tan(FOV/2)))
		float HalfFOVRad = FMath::DegreesToRadians(VerticalFOV * 0.5f);
		float ProjectionScale = ScreenHeight / (2.0f * FMath::Tan(HalfFOVRad));

		return (WorldSpaceError / DistanceToCamera) * ProjectionScale;
	}

	/**
	 * Test if bounding sphere is inside or intersecting view frustum
	 * Returns true if potentially visible
	 */
	inline bool IsSphereInFrustum(
		const FVector3f& SphereCenter,
		float SphereRadius,
		const FMatrix& ViewProjectionMatrix)
	{
		// Transform center to clip space
		FVector4 ClipPos = ViewProjectionMatrix.TransformFVector4(
			FVector4(SphereCenter.X, SphereCenter.Y, SphereCenter.Z, 1.0f));

		// Check against frustum planes with sphere radius expansion
		// This is a conservative test - may return true for some invisible spheres
		float W = ClipPos.W;

		if (W <= 0)
		{
			// Behind camera - check if sphere extends in front
			return SphereRadius > -W;
		}

		// Check each frustum plane
		// Left: X > -W, Right: X < W, Bottom: Y > -W, Top: Y < W, Near: Z > 0, Far: Z < W
		float RadiusInClip = SphereRadius * ViewProjectionMatrix.GetMaximumAxisScale();

		return ClipPos.X > -W - RadiusInClip &&
		       ClipPos.X < W + RadiusInClip &&
		       ClipPos.Y > -W - RadiusInClip &&
		       ClipPos.Y < W + RadiusInClip &&
		       ClipPos.Z > -RadiusInClip &&
		       ClipPos.Z < W + RadiusInClip;
	}
}
