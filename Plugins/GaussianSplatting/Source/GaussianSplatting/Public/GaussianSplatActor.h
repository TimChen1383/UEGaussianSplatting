// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatComponent;

/**
 * Simple actor for placing Gaussian Splats in the level.
 * Use this instead of Blueprint actors for better editor performance.
 */
UCLASS()
class GAUSSIANSPLATTING_API AGaussianSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSplatActor();

	/** The Gaussian Splat component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;
};
