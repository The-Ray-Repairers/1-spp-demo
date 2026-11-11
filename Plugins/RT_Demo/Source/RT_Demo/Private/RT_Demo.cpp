#include "RT_Demo.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogRTDemo);

/**
 * Registers the plugin's shader directory as /Plugin/RT_Demo.
 * The module loads at PostConfigInit so the mapping exists before the engine
 * compiles the global shaders declared with IMPLEMENT_GLOBAL_SHADER.
 */
class FRTDemoModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RT_Demo"));
		check(Plugin.IsValid());
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/RT_Demo"),
			FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
	}
};

IMPLEMENT_MODULE(FRTDemoModule, RT_Demo)
