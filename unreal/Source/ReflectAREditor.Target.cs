using UnrealBuildTool;

public class ReflectAREditorTarget : TargetRules
{
	public ReflectAREditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ReflectAR");
	}
}
