// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"

AGaussianSplatActor::AGaussianSplatActor()
{
	// Create the Gaussian Splat component as a default subobject
	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
	RootComponent = GaussianSplatComponent;
}
