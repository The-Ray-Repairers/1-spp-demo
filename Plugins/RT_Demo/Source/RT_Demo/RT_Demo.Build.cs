using System.IO;
using UnrealBuildTool;

public class RT_Demo : ModuleRules
{
	public RT_Demo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The scene view extension passes use renderer-private types
		// (FPostProcessingInputs, FSceneTextures, custom render passes).
		PrivateIncludePaths.AddRange(new string[] {
			Path.Combine(GetModuleDirectory("Renderer"), "Private"),
			Path.Combine(GetModuleDirectory("Renderer"), "Internal")
		});

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",               // USLLiveComparisonUserWidget
			"DeveloperSettings"  // USLGMToolSettings
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"InputCore",
			"Slate",
			"SlateCore",
			"Projects",          // IPluginManager, to locate the plugin's Shaders directory
			"RenderCore",
			"Renderer",          // FRDGBuilder, FScreenPassTexture, AddDrawScreenPass, FPostProcessingInputs
			"RHI",               // TStaticSamplerState, FRHICopyTextureInfo
			"NNE",               // neural denoisers (runtime NNERuntimeORTDml comes from the NNERuntimeORT plugin)
			"LevelSequence",     // frame-addressed camera evaluation
			"MovieScene",        // sequence range and binding validation
			"MovieSceneTracks",
			"Json",              // flythrough run manifests
			"ImageWrapper"       // PNG loading for flythrough metric computation
		});

		if (Target.bBuildEditor)
		{
			// Widget authoring command and editor-context automation tests.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "UMGEditor", "KismetCompiler", "AssetRegistry" });
		}

		// NVIDIA DLSS is optional. When the DLSS plugin is installed the Lumen + DLSS Ray
		// Reconstruction profile is compiled in; otherwise that profile reports itself as
		// unavailable and everything else works unchanged. RT_Demo.uplugin lists DLSS as an
		// optional dependency, so it is enabled automatically whenever it is present.
		bool bWithDLSS = Target.Platform == UnrealTargetPlatform.Win64
			&& UnrealBuildTool.Plugins.GetPlugin("DLSS") != null;
		PrivateDefinitions.Add("RT_DEMO_WITH_DLSS=" + (bWithDLSS ? "1" : "0"));
		if (bWithDLSS)
		{
			PrivateDependencyModuleNames.Add("DLSSBlueprint");
		}
	}
}
