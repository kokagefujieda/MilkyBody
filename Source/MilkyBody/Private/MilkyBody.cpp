#include "MilkyBody.h"
#include "Misc/CoreDelegates.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FMilkyBodyModule"

void FMilkyBodyModule::StartupModule()
{
	// Register shader directory mapping immediately
	// Use IPluginManager to find plugin base directory
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MilkyBody"));
	if (Plugin.IsValid())
	{
		const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/MilkyBodyShaders"), PluginShaderDir);
	}
}

void FMilkyBodyModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMilkyBodyModule, MilkyBody)
