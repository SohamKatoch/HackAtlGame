#include "Core/NeonPlayerController.h"

#include "Arena/NeonArena.h"
#include "Blueprint/UserWidget.h"
#include "Core/NeonGameMode.h"
#include "Core/NeonGameState.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "NeonGrid.h"
#include "UI/NeonHUDWidget.h"
#include "UI/NeonMainMenuWidget.h"
#include "Vehicle/NeonMovementComponent.h"
#include "Vehicle/NeonVehiclePawn.h"

ANeonPlayerController::ANeonPlayerController()
{
	bShowMouseCursor = false;
	// View targets are managed by phase (arena camera in the menu, follow camera in play) so blends work.
	bAutoManageActiveCameraTarget = false;
	HUDWidgetClass = UNeonHUDWidget::StaticClass();
	MainMenuWidgetClass = UNeonMainMenuWidget::StaticClass();
}

void ANeonPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// Idempotent; SetupInputComponent normally runs first, but do not depend on it.
	BuildDefaultInputAssets();

	if (ULocalPlayer* LP = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (VehicleMappingContext)
			{
				Subsystem->AddMappingContext(VehicleMappingContext, 0);
			}
		}
	}

	EnsureUI();
	BindGameState();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void ANeonPlayerController::BuildDefaultInputAssets()
{
	if (!SteerAction)
	{
		UInputAction* Steer = NewObject<UInputAction>(this, TEXT("IA_NeonSteer"));
		Steer->ValueType = EInputActionValueType::Axis1D;
		SteerAction = Steer;
	}

	if (!MenuAction)
	{
		UInputAction* Menu = NewObject<UInputAction>(this, TEXT("IA_NeonMenu"));
		Menu->ValueType = EInputActionValueType::Boolean;
		MenuAction = Menu;
	}

	if (!ActivateAction)
	{
		UInputAction* Activate = NewObject<UInputAction>(this, TEXT("IA_NeonActivate"));
		Activate->ValueType = EInputActionValueType::Boolean;
		ActivateAction = Activate;
	}

	if (!VehicleMappingContext)
	{
		UInputMappingContext* IMC = NewObject<UInputMappingContext>(this, TEXT("IMC_NeonVehicle"));

		// Right
		IMC->MapKey(SteerAction, EKeys::D);
		IMC->MapKey(SteerAction, EKeys::Right);

		// Left (negated)
		for (const FKey& Key : { EKeys::A, EKeys::Left })
		{
			FEnhancedActionKeyMapping& Mapping = IMC->MapKey(SteerAction, Key);
			Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(IMC));
		}

		// Gamepad left stick X with a small dead zone
		{
			FEnhancedActionKeyMapping& Mapping = IMC->MapKey(SteerAction, EKeys::Gamepad_LeftX);
			UInputModifierDeadZone* DeadZone = NewObject<UInputModifierDeadZone>(IMC);
			DeadZone->LowerThreshold = 0.2f;
			DeadZone->Type = EDeadZoneType::Axial;
			Mapping.Modifiers.Add(DeadZone);
		}

		IMC->MapKey(MenuAction, EKeys::Escape);
		IMC->MapKey(MenuAction, EKeys::Gamepad_Special_Right);

		IMC->MapKey(ActivateAction, EKeys::SpaceBar);
		IMC->MapKey(ActivateAction, EKeys::Gamepad_RightTrigger);
		IMC->MapKey(ActivateAction, EKeys::Gamepad_FaceButton_Bottom);

		VehicleMappingContext = IMC;
	}
}

void ANeonPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	BuildDefaultInputAssets();

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC)
	{
		UE_LOG(LogNeonGrid, Error, TEXT("InputComponent is not an EnhancedInputComponent. Check DefaultInput.ini DefaultInputComponentClass."));
		return;
	}

	EIC->BindAction(SteerAction, ETriggerEvent::Triggered, this, &ANeonPlayerController::OnSteer);
	EIC->BindAction(SteerAction, ETriggerEvent::Completed, this, &ANeonPlayerController::OnSteerReleased);
	EIC->BindAction(SteerAction, ETriggerEvent::Canceled, this, &ANeonPlayerController::OnSteerReleased);
	EIC->BindAction(MenuAction, ETriggerEvent::Started, this, &ANeonPlayerController::OnMenuPressed);

	EIC->BindAction(ActivateAction, ETriggerEvent::Started, this, &ANeonPlayerController::OnActivatePressed);
	EIC->BindAction(ActivateAction, ETriggerEvent::Completed, this, &ANeonPlayerController::OnActivateReleased);
	EIC->BindAction(ActivateAction, ETriggerEvent::Canceled, this, &ANeonPlayerController::OnActivateReleased);
}

void ANeonPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	CurrentInput = FNeonVehicleInput();

	const ANeonGameState* GS = GetWorld() ? GetWorld()->GetGameState<ANeonGameState>() : nullptr;
	if (InPawn && IsLocalController() && (!GS || GS->GetPhase() != ENeonMatchPhase::MainMenu))
	{
		SetViewTargetWithBlend(InPawn, ViewBlendTime);
	}
}

void ANeonPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (!BoundGameState.IsValid())
	{
		BindGameState(); // game state may replicate after BeginPlay on clients
	}

	ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(GetPawn());
	if (!Vehicle)
	{
		return;
	}

	if (UNeonMovementComponent* Movement = Vehicle->GetMovement())
	{
		Movement->SetInput(CurrentInput);
	}
}

void ANeonPlayerController::OnSteer(const FInputActionValue& Value)
{
	CurrentInput.Steering = FMath::Clamp(Value.Get<float>(), -1.f, 1.f);
}

void ANeonPlayerController::OnSteerReleased(const FInputActionValue& /*Value*/)
{
	CurrentInput.Steering = 0.f;
}

void ANeonPlayerController::OnMenuPressed(const FInputActionValue& /*Value*/)
{
	const ANeonGameState* GS = GetWorld() ? GetWorld()->GetGameState<ANeonGameState>() : nullptr;
	if (GS && GS->GetPhase() != ENeonMatchPhase::MainMenu)
	{
		RequestReturnToMainMenu();
	}
}

void ANeonPlayerController::OnActivatePressed(const FInputActionValue& /*Value*/)
{
	CurrentInput.bActivatePowerup = true;
}

void ANeonPlayerController::OnActivateReleased(const FInputActionValue& /*Value*/)
{
	CurrentInput.bActivatePowerup = false;
}

// ---------------------------------------------------------------------------
// Match requests
// ---------------------------------------------------------------------------

void ANeonPlayerController::RequestStartSoloMatch(const FNeonMatchSettings& Settings)
{
	ServerStartSoloMatch(Settings);
}

void ANeonPlayerController::RequestRematch()
{
	ServerRematch();
}

void ANeonPlayerController::RequestReturnToMainMenu()
{
	ServerReturnToMainMenu();
}

void ANeonPlayerController::RequestQuit()
{
	UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false);
}

void ANeonPlayerController::ServerStartSoloMatch_Implementation(const FNeonMatchSettings& Settings)
{
	if (ANeonGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ANeonGameMode>() : nullptr)
	{
		GM->StartSoloMatch(Settings);
	}
}

void ANeonPlayerController::ServerRematch_Implementation()
{
	if (ANeonGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ANeonGameMode>() : nullptr)
	{
		GM->Rematch();
	}
}

void ANeonPlayerController::ServerReturnToMainMenu_Implementation()
{
	if (ANeonGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<ANeonGameMode>() : nullptr)
	{
		GM->ReturnToMainMenu();
	}
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void ANeonPlayerController::EnsureUI()
{
	if (bUIBuilt || !IsLocalController())
	{
		return;
	}
	if (!GEngine || !GEngine->GameViewport)
	{
		return; // dedicated server or no viewport yet
	}
	bUIBuilt = true;

	if (HUDWidgetClass)
	{
		HUDWidget = CreateWidget<UNeonHUDWidget>(this, HUDWidgetClass);
		if (HUDWidget)
		{
			HUDWidget->AddToViewport(0);
			HUDWidget->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	if (MainMenuWidgetClass)
	{
		MainMenuWidget = CreateWidget<UNeonMainMenuWidget>(this, MainMenuWidgetClass);
		if (MainMenuWidget)
		{
			MainMenuWidget->AddToViewport(10);
			MainMenuWidget->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void ANeonPlayerController::BindGameState()
{
	ANeonGameState* GS = GetWorld() ? GetWorld()->GetGameState<ANeonGameState>() : nullptr;
	if (!GS || BoundGameState.Get() == GS)
	{
		return;
	}
	if (ANeonGameState* Old = BoundGameState.Get())
	{
		Old->OnPhaseChanged.RemoveDynamic(this, &ANeonPlayerController::HandlePhaseChanged);
	}
	BoundGameState = GS;
	GS->OnPhaseChanged.AddDynamic(this, &ANeonPlayerController::HandlePhaseChanged);
	ApplyPhaseUI(GS->GetPhase());
}

void ANeonPlayerController::HandlePhaseChanged(ENeonMatchPhase NewPhase)
{
	ApplyPhaseUI(NewPhase);
}

void ANeonPlayerController::ViewArena()
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ANeonArena> It(World); It; ++It)
		{
			SetViewTargetWithBlend(*It, ViewBlendTime);
			return;
		}
	}
}

void ANeonPlayerController::ApplyPhaseUI(ENeonMatchPhase Phase)
{
	if (!IsLocalController())
	{
		return;
	}
	EnsureUI();

	const bool bMenu = Phase == ENeonMatchPhase::MainMenu;
	const bool bResults = Phase == ENeonMatchPhase::MatchResults;

	if (MainMenuWidget)
	{
		MainMenuWidget->SetVisibility(bMenu ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		if (bMenu)
		{
			MainMenuWidget->ShowMainPage();
		}
	}
	if (HUDWidget)
	{
		HUDWidget->SetVisibility(bMenu ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}

	if (bMenu)
	{
		FInputModeUIOnly Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		if (MainMenuWidget)
		{
			Mode.SetWidgetToFocus(MainMenuWidget->TakeWidget());
		}
		SetInputMode(Mode);
		SetShowMouseCursor(true);
		ViewArena();
	}
	else if (bResults)
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		SetShowMouseCursor(true);
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
		SetShowMouseCursor(false);
		if (APawn* P = GetPawn())
		{
			SetViewTargetWithBlend(P, ViewBlendTime);
		}
	}
}
