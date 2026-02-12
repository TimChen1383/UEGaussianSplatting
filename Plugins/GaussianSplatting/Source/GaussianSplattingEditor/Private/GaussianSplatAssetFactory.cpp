// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetFactory.h"
#include "GaussianSplatAsset.h"
#include "PLYFileReader.h"
#include "GaussianClusterBuilder.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopedSlowTask.h"

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = false;

	SupportedClass = UGaussianSplatAsset::StaticClass();

	Formats.Add(TEXT("ply;PLY Gaussian Splatting File"));
}

bool UGaussianSplatAssetFactory::FactoryCanImport(const FString& Filename)
{
	const FString Extension = FPaths::GetExtension(Filename);
	return Extension.Equals(TEXT("ply"), ESearchCase::IgnoreCase) && FPLYFileReader::IsValidPLYFile(Filename);
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	UGaussianSplatAsset* NewAsset = ImportPLYFile(Filename, InParent, InName, Flags, nullptr);

	if (!NewAsset)
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import Gaussian Splat from: %s"), *Filename);
		}
	}

	return NewAsset;
}

FText UGaussianSplatAssetFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("Gaussian Splat Asset"));
}

bool UGaussianSplatAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && !Asset->SourceFilePath.IsEmpty())
	{
		OutFilenames.Add(Asset->SourceFilePath);
		return true;
	}
	return false;
}

void UGaussianSplatAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && NewReimportPaths.Num() > 0)
	{
		Asset->SourceFilePath = NewReimportPaths[0];
	}
}

EReimportResult::Type UGaussianSplatAssetFactory::Reimport(UObject* Obj)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	if (Asset->SourceFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file path is empty"));
		return EReimportResult::Failed;
	}

	if (!FPaths::FileExists(Asset->SourceFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file not found: %s"), *Asset->SourceFilePath);
		return EReimportResult::Failed;
	}

	// Use the original quality level
	QualityLevel = Asset->ImportQuality;

	UGaussianSplatAsset* ReimportedAsset = ImportPLYFile(
		Asset->SourceFilePath,
		Asset->GetOuter(),
		Asset->GetFName(),
		Asset->GetFlags(),
		Asset
	);

	if (ReimportedAsset)
	{
		return EReimportResult::Succeeded;
	}

	return EReimportResult::Failed;
}

UGaussianSplatAsset* UGaussianSplatAssetFactory::ImportPLYFile(
	const FString& FilePath,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UGaussianSplatAsset* ExistingAsset)
{
	FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("Importing Gaussian Splat...")));
	SlowTask.MakeDialog(true);

	// Read PLY file
	SlowTask.EnterProgressFrame(20.0f, FText::FromString(TEXT("Reading PLY file...")));

	TArray<FGaussianSplatData> SplatData;
	FString ErrorMessage;

	if (!FPLYFileReader::ReadPLYFile(FilePath, SplatData, ErrorMessage))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to read PLY file: %s"), *ErrorMessage);
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Read %d splats from PLY file"), SplatData.Num());

	// Build cluster hierarchy (this reorders splats for spatial locality)
	SlowTask.EnterProgressFrame(20.0f, FText::FromString(TEXT("Building cluster hierarchy...")));

	FGaussianClusterHierarchy ClusterHierarchy;
	FGaussianClusterBuilder::FBuildSettings BuildSettings;
	BuildSettings.SplatsPerCluster = 128;
	BuildSettings.MaxChildrenPerCluster = 8;
	BuildSettings.bReorderSplats = true; // Reorder for better cache locality

	bool bClusteringSucceeded = FGaussianClusterBuilder::BuildClusterHierarchy(
		SplatData, ClusterHierarchy, BuildSettings);

	if (bClusteringSucceeded)
	{
		UE_LOG(LogTemp, Log, TEXT("Built cluster hierarchy: %d clusters, %d LOD levels"),
			ClusterHierarchy.Clusters.Num(), ClusterHierarchy.NumLODLevels);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to build cluster hierarchy, continuing without clustering"));
	}

	// Create or reuse asset
	SlowTask.EnterProgressFrame(10.0f, FText::FromString(TEXT("Creating asset...")));

	UGaussianSplatAsset* Asset = ExistingAsset;
	if (!Asset)
	{
		Asset = NewObject<UGaussianSplatAsset>(InParent, UGaussianSplatAsset::StaticClass(), InName, Flags);
	}

	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create Gaussian Splat asset"));
		return nullptr;
	}

	// Store source file path
	Asset->SourceFilePath = FilePath;

	// Initialize asset from splat data (uses reordered splats if clustering succeeded)
	SlowTask.EnterProgressFrame(45.0f, FText::FromString(TEXT("Compressing splat data...")));

	Asset->InitializeFromSplatData(SplatData, QualityLevel);

	// Store cluster hierarchy if building succeeded
	if (bClusteringSucceeded)
	{
		SlowTask.EnterProgressFrame(5.0f, FText::FromString(TEXT("Storing cluster hierarchy...")));
		Asset->ClusterHierarchy = MoveTemp(ClusterHierarchy);
		Asset->bHasClusterHierarchy = true;
	}
	else
	{
		Asset->bHasClusterHierarchy = false;
		Asset->ClusterHierarchy.Reset();
	}

	// Mark package dirty
	Asset->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Successfully imported Gaussian Splat asset: %d splats, %d clusters, %lld bytes"),
		Asset->GetSplatCount(), Asset->GetClusterCount(), Asset->GetMemoryUsage());

	return Asset;
}
