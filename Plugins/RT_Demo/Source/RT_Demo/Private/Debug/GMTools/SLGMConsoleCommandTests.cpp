#if WITH_DEV_AUTOMATION_TESTS
#include "Debug/GMTools/SLGMToolsComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"

// The plugin must work in a project whose player controller knows nothing about it:
// `gm` alone has to attach the tools to the local player and open the panel.
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FSLCheckGMPanelOpened, FAutomationTestBase*, Test, TWeakObjectPtr<APlayerController>, Player);
bool FSLCheckGMPanelOpened::Update()
{
	APlayerController* PC = Player.Get();
	if (!PC)
	{
		Test->AddError(TEXT("The player controller was destroyed before the panel opened."));
		return true;
	}
	USLGMToolsComponent* Tools = PC->FindComponentByClass<USLGMToolsComponent>();
	// The command defers opening to the next world tick; allow a few seconds of slack.
	if ((!Tools || !Tools->IsPanelOpen()) && GetCurrentRunTime() < 5.0) return false;
	if (!Test->TestNotNull(TEXT("gm attached the tools to the local player controller"), Tools)) return true;
	Test->TestTrue(TEXT("gm opened the panel"), Tools->IsPanelOpen());
	Test->TestTrue(TEXT("A second lookup reuses the same component"), USLGMToolsComponent::GetOrCreate(PC) == Tools);
	Tools->ClosePanel();
	Test->TestFalse(TEXT("The panel closes"), Tools->IsPanelOpen());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLGMConsoleCommandTest, "SL.GMTools.ConsoleCommand",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FSLGMConsoleCommandTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
	if (!TestNotNull(TEXT("Run in a game instance"), World)) return false;
	APlayerController* Player = GEngine->GetFirstLocalPlayerController(World);
	if (!TestNotNull(TEXT("Local player controller"), Player)) return false;
	if (!TestNotNull(TEXT("gm is a registered console command"), IConsoleManager::Get().FindConsoleObject(TEXT("gm")))) return false;
	TestTrue(TEXT("The engine handles gm"), GEngine->Exec(World, TEXT("gm")));
	ADD_LATENT_AUTOMATION_COMMAND(FSLCheckGMPanelOpened(this, Player));
	return true;
}
#endif
