// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class SL_Test : ModuleRules
{
	public SL_Test(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "UMGEditor", "KismetCompiler", "AssetRegistry" });
        }
        PrivateIncludePaths.AddRange(new string[] {
            Path.Combine(GetModuleDirectory("Renderer"), "Private"),
            Path.Combine(GetModuleDirectory("Renderer"), "Internal")
        });

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			"NNE"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"SlateCore",
			"DeveloperSettings",
			"RenderCore",
			"Renderer",   // FRDGBuilder, FScreenPassTexture, AddDrawScreenPass, FPostProcessingInputs
			"RHI",        // TStaticSamplerState, FRHICopyTextureInfo
			"DLSSBlueprint",
			"ImageWrapper", // PNG loading for flythrough metric computation
			"ImageCore",    // FImage / FImageView for flythrough HDR (EXR) frames
		});

		PublicIncludePaths.AddRange(new string[] {
			"SL_Test",
			"SL_Test/Variant_Platforming",
			"SL_Test/Variant_Platforming/Animation",
			"SL_Test/Variant_Combat",
			"SL_Test/Variant_Combat/AI",
			"SL_Test/Variant_Combat/Animation",
			"SL_Test/Variant_Combat/Gameplay",
			"SL_Test/Variant_Combat/Interfaces",
			"SL_Test/Variant_Combat/UI",
			"SL_Test/Variant_SideScrolling",
			"SL_Test/Variant_SideScrolling/AI",
			"SL_Test/Variant_SideScrolling/Gameplay",
			"SL_Test/Variant_SideScrolling/Interfaces",
			"SL_Test/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
