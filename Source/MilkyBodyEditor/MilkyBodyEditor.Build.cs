using UnrealBuildTool;

public class MilkyBodyEditor : ModuleRules
{
	public MilkyBodyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"MilkyBody",
			"UnrealEd",
			"AssetTools",
			"Slate",
			"SlateCore",
		});
	}
}
