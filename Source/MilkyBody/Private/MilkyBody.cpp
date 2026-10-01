#include "MilkyBody.h"
#include "MilkyBodyPrivate.h"
#include "Misc/CoreDelegates.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FMilkyBodyModule"

DEFINE_LOG_CATEGORY(LogMilkyBody);

TAutoConsoleVariable<int32> CVarMilkyBodyEnable(
	TEXT("milkybody.Enable"),
	1,
	TEXT("0: Bypass MilkyBody entirely and fall back to the engine's default skinning. 1: Enabled (default)."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarMilkyBodyDebugDraw(
	TEXT("milkybody.DebugDraw"),
	0,
	TEXT("1: Force MilkyBody debug drawing on for every deformer and BoneSpherePuni component."),
	ECVF_Cheat);

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
