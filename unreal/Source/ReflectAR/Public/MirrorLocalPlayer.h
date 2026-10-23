#pragma once

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "MirrorLocalPlayer.generated.h"

/**
 * Replaces the player camera's view and projection with AReflectARMirror's: the webcam's pose
 * and intrinsics (CameraFeed), or an off-axis frustum from the tracked eye through the physical
 * screen (Mirror). Set as LocalPlayerClassName in Config/DefaultEngine.ini.
 */
UCLASS()
class REFLECTAR_API UMirrorLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	virtual bool GetProjectionData(FViewport* Viewport, FSceneViewProjectionData& ProjectionData, int32 StereoViewIndex) const override;
};
