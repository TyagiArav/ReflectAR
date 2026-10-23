using UnrealBuildTool;

public class ReflectAR : ModuleRules
{
	public ReflectAR(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "ProceduralMeshComponent",
		});
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Sockets", "Networking", "Json", "ImageWrapper", "RenderCore", "RHI", "SlateCore",
		});
	}
}
