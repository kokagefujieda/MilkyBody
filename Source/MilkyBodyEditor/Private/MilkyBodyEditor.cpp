#include "MilkyBodyEditor.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "MilkyBodyDeformerAssetActions.h"

#define LOCTEXT_NAMESPACE "FMilkyBodyEditorModule"

void FMilkyBodyEditorModule::StartupModule()
{
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	TSharedRef<IAssetTypeActions> MilkyBodyDeformerAction = MakeShared<FMilkyBodyDeformerAssetActions>();
	AssetTools.RegisterAssetTypeActions(MilkyBodyDeformerAction);
	RegisteredAssetTypeActions.Add(MilkyBodyDeformerAction);
}

void FMilkyBodyEditorModule::ShutdownModule()
{
	if (FAssetToolsModule* AssetToolsModule = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools"))
	{
		IAssetTools& AssetTools = AssetToolsModule->Get();
		for (const TSharedRef<IAssetTypeActions>& Action : RegisteredAssetTypeActions)
		{
			AssetTools.UnregisterAssetTypeActions(Action);
		}
	}
	RegisteredAssetTypeActions.Empty();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMilkyBodyEditorModule, MilkyBodyEditor)
