using UnrealBuildTool;
using System.IO;

public class MilkyBody : ModuleRules
{
	public MilkyBody(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Engine",
			"RenderCore",
			"Projects",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"RHI",
			"Renderer",
			"Slate",
			"SlateCore",
		});

		// Register shader source directory
		PrivateIncludePathModuleNames.AddRange(new string[]
		{
			"Renderer",
		});
	}
}
