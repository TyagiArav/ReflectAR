#include "MirrorLocalPlayer.h"
#include "ReflectARMirror.h"
#include "SceneView.h"

bool UMirrorLocalPlayer::GetProjectionData(FViewport* Viewport, FSceneViewProjectionData& ProjectionData, int32 StereoViewIndex) const
{
	if (!Super::GetProjectionData(Viewport, ProjectionData, StereoViewIndex)) return false;
	const AReflectARMirror* Mirror = AReflectARMirror::Get(GetWorld());
	if (!Mirror || !Mirror->View().bValid) return true;
	const FMirrorView& V = Mirror->View();
	ProjectionData.ViewOrigin = V.Origin;
	ProjectionData.ViewRotationMatrix = V.Rotation;
	ProjectionData.ProjectionMatrix = V.Projection;
	return true;
}
