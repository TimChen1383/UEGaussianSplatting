// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplattingEditorModule.h"
#include "GaussianSplatAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

#define LOCTEXT_NAMESPACE "FGaussianSplattingEditorModule"

void FGaussianSplattingEditorModule::StartupModule()
{
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Register Gaussian Splat asset type
	TSharedPtr<IAssetTypeActions> GaussianSplatAssetActions = MakeShareable(new FAssetTypeActions_GaussianSplatAsset());
	AssetTools.RegisterAssetTypeActions(GaussianSplatAssetActions.ToSharedRef());
	RegisteredAssetTypeActions.Add(GaussianSplatAssetActions);

	UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor module started."));
}

void FGaussianSplattingEditorModule::ShutdownModule()
{
	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		for (auto& Action : RegisteredAssetTypeActions)
		{
			AssetTools.UnregisterAssetTypeActions(Action.ToSharedRef());
		}
	}
	RegisteredAssetTypeActions.Empty();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplattingEditor module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSplattingEditorModule, GaussianSplattingEditor)
