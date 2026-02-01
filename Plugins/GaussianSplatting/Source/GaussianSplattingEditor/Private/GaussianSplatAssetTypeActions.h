// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UGaussianSplatAsset;

/**
 * Asset type actions for Gaussian Splat assets
 * Provides editor integration like custom color, category, and actions
 */
class FAssetTypeActions_GaussianSplatAsset : public FAssetTypeActions_Base
{
public:
	//~ Begin IAssetTypeActions Interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return true; }
	virtual void GetActions(const TArray<UObject*>& InObjects, struct FToolMenuSection& Section) override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;
	//~ End IAssetTypeActions Interface

private:
	/** Handler for reimporting assets */
	void ExecuteReimport(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for showing asset info */
	void ExecuteShowInfo(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);
};
