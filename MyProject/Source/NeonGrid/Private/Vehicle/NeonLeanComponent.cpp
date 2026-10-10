#include "Vehicle/NeonLeanComponent.h"

#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehicleConfig.h"

UNeonLeanComponent::UNeonLeanComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After movement has written this frame's steering.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UNeonLeanComponent::BeginPlay()
{
	Super::BeginPlay();

	if (const AActor* Owner = GetOwner())
	{
		Movement = Owner->FindComponentByClass<UNeonMovementComponent>();
	}
}

void UNeonLeanComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!Movement)
	{
		return;
	}

	const UNeonVehicleConfig* C = Movement->GetConfig();
	const float Steer = Movement->IsAlive() ? Movement->GetAppliedSteering() : 0.f;

	const float TargetLean = FMath::Sign(Steer) * C->EvaluateLeanAngle(FMath::Abs(Steer));
	const bool bLeaningIn = FMath::Abs(TargetLean) > FMath::Abs(CurrentLean);
	const float InterpSpeed = bLeaningIn ? C->LeanResponseSpeed : C->LeanReturnSpeed;

	CurrentLean = FMath::FInterpTo(CurrentLean, TargetLean, DeltaTime, InterpSpeed);

	SetRelativeRotation(FRotator(0.f, 0.f, CurrentLean * LeanDirection));
}
