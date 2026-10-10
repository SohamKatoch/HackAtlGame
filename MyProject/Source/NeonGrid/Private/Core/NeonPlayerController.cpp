#include "Core/NeonPlayerController.h"

#include "Arena/NeonArena.h"
#include "Blueprint/UserWidget.h"
#include "Core/NeonGameInstance.h"
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

	if (!SprintAction)
	{
		UInputAction* Sprint = NewObject<UInputAction>(this, TEXT("IA_NeonSprint"));
		Sprint->ValueType = EInputActionValueType::Boolean;
		SprintAction = Sprint;
	}

	if (!JumpAction)
	{
		UInputAction* Jump = NewObject<UInputAction>(this, TEXT("IA_NeonJump"));
		Jump->ValueType = EInputActionValueType::Boolean;
		JumpAction = Jump;
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

		IMC->MapKey(SprintAction, EKeys::LeftShift);
		IMC->MapKey(SprintAction, EKeys::Gamepad_LeftShoulder);

		IMC->MapKey(JumpAction, EKeys::LeftControl);
		IMC->MapKey(JumpAction, EKeys::F);
		IMC->MapKey(JumpAction, EKeys::Gamepad_RightShoulder);

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

	EIC->BindAction(SprintAction, ETriggerEvent::Started, this, &ANeonPlayerController::OnSprintPressed);
	EIC->BindAction(SprintAction, ETriggerEvent::Triggered, this, &ANeonPlayerController::OnSprintPressed);
	EIC->BindAction(SprintAction, ETriggerEvent::Completed, this, &ANeonPlayerController::OnSprintReleased);
	EIC->BindAction(SprintAction, ETriggerEvent::Canceled, this, &ANeonPlayerController::OnSprintReleased);

	EIC->BindAction(JumpAction, ETriggerEvent::Started, this, &ANeonPlayerController::OnJumpPressed);
	EIC->BindAction(JumpAction, ETriggerEvent::Triggered, this, &ANeonPlayerController::OnJumpPressed);
	EIC->BindAction(JumpAction, ETriggerEvent::Completed, this, &ANeonPlayerController::OnJumpReleased);
	EIC->BindAction(JumpAction, ETriggerEvent::Canceled, this, &ANeonPlayerController::OnJumpReleased);
}

void ANeonPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	CurrentInput = FNeonVehicleInput();

	const ANeonGameState* GS = GetWorld() ? GetWorld()->GetGameState<ANeonGameState>() : nullptr;
	if (ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(InPawn);
		Vehicle && IsLocalController() && (!GS || GS->GetPhase() != ENeonMatchPhase::MainMenu))
	{
		ViewVehicle(Vehicle, ViewBlendTime);
	}
}

void ANeonPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (!BoundGameState.IsValid())
	{
		BindGameState(); // game state may replicate after BeginPlay on clients
	}

	if (ANeonVehiclePawn* Vehicle = Cast<ANeonVehiclePawn>(GetPawn()))
	{
		if (UNeonMovementComponent* Movement = Vehicle->GetMovement())
		{
			Movement->SetInput(CurrentInput);
		}
	}

	if (IsLocalController())
	{
		UpdateSpectating();
	}
}

// ---------------------------------------------------------------------------
// Spectating
// ---------------------------------------------------------------------------

ANeonVehiclePawn* ANeonPlayerController::GetSpectateTarget() const
{
	return bSpectating ? SpectateTarget.Get() : nullptr;
}

namespace
{
	bool IsVehicleAlive(const ANeonVehiclePawn* V)
	{
		return IsValid(V) && V->GetMovement() && V->GetMovement()->IsAlive();
	}
}

void ANeonPlayerController::CollectAliveVehicles(TArray<ANeonVehiclePawn*>& Out) const
{
	Out.Reset();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ANeonVehiclePawn> It(World); It; ++It)
		{
			if (*It != GetPawn() && IsVehicleAlive(*It))
			{
				Out.Add(*It);
			}
		}
	}
	// Stable order so A/D cycles predictably.
	Out.Sort([](const ANeonVehiclePawn& A, const ANeonVehiclePawn& B) { return A.GetPlayerSlot() < B.GetPlayerSlot(); });
}

bool ANeonPlayerController::CycleSpectateTarget(int32 Direction)
{
	TArray<ANeonVehiclePawn*> Alive;
	CollectAliveVehicles(Alive);
	if (Alive.Num() == 0)
	{
		SpectateTarget = nullptr;
		return false;
	}

	int32 Index = Alive.IndexOfByKey(SpectateTarget.Get());
	if (Index == INDEX_NONE)
	{
		Index = Direction >= 0 ? 0 : Alive.Num() - 1;
	}
	else
	{
		Index = (Index + (Direction >= 0 ? 1 : -1) + Alive.Num()) % Alive.Num();
	}

	ANeonVehiclePawn* Next = Alive[Index];
	if (Next != SpectateTarget.Get() || GetViewTarget() != Next)
	{
		SpectateTarget = Next;
		ViewVehicle(Next, SpectateBlendTime);
		bSpectatingArena = false;
	}
	return true;
}

void ANeonPlayerController::StopSpectating(bool bViewOwnPawn)
{
	const bool bWas = bSpectating;
	bSpectating = false;
	bSpectatingArena = false;
	SpectateTarget = nullptr;
	LastSpectateSteerDir = 0;
	if (bWas && bViewOwnPawn)
	{
		if (ANeonVehiclePawn* Own = Cast<ANeonVehiclePawn>(GetPawn()))
		{
			ViewVehicle(Own, ViewBlendTime);
		}
	}
}

void ANeonPlayerController::UpdateSpectating()
{
	const ANeonGameState* GS = BoundGameState.Get();
	const ENeonMatchPhase Phase = GS ? GS->GetPhase() : ENeonMatchPhase::MainMenu;
	const bool bRoundPhase = Phase == ENeonMatchPhase::Playing || Phase == ENeonMatchPhase::RoundResults;
	if (!bRoundPhase)
	{
		StopSpectating(false);
		return;
	}

	ANeonVehiclePawn* Own = Cast<ANeonVehiclePawn>(GetPawn());
	if (IsVehicleAlive(Own))
	{
		StopSpectating(true);
		return;
	}

	// Our vehicle is out. Follow somebody who is still driving.
	if (!bSpectating)
	{
		bSpectating = true;
		SpectateTarget = nullptr;
		if (Own)
		{
			Own->SetFollowCameraActive(false);
		}
		// Ignore a steer key that was already held when we died; wait for a fresh press.
		LastSpectateSteerDir = CurrentInput.Steering > 0.5f ? 1 : (CurrentInput.Steering < -0.5f ? -1 : 0);
	}

	const int32 SteerDir = CurrentInput.Steering > 0.5f ? 1 : (CurrentInput.Steering < -0.5f ? -1 : 0);
	const bool bPressed = SteerDir != 0 && SteerDir != LastSpectateSteerDir;
	LastSpectateSteerDir = SteerDir;

	bool bHaveTarget = IsVehicleAlive(SpectateTarget.Get());
	if (bPressed || !bHaveTarget)
	{
		bHaveTarget = CycleSpectateTarget(bPressed ? SteerDir : 1);
	}

	if (!bHaveTarget && !bSpectatingArena)
	{
		// Nobody left to follow (simultaneous wipe-out): show the whole arena.
		bSpectatingArena = true;
		ViewArena();
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

void ANeonPlayerController::OnSprintPressed(const FInputActionValue& /*Value*/)
{
	CurrentInput.bSprint = true;
}

void ANeonPlayerController::OnSprintReleased(const FInputActionValue& /*Value*/)
{
	CurrentInput.bSprint = false;
}

void ANeonPlayerController::OnJumpPressed(const FInputActionValue& /*Value*/)
{
	CurrentInput.bJump = true;
}

void ANeonPlayerController::OnJumpReleased(const FInputActionValue& /*Value*/)
{
	CurrentInput.bJump = false;
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

void ANeonPlayerController::RequestHostLan()
{
	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->HostListen();
	}
}

void ANeonPlayerController::RequestJoinLan(const FString& Address)
{
	if (UNeonGameInstance* GI = GetGameInstance<UNeonGameInstance>())
	{
		GI->JoinListen(Address);
	}
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

void ANeonPlayerController::ViewVehicle(ANeonVehiclePawn* Vehicle, float BlendTime)
{
	if (!Vehicle)
	{
		return;
	}
	Vehicle->SetFollowCameraActive(true);
	SetViewTargetWithBlend(Vehicle, BlendTime, VTBlend_Cubic);
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
		// Countdown always returns the camera to our own (respawned) vehicle. During Playing /
		// RoundResults a dead player keeps the spectator camera (UpdateSpectating owns the view then).
		if (Phase == ENeonMatchPhase::Countdown)
		{
			StopSpectating(false);
		}
		if (ANeonVehiclePawn* Own = Cast<ANeonVehiclePawn>(GetPawn()); Own && !bSpectating)
		{
			ViewVehicle(Own, ViewBlendTime);
		}
	}
}
