#include "SLGMCommandService.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"
#include "Misc/StringOutputDevice.h"

namespace
{
	IConsoleVariable* Resolve(const FSLGMToolEntry& Entry, FText& Error)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Entry.ConsoleVariable);
		if (!Variable)
		{
			Error = FText::FromString(TEXT("Console variable not found or not registered."));
			return nullptr;
		}
		// RadioGroup always writes integers; skip the float/int mismatch check for it.
		if (Entry.Control != ESLGMControlType::RadioGroup)
		{
			const bool bInteger = Entry.Control != ESLGMControlType::Float;
			if ((bInteger && !Variable->IsVariableInt()) || (!bInteger && !Variable->IsVariableFloat()))
			{
				Error = FText::FromString(TEXT("Control type does not match the console variable type."));
				return nullptr;
			}
		}
		else if (!Variable->IsVariableInt())
		{
			Error = FText::FromString(TEXT("RadioGroup requires an integer console variable."));
			return nullptr;
		}
		return Variable;
	}
}

bool FSLGMCommandService::Read(const FSLGMToolEntry& Entry, FString& Value, FText& Error)
{
	Error = FText::GetEmpty();
	if (Entry.Control == ESLGMControlType::CommandBatch)
	{
		Error = FText::FromString(TEXT("Use Run commands to execute a command batch."));
		return false;
	}
	IConsoleVariable* Variable = Resolve(Entry, Error);
	if (!Variable) return false;
	Value = Variable->GetString();
	if (Variable->TestFlags(ECVF_ReadOnly))
	{
		Error = FText::FromString(TEXT("This console variable is read-only and cannot be changed at runtime."));
		return false;
	}
	if (Entry.Control != ESLGMControlType::RadioGroup &&
		Entry.bLimitRange && (!FMath::IsFinite(Entry.Minimum) || !FMath::IsFinite(Entry.Maximum) || Entry.Minimum > Entry.Maximum))
	{
		Error = FText::FromString(TEXT("The configured value range is invalid."));
		return false;
	}
	return true;
}

bool FSLGMCommandService::Apply(const FSLGMToolEntry& Entry, const FString& Input, FText& Result)
{
	// RadioGroup: validate the chosen value is in the declared option set, then write it.
	if (Entry.Control == ESLGMControlType::RadioGroup)
	{
		FText ReadError;
		IConsoleVariable* Variable = Resolve(Entry, ReadError);
		if (!Variable) { Result = ReadError; return false; }
		int32 Chosen = 0;
		if (!LexTryParseString(Chosen, *Input))
		{
			Result = FText::FromString(TEXT("Invalid radio option value."));
			return false;
		}
		bool bValidOption = false;
		for (const FSLGMRadioOption& Opt : Entry.RadioOptions)
		{
			if (Opt.Value == Chosen) { bValidOption = true; break; }
		}
		if (!bValidOption)
		{
			Result = FText::FromString(FString::Printf(TEXT("Value %d is not a valid option for this control."), Chosen));
			return false;
		}
		Variable->Set(Chosen, ECVF_SetByConsole);
		const bool bMatches = Variable->GetInt() == Chosen;
		Result = FText::FromString(FString::Printf(TEXT("%s. Current value: %d"), bMatches ? TEXT("Applied") : TEXT("Not applied or overridden"), Variable->GetInt()));
		return bMatches;
	}

	FString Previous;
	if (!Read(Entry, Previous, Result)) return false;
	const FString Trimmed = Input.TrimStartAndEnd();
	double Value = 0;
	if (Trimmed.IsEmpty() || !Trimmed.IsNumeric() || !LexTryParseString(Value, *Trimmed) || !FMath::IsFinite(Value))
	{
		Result = FText::FromString(TEXT("Enter a valid, finite number."));
		return false;
	}
	if (Entry.Control != ESLGMControlType::Float && (Value != FMath::TruncToDouble(Value) || Value < MIN_int32 || Value > MAX_int32))
	{
		Result = FText::FromString(TEXT("Enter a valid 32-bit integer."));
		return false;
	}
	if ((Entry.Control == ESLGMControlType::Toggle && Value != 0 && Value != 1) ||
		(Entry.bLimitRange && (Value < Entry.Minimum || Value > Entry.Maximum)))
	{
		Result = FText::FromString(FString::Printf(TEXT("Value is outside the allowed range [%g, %g]."), Entry.Minimum, Entry.Maximum));
		return false;
	}
	if (Entry.Control == ESLGMControlType::Float && !FMath::IsFinite(static_cast<float>(Value)))
	{
		Result = FText::FromString(TEXT("Value exceeds the supported floating-point range."));
		return false;
	}
	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Entry.ConsoleVariable);
	if (Entry.Control == ESLGMControlType::Float) Variable->Set(static_cast<float>(Value), ECVF_SetByConsole);
	else Variable->Set(static_cast<int32>(Value), ECVF_SetByConsole);
	const bool bMatches = Entry.Control == ESLGMControlType::Float
		? Variable->GetFloat() == static_cast<float>(Value)
		: Variable->GetInt() == static_cast<int32>(Value);
	Result = FText::FromString(FString::Printf(TEXT("%s. Current value: %s"), bMatches ? TEXT("Applied") : TEXT("Not applied or overridden"), *Variable->GetString()));
	return bMatches;
}

bool FSLGMCommandService::ExecuteBatch(const FSLGMToolEntry& Entry, APlayerController* Player, FText& Result)
{
	if (Entry.Control != ESLGMControlType::CommandBatch || Entry.Commands.IsEmpty())
	{
		Result = FText::FromString(TEXT("The command batch is empty or the control type is incorrect."));
		return false;
	}
	// Copy before dispatch: a command may change levels or tear down the panel.
	const TArray<FString> Commands = Entry.Commands;
	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const FString Command = Commands[Index].TrimStartAndEnd();
		if (Command.IsEmpty() || Command.Contains(TEXT("\n")) || Command.Contains(TEXT("\r")) || Command.Contains(TEXT("|")))
		{
			Result = FText::FromString(FString::Printf(TEXT("Invalid entry %d: use one command per array element. No commands were executed."), Index + 1));
			return false;
		}
	}
	const TWeakObjectPtr<APlayerController> Owner = Player;
	FString Report;
	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const FString Command = Commands[Index].TrimStartAndEnd();
		const TCHAR* Arguments = *Command;
		const FString Name = FParse::Token(Arguments, false);
		const FString Input = FString(Arguments).TrimStartAndEnd();
		FText Detail;
		bool bSuccess = false;
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (Variable)
		{
			if (Input.IsEmpty())
			{
				bSuccess = true;
				Detail = FText::FromString(Variable->GetString());
			}
			else if (Variable->IsVariableInt() || Variable->IsVariableFloat())
			{
				FSLGMToolEntry Parameter;
				Parameter.ConsoleVariable = Name;
				Parameter.Control = Variable->IsVariableInt() ? ESLGMControlType::Integer : ESLGMControlType::Float;
				Parameter.bLimitRange = false;
				bSuccess = Apply(Parameter, Input, Detail);
			}
			else if (Variable->TestFlags(ECVF_ReadOnly))
			{
				Detail = FText::FromString(TEXT("This console variable is read-only and cannot be changed at runtime."));
			}
			else
			{
				const FString Value = Input.TrimQuotes();
				Variable->Set(*Value, ECVF_SetByConsole);
				bSuccess = Variable->GetString() == Value;
				Detail = FText::FromString(FString::Printf(TEXT("%s. Current value: %s"), bSuccess ? TEXT("Applied") : TEXT("Not applied or overridden"), *Variable->GetString()));
			}
		}
		else
		{
			FStringOutputDevice Output;
			UWorld* World = Owner.IsValid() ? Owner->GetWorld() : nullptr;
			bSuccess = IConsoleManager::Get().ProcessUserConsoleInput(*Command, Output, World);
			if (!bSuccess && Owner.IsValid() && Owner->GetLocalPlayer())
				bSuccess = Owner->GetLocalPlayer()->Exec(World, *Command, Output);
			Detail = FText::FromString((bSuccess ? FString(TEXT("Submitted")) : FString(TEXT("Command not recognized or not handled")))
				+ (Output.IsEmpty() ? FString() : TEXT(": ") + FString(Output)));
		}
		Report += FString::Printf(TEXT("%d. %s\n%s\n"), Index + 1, *Command, *Detail.ToString());
		if (!bSuccess)
		{
			Result = FText::FromString(Report + FString::Printf(TEXT("Stopped: %d earlier commands executed; %d remaining commands skipped. Earlier changes were not rolled back."), Index, Commands.Num() - Index - 1));
			return false;
		}
	}
	Result = FText::FromString(Report + FString::Printf(TEXT("Processed all %d commands."), Commands.Num()));
	return true;
}
