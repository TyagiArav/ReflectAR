using UnrealBuildTool;

public class ReflectARTarget : TargetRules
{
	public ReflectARTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("ReflectAR");
	}
}
