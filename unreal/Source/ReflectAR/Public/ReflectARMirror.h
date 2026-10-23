#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BodyRig.h"
#include "BodyProxy.h"
#include "ClothGarment.h"
#include "TrackingClient.h"
#include "ReflectARMirror.generated.h"

class UProceduralMeshComponent;
class UDirectionalLightComponent;
class UPostProcessComponent;
class UStaticMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;

UENUM()
enum class EMirrorViewMode : uint8 { CameraFeed, Mirror };

/** The camera this frame, for UMirrorLocalPlayer (Unreal world) and the HUD (body coords). */
struct FMirrorView
{
	bool bValid = false;
	FVector Origin = FVector::ZeroVector;          // cm, Unreal world
	FMatrix Rotation = FMatrix::Identity;          // world -> view (x right, y up, z forward)
	FMatrix Projection = FMatrix::Identity;        // reversed-Z, off-axis
	// the same in body coords, to project points for the HUD
	ReflectAR::V3 EyeB, RightB, UpB, FwdB;
	float P00 = 1, P20 = 0, P11 = 1, P21 = 0;
};

/**
 * Top-level controller: camera modes, body proxy, garment catalogue, gestures, lighting.
 *
 * CameraFeed: a "digital mirror" for the laptop. A virtual camera with the webcam's intrinsics
 *   sits at the webcam's pose; the image is mirrored and garments are drawn over the live feed.
 * Mirror: what the two-way-mirror build needs. Black background, garments placed on the
 *   reflected body (MirrorWorld mirrored in body z), rendered with an off-axis frustum from the
 *   tracked eye through the physical screen rectangle.
 *
 * Everything body-related lives under MirrorWorld, whose transform maps body coordinates (m) to
 * Unreal world space. The materials come from Content/Python/reflectar_setup.py.
 */
UCLASS()
class REFLECTAR_API AReflectARMirror : public AActor
{
	GENERATED_BODY()

public:
	AReflectARMirror();
	static AReflectARMirror* Get(const UWorld* World);

	UPROPERTY(EditAnywhere, Category = "ReflectAR") EMirrorViewMode Mode = EMirrorViewMode::CameraFeed;
	UPROPERTY(EditAnywhere, Category = "ReflectAR") bool bShowProxy = false;
	UPROPERTY(EditAnywhere, Category = "ReflectAR") bool bShowDebug = true;
	UPROPERTY(EditAnywhere, Category = "ReflectAR") bool bCubeTest = false;
	/** Extrapolate the pose to when each frame reaches the screen (P toggles, for A/B). */
	UPROPERTY(EditAnywhere, Category = "ReflectAR") bool bPredict = true;
	/** Extra prediction lead (ms) on top of config.json's latencies, Mirror mode. Tune with [ and ]. */
	UPROPERTY(EditAnywhere, Category = "ReflectAR") float LeadMs = 0.f;
	/** Trim the hands / bare arms / head occluders to perception's person mask (O toggles). */
	UPROPERTY(EditAnywhere, Category = "ReflectAR") bool bTrimToMask = true;

	// physical lighting: lux, with a camera exposure of EV100 ~10 (ISO 100, 1/60 s, f/4)
	UPROPERTY(EditAnywhere, Category = "ReflectAR|Look") float KeyLux = 3000.f;
	UPROPERTY(EditAnywhere, Category = "ReflectAR|Look") float FillLux = 700.f;
	/** Ambient light on the fabric (luminance of a white surface, cd/m2): the room's bounce light. */
	UPROPERTY(EditAnywhere, Category = "ReflectAR|Look") float Ambient = 120.f;

	// ---- state the HUD draws
	ReflectAR::FTrackingClient* Client() const { return Tracking.Get(); }
	const ReflectAR::FBodyRig& Body() const { return Rig; }
	int32 Current = 0, Selected = -1;
	bool bTracked = false;
	FString LatencyLine;
	FString ToastText;
	double ToastTime = -10;
	float RenderFps = 0;
	const FMirrorView& View() const { return MirrorView; }
	/** Body point (unreflected) -> screen pixels, or false if behind the camera. */
	bool BodyToScreen(const ReflectAR::V3& P, const FVector2D& ViewportSize, FVector2D& Out) const;
	UTexture2D* Swatch(int32 I) const;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	struct FGarmentSlot
	{
		TUniquePtr<ReflectAR::FClothGarment> Sim;
		UProceduralMeshComponent* Mesh = nullptr;
		UProceduralMeshComponent* Trims = nullptr;
		UMaterialInstanceDynamic* Opaque[3] = {};   // fabric, plastic, metal
		UMaterialInstanceDynamic* Fade[3] = {};
		int bFading = -1;
		bool bHasTrims = false;
	};

	UPROPERTY() USceneComponent* MirrorWorld = nullptr;
	UPROPERTY() UProceduralMeshComponent* ProxyMesh = nullptr;
	UPROPERTY() UProceduralMeshComponent* FeedQuad = nullptr;
	UPROPERTY() UStaticMeshComponent* Cube = nullptr;
	UPROPERTY() UDirectionalLightComponent* KeyLight = nullptr;
	UPROPERTY() UDirectionalLightComponent* FillLight = nullptr;
	UPROPERTY() UPostProcessComponent* PostProcess = nullptr;
	UPROPERTY() TArray<TObjectPtr<UObject>> Keep;          // components and materials owned here
	UPROPERTY() UMaterialParameterCollection* Params = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* ProxyMat = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* FeedMat = nullptr;

	TUniquePtr<ReflectAR::FTrackingClient> Tracking;
	ReflectAR::FBodyRig Rig;
	ReflectAR::FBodyShape Shape;
	ReflectAR::FBodyProxy Proxy;
	ReflectAR::V3 MaskJoints[ReflectAR::LM::Count];
	TArray<FGarmentSlot> Garments;
	bool bProxyCreated = false, bFeedCreated = false;
	float Visible = 0.f;
	bool bHadBody = false;
	double LastTracked = -10;
	FMirrorView MirrorView;
	double StatT = 0; float StatWorst = 0, StatSum = 0, StatCloth = 0; int StatN = 0, StatRebuilds = 0;

	UMaterialInterface* LoadMat(const TCHAR* Name) const;
	void CreateGarments();
	void HandleInput();
	void Browse(int D);
	void Select();
	void Toast(const FString& S) { ToastText = S; ToastTime = ReflectAR::FTrackingClient::Now(); }
	void UploadGarment(FGarmentSlot& G);
	void ApplyMaterials(FGarmentSlot& G, float Alpha);
	void UpdateProxy(bool bOn, bool bMaskOn);
	void UpdateFeedQuad();
	void UpdateCamera();
	void SetParams(bool bMaskOn);
	void LogStats(float Dt);
};
