#if WITH_EDITOR
#include "SLLiveComparisonUserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "SL_Test.h"

// Explicit editor-only authoring action. Never creates or rewrites assets at game startup.
static FAutoConsoleCommand CreateComparisonWidget(
    TEXT("SL.Comparison.CreateWidgetBlueprint"),
    TEXT("Create an editable UMG comparison widget and assign it to a GM config. Args: /Game/.../WBP_Name /Game/.../Config"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
    {
        if (Args.Num() != 2 || !FPackageName::IsValidLongPackageName(Args[0]))
        { UE_LOG(LogSL_Test, Error, TEXT("Expected widget package path and existing GM configuration asset path.")); return; }
        auto* Config = LoadObject<USLGMToolConfig>(nullptr, *Args[1]);
        if (!Config) { UE_LOG(LogSL_Test, Error, TEXT("GM configuration not found.")); return; }
        const FString ObjectPath = Args[0] + TEXT(".") + FPackageName::GetLongPackageAssetName(Args[0]);
        auto* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (!Blueprint)
        {
            if (FPackageName::DoesPackageExist(Args[0]))
            { UE_LOG(LogSL_Test, Error, TEXT("Destination exists and is not a Widget Blueprint.")); return; }
            UPackage* Package = CreatePackage(*Args[0]);
            Blueprint = CastChecked<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
                USLLiveComparisonUserWidget::StaticClass(), Package, *FPackageName::GetLongPackageAssetName(Args[0]),
                BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
            USLLiveComparisonUserWidget::BuildDefaultLayout(Blueprint->WidgetTree);
            FAssetRegistryModule::AssetCreated(Blueprint);
        }
        if (!Blueprint->ParentClass->IsChildOf(USLLiveComparisonUserWidget::StaticClass()))
        { UE_LOG(LogSL_Test, Error, TEXT("Existing widget has an incompatible parent class.")); return; }
        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
        if (Results.NumErrors > 0) { UE_LOG(LogSL_Test, Error, TEXT("Comparison Widget Blueprint failed to compile.")); return; }
        auto Save = [](UObject* Asset) {
            UPackage* Package = Asset->GetOutermost();
            Package->MarkPackageDirty();
            FSavePackageArgs Options;
            Options.TopLevelFlags = RF_Public | RF_Standalone;
            return UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(
                Package->GetName(), FPackageName::GetAssetPackageExtension()), Options);
        };
        if (!Save(Blueprint)) { UE_LOG(LogSL_Test, Error, TEXT("Unable to save comparison widget.")); return; }
        Config->LiveComparisonWidgetClass = Blueprint->GeneratedClass;
        if (!Save(Config)) { UE_LOG(LogSL_Test, Error, TEXT("Unable to save comparison configuration.")); return; }
        UE_LOG(LogSL_Test, Display, TEXT("Comparison UMG authored and configured: %s"), *ObjectPath);
    }));
#endif
