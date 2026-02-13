// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianClusterBuilder.h"

bool FGaussianClusterBuilder::BuildClusterHierarchy(
	TArray<FGaussianSplatData>& InOutSplats,
	FGaussianClusterHierarchy& OutHierarchy,
	const FBuildSettings& Settings)
{
	// Reset output
	OutHierarchy.Reset();

	const int32 NumSplats = InOutSplats.Num();
	if (NumSplats == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FGaussianClusterBuilder: No splats to cluster"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("FGaussianClusterBuilder: Building cluster hierarchy for %d splats"), NumSplats);

	// Store settings in hierarchy
	OutHierarchy.SplatsPerCluster = Settings.SplatsPerCluster;
	OutHierarchy.TotalSplatCount = NumSplats;

	// Step 1: Calculate global bounds
	const FBox GlobalBounds = CalculateGlobalBounds(InOutSplats);
	UE_LOG(LogTemp, Log, TEXT("  Global bounds: Min(%s) Max(%s)"),
		*GlobalBounds.Min.ToString(), *GlobalBounds.Max.ToString());

	// Step 2: Sort splats by Morton code for spatial locality
	TArray<int32> SortedIndices;
	SortSplatsByMortonCode(InOutSplats, GlobalBounds, Settings.bReorderSplats, SortedIndices);
	UE_LOG(LogTemp, Log, TEXT("  Sorted splats by Morton code"));

	// Step 3: Create leaf clusters
	TArray<FGaussianCluster> LeafClusters;
	CreateLeafClusters(InOutSplats, Settings.SplatsPerCluster, LeafClusters);
	OutHierarchy.NumLeafClusters = LeafClusters.Num();
	UE_LOG(LogTemp, Log, TEXT("  Created %d leaf clusters"), LeafClusters.Num());

	// Step 4: Build hierarchy bottom-up
	TArray<FGaussianCluster> CurrentLevel = MoveTemp(LeafClusters);
	TArray<FGaussianCluster> AllClusters;
	uint32 CurrentLODLevel = 0;
	uint32 NextClusterID = CurrentLevel.Num();

	// Add leaf clusters to all clusters
	AllClusters.Append(CurrentLevel);

	// Build parent levels until we have a single root
	while (CurrentLevel.Num() > 1)
	{
		CurrentLODLevel++;
		TArray<FGaussianCluster> ParentLevel;

		BuildParentLevel(
			CurrentLevel,
			Settings.MaxChildrenPerCluster,
			CurrentLODLevel,
			NextClusterID,
			ParentLevel);

		UE_LOG(LogTemp, Log, TEXT("  LOD Level %d: %d clusters"), CurrentLODLevel, ParentLevel.Num());

		// Update child clusters in AllClusters with their new ParentClusterID
		// (BuildParentLevel updated CurrentLevel, but AllClusters has old copies)
		for (const FGaussianCluster& UpdatedChild : CurrentLevel)
		{
			// Find and update the corresponding cluster in AllClusters
			for (FGaussianCluster& StoredCluster : AllClusters)
			{
				if (StoredCluster.ClusterID == UpdatedChild.ClusterID)
				{
					StoredCluster.ParentClusterID = UpdatedChild.ParentClusterID;
					break;
				}
			}
		}

		// Add parent clusters to all clusters
		AllClusters.Append(ParentLevel);

		// Move to next level
		CurrentLevel = MoveTemp(ParentLevel);
	}

	// Step 5: Calculate error metrics for all non-leaf clusters
	for (FGaussianCluster& Cluster : AllClusters)
	{
		if (!Cluster.IsLeaf())
		{
			CalculateClusterError(Cluster, AllClusters);
		}
	}

	// Store results
	OutHierarchy.Clusters = MoveTemp(AllClusters);
	OutHierarchy.NumLODLevels = CurrentLODLevel + 1;
	OutHierarchy.RootClusterIndex = OutHierarchy.Clusters.Num() - 1; // Root is last cluster added

	UE_LOG(LogTemp, Log, TEXT("FGaussianClusterBuilder: Hierarchy complete"));
	UE_LOG(LogTemp, Log, TEXT("  Total clusters: %d"), OutHierarchy.Clusters.Num());
	UE_LOG(LogTemp, Log, TEXT("  LOD levels: %d"), OutHierarchy.NumLODLevels);
	UE_LOG(LogTemp, Log, TEXT("  Root cluster index: %d"), OutHierarchy.RootClusterIndex);

	// Step 6: Generate LOD splats for non-leaf clusters
	if (Settings.bGenerateLOD && OutHierarchy.NumLODLevels > 1)
	{
		GenerateLODSplats(InOutSplats, OutHierarchy, Settings.LODReductionRatio);
		UE_LOG(LogTemp, Log, TEXT("  Generated %d LOD splats"), OutHierarchy.TotalLODSplatCount);
	}

	return true;
}

FBox FGaussianClusterBuilder::CalculateGlobalBounds(const TArray<FGaussianSplatData>& Splats)
{
	FBox Bounds(ForceInit);

	for (const FGaussianSplatData& Splat : Splats)
	{
		Bounds += FVector(Splat.Position);
	}

	// Ensure non-zero size bounds
	if (Bounds.GetSize().IsNearlyZero())
	{
		Bounds = Bounds.ExpandBy(1.0f);
	}

	return Bounds;
}

void FGaussianClusterBuilder::SortSplatsByMortonCode(
	TArray<FGaussianSplatData>& InOutSplats,
	const FBox& GlobalBounds,
	bool bReorder,
	TArray<int32>& OutSortedIndices)
{
	const int32 NumSplats = InOutSplats.Num();

	// Create sort entries with Morton codes
	TArray<FSplatSortEntry> SortEntries;
	SortEntries.SetNum(NumSplats);

	const FVector3f BoundsMin(GlobalBounds.Min);
	const FVector3f BoundsMax(GlobalBounds.Max);

	for (int32 i = 0; i < NumSplats; i++)
	{
		SortEntries[i].OriginalIndex = i;
		SortEntries[i].MortonCode = GaussianClusterUtils::EncodeMorton3D(
			InOutSplats[i].Position, BoundsMin, BoundsMax);
	}

	// Sort by Morton code
	SortEntries.Sort();

	// Extract sorted indices
	OutSortedIndices.SetNum(NumSplats);
	for (int32 i = 0; i < NumSplats; i++)
	{
		OutSortedIndices[i] = SortEntries[i].OriginalIndex;
	}

	// Reorder splat array if requested
	if (bReorder)
	{
		TArray<FGaussianSplatData> ReorderedSplats;
		ReorderedSplats.SetNum(NumSplats);

		for (int32 i = 0; i < NumSplats; i++)
		{
			ReorderedSplats[i] = InOutSplats[OutSortedIndices[i]];
		}

		InOutSplats = MoveTemp(ReorderedSplats);

		// Update sorted indices to reflect new order (now identity mapping)
		for (int32 i = 0; i < NumSplats; i++)
		{
			OutSortedIndices[i] = i;
		}
	}
}

void FGaussianClusterBuilder::CreateLeafClusters(
	const TArray<FGaussianSplatData>& Splats,
	int32 SplatsPerCluster,
	TArray<FGaussianCluster>& OutClusters)
{
	const int32 NumSplats = Splats.Num();
	const int32 NumClusters = FMath::DivideAndRoundUp(NumSplats, SplatsPerCluster);

	OutClusters.Reset(NumClusters);

	for (int32 ClusterIdx = 0; ClusterIdx < NumClusters; ClusterIdx++)
	{
		FGaussianCluster Cluster;
		Cluster.ClusterID = static_cast<uint32>(ClusterIdx);
		Cluster.ParentClusterID = GaussianClusterConstants::RootParentID; // Will be set when parent is created
		Cluster.LODLevel = 0; // Leaf level
		Cluster.SplatStartIndex = static_cast<uint32>(ClusterIdx * SplatsPerCluster);
		Cluster.SplatCount = static_cast<uint32>(FMath::Min(SplatsPerCluster, NumSplats - static_cast<int32>(Cluster.SplatStartIndex)));
		Cluster.MaxError = 0.0f; // Leaf clusters have no simplification error

		// Calculate bounds from splats
		CalculateClusterBounds(Cluster, Splats);

		OutClusters.Add(MoveTemp(Cluster));
	}
}

void FGaussianClusterBuilder::BuildParentLevel(
	TArray<FGaussianCluster>& ChildClusters,
	int32 MaxChildrenPerCluster,
	uint32 ParentLODLevel,
	uint32& NextClusterID,
	TArray<FGaussianCluster>& OutParentClusters)
{
	const int32 NumChildren = ChildClusters.Num();
	const int32 NumParents = FMath::DivideAndRoundUp(NumChildren, MaxChildrenPerCluster);

	OutParentClusters.Reset(NumParents);

	for (int32 ParentIdx = 0; ParentIdx < NumParents; ParentIdx++)
	{
		FGaussianCluster Parent;
		Parent.ClusterID = NextClusterID++;
		Parent.ParentClusterID = GaussianClusterConstants::RootParentID; // Will be set if another level is created
		Parent.LODLevel = ParentLODLevel;
		Parent.ResetBounds();

		// Assign children to this parent
		const int32 ChildStartIdx = ParentIdx * MaxChildrenPerCluster;
		const int32 ChildEndIdx = FMath::Min(ChildStartIdx + MaxChildrenPerCluster, NumChildren);

		uint32 TotalSplatCount = 0;
		uint32 MinSplatStartIndex = MAX_uint32;

		for (int32 ChildIdx = ChildStartIdx; ChildIdx < ChildEndIdx; ChildIdx++)
		{
			FGaussianCluster& Child = ChildClusters[ChildIdx];

			// Set child's parent
			Child.ParentClusterID = Parent.ClusterID;

			// Add child ID to parent
			Parent.ChildClusterIDs.Add(Child.ClusterID);

			// Expand parent bounds to include child
			Parent.ExpandBounds(Child);

			// Track splat range
			TotalSplatCount += Child.SplatCount;
			MinSplatStartIndex = FMath::Min(MinSplatStartIndex, Child.SplatStartIndex);
		}

		Parent.SplatCount = TotalSplatCount;
		Parent.SplatStartIndex = MinSplatStartIndex;

		// Compute bounding sphere from AABB
		Parent.ComputeBoundingSphereFromAABB();

		OutParentClusters.Add(MoveTemp(Parent));
	}

	// Update child clusters with their new parent IDs (they're passed by reference)
	// Already done in the loop above
}

void FGaussianClusterBuilder::CalculateClusterBounds(
	FGaussianCluster& Cluster,
	const TArray<FGaussianSplatData>& Splats)
{
	Cluster.ResetBounds();

	const uint32 EndIndex = Cluster.SplatStartIndex + Cluster.SplatCount;
	for (uint32 i = Cluster.SplatStartIndex; i < EndIndex && i < (uint32)Splats.Num(); i++)
	{
		Cluster.ExpandBounds(Splats[i].Position);
	}

	Cluster.ComputeBoundingSphereFromAABB();
}

void FGaussianClusterBuilder::CalculateParentClusterBounds(
	FGaussianCluster& ParentCluster,
	const TArray<FGaussianCluster>& AllClusters)
{
	ParentCluster.ResetBounds();

	for (uint32 ChildID : ParentCluster.ChildClusterIDs)
	{
		// Find child cluster (assuming ClusterID matches array index for simplicity)
		// In a more robust implementation, use a map
		for (const FGaussianCluster& Cluster : AllClusters)
		{
			if (Cluster.ClusterID == ChildID)
			{
				ParentCluster.ExpandBounds(Cluster);
				break;
			}
		}
	}

	ParentCluster.ComputeBoundingSphereFromAABB();
}

void FGaussianClusterBuilder::CalculateClusterError(
	FGaussianCluster& ParentCluster,
	const TArray<FGaussianCluster>& AllClusters)
{
	// Error metric: maximum distance from parent center to any child's bounding sphere surface
	// This represents the worst-case error if we render the parent's LOD instead of children

	float MaxError = 0.0f;

	for (uint32 ChildID : ParentCluster.ChildClusterIDs)
	{
		// Find child cluster
		for (const FGaussianCluster& Child : AllClusters)
		{
			if (Child.ClusterID == ChildID)
			{
				// Distance from parent center to child center
				float CenterDistance = FVector3f::Dist(
					ParentCluster.BoundingSphereCenter,
					Child.BoundingSphereCenter);

				// Add child's radius to get distance to furthest point
				float TotalDistance = CenterDistance + Child.BoundingSphereRadius;

				// Also consider child's own error (propagate error up the tree)
				TotalDistance += Child.MaxError;

				MaxError = FMath::Max(MaxError, TotalDistance);
				break;
			}
		}
	}

	// Subtract parent's radius since error is relative to parent's representation
	ParentCluster.MaxError = FMath::Max(0.0f, MaxError - ParentCluster.BoundingSphereRadius);
}

//----------------------------------------------------------------------
// LOD Generation
//----------------------------------------------------------------------

FGaussianLODSplat FGaussianClusterBuilder::MergeGaussians(
	const TArray<FGaussianSplatData>& Splats,
	int32 StartIndex,
	int32 Count)
{
	FGaussianLODSplat Result;

	if (Count <= 0 || StartIndex < 0 || StartIndex >= Splats.Num())
	{
		return Result;
	}

	// Clamp count to available splats
	Count = FMath::Min(Count, Splats.Num() - StartIndex);

	// Calculate total opacity weight for weighted averaging
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < Count; i++)
	{
		TotalWeight += Splats[StartIndex + i].Opacity;
	}

	if (TotalWeight < SMALL_NUMBER)
	{
		TotalWeight = 1.0f; // Avoid division by zero
	}

	// Weighted centroid position
	FVector3f WeightedPosition = FVector3f::ZeroVector;
	FVector3f WeightedColor = FVector3f::ZeroVector;

	for (int32 i = 0; i < Count; i++)
	{
		const FGaussianSplatData& Splat = Splats[StartIndex + i];
		float Weight = Splat.Opacity / TotalWeight;

		WeightedPosition += Splat.Position * Weight;

		// Convert SH DC to color for averaging
		FVector3f Color = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);
		WeightedColor += Color * Weight;
	}

	Result.Position = WeightedPosition;
	Result.Color = WeightedColor;

	// Combined scale: use average scale expanded to cover the merged region
	// This is an approximation - proper covariance merging would be more complex
	FVector3f AvgScale = FVector3f::ZeroVector;
	FVector3f MinPos = FVector3f(MAX_FLT);
	FVector3f MaxPos = FVector3f(-MAX_FLT);

	for (int32 i = 0; i < Count; i++)
	{
		const FGaussianSplatData& Splat = Splats[StartIndex + i];
		float Weight = Splat.Opacity / TotalWeight;
		AvgScale += Splat.Scale * Weight;

		MinPos = FVector3f(
			FMath::Min(MinPos.X, Splat.Position.X),
			FMath::Min(MinPos.Y, Splat.Position.Y),
			FMath::Min(MinPos.Z, Splat.Position.Z));
		MaxPos = FVector3f(
			FMath::Max(MaxPos.X, Splat.Position.X),
			FMath::Max(MaxPos.Y, Splat.Position.Y),
			FMath::Max(MaxPos.Z, Splat.Position.Z));
	}

	// Scale should be larger to cover the spread of merged splats
	FVector3f Spread = (MaxPos - MinPos) * 0.5f;
	Result.Scale = AvgScale + Spread * 0.5f; // Blend between average and spread

	// Use identity rotation for merged splat (could compute average quaternion)
	Result.Rotation = FQuat4f::Identity;

	// Combined opacity: probabilistic - 1 - product(1 - opacity_i)
	// For small opacities, this approximates the sum
	float CombinedTransparency = 1.0f;
	for (int32 i = 0; i < Count; i++)
	{
		CombinedTransparency *= (1.0f - FMath::Clamp(Splats[StartIndex + i].Opacity, 0.0f, 1.0f));
	}
	Result.Opacity = FMath::Clamp(1.0f - CombinedTransparency, 0.0f, 1.0f);

	return Result;
}

FGaussianLODSplat FGaussianClusterBuilder::MergeLODSplats(
	const TArray<FGaussianLODSplat>& LODSplats,
	int32 StartIndex,
	int32 Count)
{
	FGaussianLODSplat Result;

	if (Count <= 0 || StartIndex < 0 || StartIndex >= LODSplats.Num())
	{
		return Result;
	}

	Count = FMath::Min(Count, LODSplats.Num() - StartIndex);

	// Calculate total opacity weight
	float TotalWeight = 0.0f;
	for (int32 i = 0; i < Count; i++)
	{
		TotalWeight += LODSplats[StartIndex + i].Opacity;
	}

	if (TotalWeight < SMALL_NUMBER)
	{
		TotalWeight = 1.0f;
	}

	// Weighted averages
	FVector3f WeightedPosition = FVector3f::ZeroVector;
	FVector3f WeightedColor = FVector3f::ZeroVector;
	FVector3f AvgScale = FVector3f::ZeroVector;
	FVector3f MinPos = FVector3f(MAX_FLT);
	FVector3f MaxPos = FVector3f(-MAX_FLT);

	for (int32 i = 0; i < Count; i++)
	{
		const FGaussianLODSplat& Splat = LODSplats[StartIndex + i];
		float Weight = Splat.Opacity / TotalWeight;

		WeightedPosition += Splat.Position * Weight;
		WeightedColor += Splat.Color * Weight;
		AvgScale += Splat.Scale * Weight;

		MinPos = FVector3f(
			FMath::Min(MinPos.X, Splat.Position.X),
			FMath::Min(MinPos.Y, Splat.Position.Y),
			FMath::Min(MinPos.Z, Splat.Position.Z));
		MaxPos = FVector3f(
			FMath::Max(MaxPos.X, Splat.Position.X),
			FMath::Max(MaxPos.Y, Splat.Position.Y),
			FMath::Max(MaxPos.Z, Splat.Position.Z));
	}

	Result.Position = WeightedPosition;
	Result.Color = WeightedColor;

	FVector3f Spread = (MaxPos - MinPos) * 0.5f;
	Result.Scale = AvgScale + Spread * 0.5f;
	Result.Rotation = FQuat4f::Identity;

	// Combined opacity
	float CombinedTransparency = 1.0f;
	for (int32 i = 0; i < Count; i++)
	{
		CombinedTransparency *= (1.0f - FMath::Clamp(LODSplats[StartIndex + i].Opacity, 0.0f, 1.0f));
	}
	Result.Opacity = FMath::Clamp(1.0f - CombinedTransparency, 0.0f, 1.0f);

	return Result;
}

void FGaussianClusterBuilder::GenerateLODSplats(
	const TArray<FGaussianSplatData>& Splats,
	FGaussianClusterHierarchy& Hierarchy,
	int32 ReductionRatio)
{
	if (ReductionRatio < 1)
	{
		ReductionRatio = 4; // Default
	}

	Hierarchy.LODSplats.Empty();
	uint32 NextLODSplatIndex = 0;

	UE_LOG(LogTemp, Log, TEXT("GenerateLODSplats: Generating LOD splats with reduction ratio %d"), ReductionRatio);

	// Process clusters by LOD level (bottom-up)
	for (uint32 LODLevel = 1; LODLevel <= Hierarchy.NumLODLevels; LODLevel++)
	{
		for (FGaussianCluster& Cluster : Hierarchy.Clusters)
		{
			if (Cluster.LODLevel != LODLevel)
			{
				continue;
			}

			// This cluster needs LOD splats
			Cluster.LODSplatStartIndex = NextLODSplatIndex;

			if (LODLevel == 1)
			{
				// First LOD level - merge from original splats
				int32 NumLODSplats = FMath::DivideAndRoundUp(static_cast<int32>(Cluster.SplatCount), ReductionRatio);
				NumLODSplats = FMath::Max(1, NumLODSplats); // At least 1 LOD splat

				for (int32 i = 0; i < NumLODSplats; i++)
				{
					int32 SrcStart = static_cast<int32>(Cluster.SplatStartIndex) + i * ReductionRatio;
					int32 SrcCount = FMath::Min(ReductionRatio,
						static_cast<int32>(Cluster.SplatStartIndex + Cluster.SplatCount) - SrcStart);

					if (SrcCount > 0)
					{
						FGaussianLODSplat MergedSplat = MergeGaussians(Splats, SrcStart, SrcCount);
						Hierarchy.LODSplats.Add(MergedSplat);
						NextLODSplatIndex++;
					}
				}

				Cluster.LODSplatCount = NextLODSplatIndex - Cluster.LODSplatStartIndex;
			}
			else
			{
				// Higher LOD levels - merge from children's LOD splats
				TArray<FGaussianLODSplat> ChildLODSplats;

				for (uint32 ChildID : Cluster.ChildClusterIDs)
				{
					for (const FGaussianCluster& Child : Hierarchy.Clusters)
					{
						if (Child.ClusterID == ChildID && Child.LODSplatCount > 0)
						{
							for (uint32 j = 0; j < Child.LODSplatCount; j++)
							{
								uint32 LODIndex = Child.LODSplatStartIndex + j;
								if (LODIndex < (uint32)Hierarchy.LODSplats.Num())
								{
									ChildLODSplats.Add(Hierarchy.LODSplats[LODIndex]);
								}
							}
							break;
						}
					}
				}

				if (ChildLODSplats.Num() > 0)
				{
					int32 NumLODSplats = FMath::DivideAndRoundUp(ChildLODSplats.Num(), ReductionRatio);
					NumLODSplats = FMath::Max(1, NumLODSplats);

					for (int32 i = 0; i < NumLODSplats; i++)
					{
						int32 SrcStart = i * ReductionRatio;
						int32 SrcCount = FMath::Min(ReductionRatio, ChildLODSplats.Num() - SrcStart);

						if (SrcCount > 0)
						{
							FGaussianLODSplat MergedSplat = MergeLODSplats(ChildLODSplats, SrcStart, SrcCount);
							Hierarchy.LODSplats.Add(MergedSplat);
							NextLODSplatIndex++;
						}
					}

					Cluster.LODSplatCount = NextLODSplatIndex - Cluster.LODSplatStartIndex;
				}
			}
		}
	}

	Hierarchy.TotalLODSplatCount = Hierarchy.LODSplats.Num();

	UE_LOG(LogTemp, Log, TEXT("GenerateLODSplats: Generated %d LOD splats"), Hierarchy.TotalLODSplatCount);
}
