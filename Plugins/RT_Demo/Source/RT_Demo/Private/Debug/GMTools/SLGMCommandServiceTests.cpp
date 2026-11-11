#include "SLGMCommandService.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLGMCommandServiceTest, "SL.GMTools.CommandService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLGMCommandServiceTest::RunTest(const FString& Parameters)
{
	IConsoleManager& Manager = IConsoleManager::Get();
	const TCHAR* IntName = TEXT("SL.GMTools.Tests.Integer");
	const TCHAR* FloatName = TEXT("SL.GMTools.Tests.Float");
	const TCHAR* ReadOnlyName = TEXT("SL.GMTools.Tests.ReadOnly");
	IConsoleVariable* Int = Manager.RegisterConsoleVariable(IntName, 3, TEXT("GM Tools test"));
	Manager.RegisterConsoleVariable(FloatName, 1.f, TEXT("GM Tools test"));
	Manager.RegisterConsoleVariable(ReadOnlyName, 7, TEXT("GM Tools test"), ECVF_ReadOnly);
	FSLGMToolEntry Entry;
	Entry.ConsoleVariable = IntName;
	Entry.Control = ESLGMControlType::Integer;
	Entry.Minimum = 0;
	Entry.Maximum = 10;
	FString Value;
	FText Result;
	TestTrue(TEXT("Read registered variable"), FSLGMCommandService::Read(Entry, Value, Result));
	TestEqual(TEXT("Read does not apply asset defaults"), Int->GetInt(), 3);
	TestTrue(TEXT("Apply integer"), FSLGMCommandService::Apply(Entry, TEXT("5"), Result));
	TestEqual(TEXT("Readback matches"), Int->GetInt(), 5);
	TestFalse(TEXT("Reject fractional integer"), FSLGMCommandService::Apply(Entry, TEXT("2.5"), Result));
	TestFalse(TEXT("Reject trailing command/text"), FSLGMCommandService::Apply(Entry, TEXT("2; quit"), Result));
	TestFalse(TEXT("Reject out-of-range value"), FSLGMCommandService::Apply(Entry, TEXT("11"), Result));
	TestFalse(TEXT("Reject NaN"), FSLGMCommandService::Apply(Entry, TEXT("nan"), Result));
	TestEqual(TEXT("Rejected edits preserve value"), Int->GetInt(), 5);
	Int->Set(8, ECVF_SetByConsole);
	FSLGMCommandService::Read(Entry, Value, Result);
	TestEqual(TEXT("External changes are reflected"), Value, FString(TEXT("8")));
	Entry.Control = ESLGMControlType::Toggle;
	TestFalse(TEXT("Toggle rejects non-boolean"), FSLGMCommandService::Apply(Entry, TEXT("2"), Result));
	TestTrue(TEXT("Toggle accepts 0"), FSLGMCommandService::Apply(Entry, TEXT("0"), Result));
	Entry.ConsoleVariable = ReadOnlyName;
	TestFalse(TEXT("Reject read-only"), FSLGMCommandService::Apply(Entry, TEXT("1"), Result));
	Entry.ConsoleVariable = TEXT("SL.GMTools.Tests.Missing");
	TestFalse(TEXT("Reject missing variable"), FSLGMCommandService::Apply(Entry, TEXT("1"), Result));
	Entry.ConsoleVariable = FloatName;
	TestFalse(TEXT("Reject type mismatch"), FSLGMCommandService::Apply(Entry, TEXT("1"), Result));
	Entry.Control = ESLGMControlType::Float;
	TestTrue(TEXT("Apply float"), FSLGMCommandService::Apply(Entry, TEXT("1.25"), Result));
	Entry.Minimum = 11;
	TestFalse(TEXT("Reject invalid configuration range"), FSLGMCommandService::Apply(Entry, TEXT("1"), Result));
	Manager.UnregisterConsoleObject(IntName, false);
	Manager.UnregisterConsoleObject(FloatName, false);
	Manager.UnregisterConsoleObject(ReadOnlyName, false);
	return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSLGMBatchTest, "SL.GMTools.CommandBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSLGMBatchTest::RunTest(const FString& Parameters)
{
	IConsoleManager& Manager = IConsoleManager::Get();
	const TCHAR* NumberName = TEXT("SL.GMTools.Tests.BatchNumber");
	const TCHAR* StringName = TEXT("SL.GMTools.Tests.BatchString");
	const TCHAR* CallbackName = TEXT("SL.GMTools.Tests.BatchCallback");
	IConsoleVariable* Number = Manager.RegisterConsoleVariable(NumberName, 1, TEXT("Batch test"));
	IConsoleVariable* String = Manager.RegisterConsoleVariable(StringName, TEXT("original"), TEXT("Batch test"));
	int32 Observed = -1;
	Manager.RegisterConsoleCommand(CallbackName, TEXT("Batch test"), FConsoleCommandDelegate::CreateLambda([&] { Observed = Number->GetInt(); }));
	FSLGMToolEntry Entry;
	Entry.Control = ESLGMControlType::CommandBatch;
	Entry.Commands = { FString(NumberName) + TEXT(" 4"), CallbackName, FString(NumberName) + TEXT(" 8"), FString(StringName) + TEXT(" NFOR") };
	FText Result;
	TestTrue(TEXT("Execute mixed ordered commands"), FSLGMCommandService::ExecuteBatch(Entry, nullptr, Result));
	TestEqual(TEXT("Callback sees earlier command, not later command"), Observed, 4);
	TestEqual(TEXT("Last numeric command wins"), Number->GetInt(), 8);
	TestEqual(TEXT("String CVar supports denoiser name"), String->GetString(), FString(TEXT("NFOR")));
	Entry.Commands = { FString(NumberName) + TEXT(" 2"), TEXT("SL.GMTools.Tests.UnknownCommand"), FString(NumberName) + TEXT(" 7") };
	TestFalse(TEXT("Unknown command stops batch"), FSLGMCommandService::ExecuteBatch(Entry, nullptr, Result));
	TestEqual(TEXT("Earlier writes retained, later commands skipped"), Number->GetInt(), 2);
	Entry.Commands = { FString(NumberName) + TEXT(" 6"), TEXT("  ") };
	TestFalse(TEXT("Reject empty step before any execution"), FSLGMCommandService::ExecuteBatch(Entry, nullptr, Result));
	TestEqual(TEXT("Invalid configuration makes no writes"), Number->GetInt(), 2);
	Entry.Commands = { FString(NumberName) + TEXT(" 1.5"), FString(StringName) + TEXT(" OIDN") };
	TestFalse(TEXT("Reject invalid integer and stop"), FSLGMCommandService::ExecuteBatch(Entry, nullptr, Result));
	TestEqual(TEXT("Skipped string command preserves value"), String->GetString(), FString(TEXT("NFOR")));
	Entry.Commands.Empty();
	TestFalse(TEXT("Reject empty batch"), FSLGMCommandService::ExecuteBatch(Entry, nullptr, Result));
	Manager.UnregisterConsoleObject(CallbackName, false);
	Manager.UnregisterConsoleObject(NumberName, false);
	Manager.UnregisterConsoleObject(StringName, false);
	return true;
}
#endif
