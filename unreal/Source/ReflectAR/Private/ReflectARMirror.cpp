#include "ReflectARMirror.h"
#include "ProceduralMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

using namespace ReflectAR;

static const TCHAR* MatDir = TEXT("/Game/ReflectAR/");

AReflectARMirror::AReflectARMirror()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	MirrorWorld = CreateDefaultSubobject<USceneComponent>(TEXT("MirrorWorld"));
	MirrorWorld->SetupAttachment(RootComponent);
	ProxyMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BodyProxy"));
	ProxyMesh->SetupAttachment(MirrorWorld);
	FeedQuad = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("CameraFeed"));
	FeedQuad->SetupAttachment(MirrorWorld);
	Cube = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CubeTest"));
	Cube->SetupAttachment(MirrorWorld);
	KeyLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("KeyLight"));
	KeyLight->SetupAttachment(RootComponent);
	FillLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("FillLight"));
	FillLight->SetupAttachment(RootComponent);
	PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Look"));
	PostProcess->SetupAttachment(RootComponent);
	for (V3& j : MaskJoints) j = V3::ZeroVector;
}

AReflectARMirror* AReflectARMirror::Get(const UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AReflectARMirror> It(const_cast<UWorld*>(World)); It; ++It) return *It;
	return nullptr;
}

UMaterialInterface* AReflectARMirror::LoadMat(const TCHAR* Name) const
{
	const FString Path = FString(MatDir) + Name + TEXT(".") + Name;
	UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path);
	if (!M)
	{
		UE_LOG(LogTemp, Error, TEXT("ReflectAR: %s is missing. Run Content/Python/reflectar_setup.py in the editor (Tools > Execute Python Script)."), *Path);
		M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}
	return M;
}

// ======================================================================== setup
void AReflectARMirror::BeginPlay()
{
	Super::BeginPlay();
	Tracking = MakeUnique<FTrackingClient>();
	Params = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/ReflectAR/MPC_ReflectAR.MPC_ReflectAR"));

	MirrorWorld->SetRelativeTransform(FTransform(BodyBasis(), FVector::ZeroVector, FVector(100.0)));

	// ---- look: physical units, a fixed camera exposure, no tone curve (the camera feed passes
	// through unchanged), no bloom / vignette / blur, so black stays black on the mirror
	FPostProcessSettings& S = PostProcess->Settings;
	PostProcess->bUnbound = true;
	S.bOverride_AutoExposureMethod = true; S.AutoExposureMethod = AEM_Manual;
	S.bOverride_AutoExposureApplyPhysicalCameraExposure = true; S.AutoExposureApplyPhysicalCameraExposure = 1;
	S.bOverride_CameraISO = true; S.CameraISO = 100.f;
	S.bOverride_CameraShutterSpeed = true; S.CameraShutterSpeed = 60.f;
	S.bOverride_DepthOfFieldFstop = true; S.DepthOfFieldFstop = 4.f;        // EV100 = log2(4^2 * 60) ~ 9.9
	S.bOverride_AutoExposureBias = true; S.AutoExposureBias = 0.f;
	S.bOverride_ToneCurveAmount = true; S.ToneCurveAmount = 0.f;
	S.bOverride_ExpandGamut = true; S.ExpandGamut = 0.f;
	S.bOverride_BlueCorrection = true; S.BlueCorrection = 0.f;
	S.bOverride_BloomIntensity = true; S.BloomIntensity = 0.f;
	S.bOverride_VignetteIntensity = true; S.VignetteIntensity = 0.f;
	S.bOverride_MotionBlurAmount = true; S.MotionBlurAmount = 0.f;
	S.bOverride_SceneFringeIntensity = true; S.SceneFringeIntensity = 0.f;
	S.bOverride_FilmGrainIntensity = true; S.FilmGrainIntensity = 0.f;
	S.bOverride_LensFlareIntensity = true; S.LensFlareIntensity = 0.f;

	// key with soft shadows and contact shadows, plus a dim fill so shadowed folds aren't black
	for (UDirectionalLightComponent* L : { KeyLight, FillLight })
	{
		L->SetMobility(EComponentMobility::Movable);
		L->bAtmosphereSunLight = false;
	}
	KeyLight->SetIntensity(KeyLux);
	KeyLight->SetLightSourceAngle(6.f);
	KeyLight->SetCastShadows(true);
	KeyLight->ContactShadowLength = 0.03f;
	FillLight->SetIntensity(FillLux);
	FillLight->SetCastShadows(false);

	// ---- proxies, feed, cube
	ProxyMat = UMaterialInstanceDynamic::Create(LoadMat(TEXT("M_Proxy")), this);
	FeedMat = UMaterialInstanceDynamic::Create(LoadMat(TEXT("M_Feed")), this);
	ProxyMesh->SetMaterial(0, ProxyMat);
	ProxyMesh->SetCastShadow(true);                 // hands, arms and head shadow the garment
	FeedQuad->SetMaterial(0, FeedMat);
	FeedQuad->SetCastShadow(false);
	if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
	{
		Cube->SetStaticMesh(CubeMesh);
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(LoadMat(TEXT("M_Trim")), this);
		M->SetVectorParameterValue(TEXT("Tint"), FLinearColor(0.6f, 0.6f, 0.6f));
		M->SetScalarParameterValue(TEXT("Roughness"), 0.5f);
		Cube->SetMaterial(0, M);
		Keep.Add(M);
		Cube->SetRelativeScale3D(FVector(0.002));       // 0.2 m: the static cube is 100 units, MirrorWorld scales by 100
	}
	Cube->SetVisibility(false);
	CreateGarments();
}

void AReflectARMirror::EndPlay(const EEndPlayReason::Type Reason)
{
	Tracking.Reset();
	Super::EndPlay(Reason);
}

void AReflectARMirror::CreateGarments()
{
	const TArray<FGarmentSpec>& Specs = GarmentCatalog();
	UMaterialInterface* Fabric = LoadMat(TEXT("M_Fabric")), *FabricFade = LoadMat(TEXT("M_FabricFade"));
	UMaterialInterface* Trim = LoadMat(TEXT("M_Trim")), *TrimFade = LoadMat(TEXT("M_TrimFade"));
	for (int i = 0; i < Specs.Num(); i++)
	{
		const FGarmentSpec& Sp = Specs[i];
		FGarmentSlot& G = Garments.AddDefaulted_GetRef();
		G.Sim = MakeUnique<FClothGarment>(Sp, Rig);
		G.Mesh = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("Garment_%d"), i));
		G.Trims = NewObject<UProceduralMeshComponent>(this, *FString::Printf(TEXT("Trims_%d"), i));
		for (UProceduralMeshComponent* C : { G.Mesh, G.Trims })
		{
			C->SetupAttachment(MirrorWorld);
			C->RegisterComponent();
			C->SetCastShadow(true);                     // the garment shadows itself (sleeves on the torso)
			C->bUseAsyncCooking = true;
			C->SetVisibility(false);
		}
		const FFabricMaps& Maps = FFabricMaps::Get(Sp);
		for (int f = 0; f < 2; f++)
		{
			UMaterialInstanceDynamic** Set = f == 0 ? G.Opaque : G.Fade;
			Set[0] = UMaterialInstanceDynamic::Create(f == 0 ? Fabric : FabricFade, this);
			Set[0]->SetTextureParameterValue(TEXT("BaseColorMap"), Maps.BaseColor.Get());
			Set[0]->SetTextureParameterValue(TEXT("NormalMap"), Maps.Normal.Get());
			Set[0]->SetTextureParameterValue(TEXT("MaskMap"), Maps.Mask.Get());
			Set[0]->SetScalarParameterValue(TEXT("Tile"), 1.f / FMath::Max(Sp.Tile, 0.01f));   // UVs are metres of fabric
			Set[0]->SetScalarParameterValue(TEXT("Sheen"), Sp.Sheen);
			for (int k = 1; k < 3; k++)
			{
				Set[k] = UMaterialInstanceDynamic::Create(f == 0 ? Trim : TrimFade, this);
				Set[k]->SetVectorParameterValue(TEXT("Tint"), k == 1 ? Sp.Trim : Sp.Metal);
				Set[k]->SetScalarParameterValue(TEXT("Metallic"), k == 1 ? 0.f : 1.f);
				Set[k]->SetScalarParameterValue(TEXT("Roughness"), k == 1 ? 0.45f : 0.25f);
			}
			for (int k = 0; k < 3; k++) Keep.Add(Set[k]);
		}
	}
}

// ======================================================================== input
void AReflectARMirror::HandleInput()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (PC)
	{
		auto Pressed = [PC](const FKey& K) { return PC->WasInputKeyJustPressed(K); };
		if (Pressed(EKeys::Right)) Browse(+1);
		if (Pressed(EKeys::Left)) Browse(-1);
		if (Pressed(EKeys::SpaceBar)) Select();
		if (Pressed(EKeys::M)) Mode = Mode == EMirrorViewMode::CameraFeed ? EMirrorViewMode::Mirror : EMirrorViewMode::CameraFeed;
		if (Pressed(EKeys::B)) bShowProxy = !bShowProxy;
		if (Pressed(EKeys::D)) bShowDebug = !bShowDebug;
		if (Pressed(EKeys::C)) bCubeTest = !bCubeTest;
		if (Pressed(EKeys::P)) { bPredict = !bPredict; Toast(bPredict ? TEXT("Prediction on") : TEXT("Prediction off")); }
		if (Pressed(EKeys::O)) { bTrimToMask = !bTrimToMask; Toast(bTrimToMask ? TEXT("Occlusion: person mask") : TEXT("Occlusion: capsules only")); }
		if (Pressed(EKeys::LeftBracket)) LeadMs -= 5.f;
		if (Pressed(EKeys::RightBracket)) LeadMs += 5.f;
		if (Pressed(EKeys::Escape)) UKismetSystemLibrary::QuitGame(this, PC, EQuitPreference::Quit, false);
	}
	FString E;
	while (Tracking->Events.Dequeue(E))
	{
		if (E == TEXT("next")) Browse(+1);
		else if (E == TEXT("prev")) Browse(-1);
		else if (E == TEXT("select")) Select();
	}
}

void AReflectARMirror::Browse(int D)
{
	Garments[Current].Mesh->SetVisibility(false);
	Garments[Current].Trims->SetVisibility(false);
	Current = (Current + D + Garments.Num()) % Garments.Num();
	Garments[Current].Sim->ResetCloth();
}

void AReflectARMirror::Select()
{
	Selected = Current;
	Toast(FString::Printf(TEXT("Selected: %s"), *GarmentCatalog()[Current].Name));
}

UTexture2D* AReflectARMirror::Swatch(int32 I) const
{
	return FFabricMaps::Get(GarmentCatalog()[I]).BaseColor.Get();
}

// ======================================================================== mesh upload
static void ToUE(const FMeshSection& S, TArray<FVector>& V, TArray<FVector>& N, TArray<FVector2D>& UV, TArray<FProcMeshTangent>& T)
{
	const int n = S.Pos.Num();
	V.SetNumUninitialized(n); N.SetNumUninitialized(n); UV.SetNumUninitialized(n); T.SetNumUninitialized(n);
	for (int i = 0; i < n; i++)
	{
		V[i] = ToFV(S.Pos[i]);
		N[i] = ToFV(S.Nrm[i]);
		UV[i] = FVector2D(S.UV[i].X, S.UV[i].Y);
		T[i] = FProcMeshTangent(FVector(S.Tan[i].X, S.Tan[i].Y, S.Tan[i].Z), S.Tan[i].W < 0.f);
	}
}

static void Upload(UProceduralMeshComponent* C, int Index, const FMeshSection& S, bool bCreate)
{
	TArray<FVector> V, N; TArray<FVector2D> UV; TArray<FProcMeshTangent> T;
	ToUE(S, V, N, UV, T);
	if (bCreate) C->CreateMeshSection(Index, V, S.Tris, N, UV, TArray<FColor>(), T, false);
	else C->UpdateMeshSection(Index, V, N, UV, TArray<FColor>(), T);
}

void AReflectARMirror::UploadGarment(FGarmentSlot& G)
{
	FClothGarment& Sim = *G.Sim;
	const bool bNew = Sim.ConsumeTopologyChanged();
	if (bNew)
	{
		G.Mesh->ClearAllMeshSections();
		G.Trims->ClearAllMeshSections();
		G.bHasTrims = Sim.Trims.IsValid();
		G.bFading = -1;
	}
	Upload(G.Mesh, 0, Sim.BodySection, bNew);
	if (G.bHasTrims)
		for (int s = 0; s < FGarmentTrims::NumSections; s++)
			if (Sim.Trims->Sections[s].Tris.Num() > 0) Upload(G.Trims, s, Sim.Trims->Sections[s], bNew);
}

/** Opaque while fully shown (proper depth, shadows); a translucent copy while fading or dimmed. */
void AReflectARMirror::ApplyMaterials(FGarmentSlot& G, float Alpha)
{
	const bool bFade = Alpha < 0.999f;
	if (bFade)
		for (UMaterialInstanceDynamic* M : G.Fade) M->SetScalarParameterValue(TEXT("Alpha"), Alpha);
	if (G.bFading == int(bFade)) return;
	UMaterialInstanceDynamic** Set = bFade ? G.Fade : G.Opaque;
	G.Mesh->SetMaterial(0, Set[0]);
	for (int s = 0; s < 3; s++) G.Trims->SetMaterial(s, Set[s]);
	G.bFading = int(bFade);
}

// ======================================================================== frame
void AReflectARMirror::LogStats(float Dt)
{
	StatWorst = FMath::Max(StatWorst, Dt); StatSum += Dt; StatN++;
	const FClothGarment& Cg = *Garments[Current].Sim;
	StatCloth = FMath::Max(StatCloth, Cg.LastStepMs);
	RenderFps = Dt > 0 ? FMath::Lerp(RenderFps, 1.f / Dt, 0.1f) : RenderFps;
	const double NowT = FTrackingClient::Now();
	if (NowT - StatT < 3.0) return;
	int Rb = 0;
	for (const FGarmentSlot& G : Garments) Rb += G.Sim->RebuildCount;
	UE_LOG(LogTemp, Log, TEXT("[ReflectAR stats] avg %.0f fps, worst frame %.0f ms, cloth max %.1f ms, rebuilds +%d (last %.0f ms), verts %d"),
		StatN / FMath::Max(StatSum, 1e-3f), StatWorst * 1000.f, StatCloth, Rb - StatRebuilds, Cg.LastBuildMs, Cg.VertexCount());
	StatT = NowT; StatWorst = StatSum = StatCloth = 0; StatN = 0; StatRebuilds = Rb;
}

// Camera -> Mirror rotation, identical to perception/geometry.py: flip(-1, -1, 1) * tilt about x
static V3 CamToMirror(const FCamInfo& Ci, const V3& v)
{
	const float Th = FMath::DegreesToRadians(Ci.tilt), c = FMath::Cos(Th), s = FMath::Sin(Th);
	const V3 t(v.X, c * v.Y + s * v.Z, -s * v.Y + c * v.Z);
	return V3(-t.X, -t.Y, t.Z);
}
static V3 MirrorToCam(const FCamInfo& Ci, const V3& m)
{
	const float Th = FMath::DegreesToRadians(Ci.tilt), c = FMath::Cos(Th), s = FMath::Sin(Th);
	const V3 t(-m.X, -m.Y, m.Z);                       // flip is its own inverse
	return V3(t.X, c * t.Y - s * t.Z, s * t.Y + c * t.Z);   // tilt transposed
}

/** Body coords (unreflected) -> camera image (u / w, v / h) * depth: three affine rows. */
static void MaskRows(const FCamInfo& Ci, FVector4f Rows[3])
{
	auto Q = [&Ci](const V3& p)
	{
		const V3 Pc = MirrorToCam(Ci, V3(p.X, p.Y, -p.Z) - Ci.PosMirror);
		return V3(Ci.fx / Ci.w * Pc.X + Ci.cx / Ci.w * Pc.Z, Ci.fy / Ci.h * Pc.Y + Ci.cy / Ci.h * Pc.Z, Pc.Z);
	};
	const V3 O = Q(V3::ZeroVector), X = Q(VRight) - O, Y = Q(VUp) - O, Z = Q(VFwd) - O;
	Rows[0] = FVector4f(X.X, Y.X, Z.X, O.X);
	Rows[1] = FVector4f(X.Y, Y.Y, Z.Y, O.Y);
	Rows[2] = FVector4f(X.Z, Y.Z, Z.Z, O.Z);
}

static V3 CamPosBody(const FCamInfo& Ci) { return V3(Ci.PosMirror.X, Ci.PosMirror.Y, -Ci.PosMirror.Z); }

void AReflectARMirror::Tick(float Dt)
{
	Super::Tick(Dt);
	if (Garments.Num() == 0) return;
	LogStats(Dt);
	Tracking->Poll();
	HandleInput();
	const double NowT = FTrackingClient::Now();
	const TSharedPtr<FTrackingFrame> F = Tracking->Latest;
	const bool bFresh = Tracking->IsConnected() && F.IsValid() && NowT - Tracking->LastFrameTime < 0.5;
	bTracked = bFresh && F->bTracked;
	if (GEngine) GEngine->SetMaxFPS(bTracked || Visible > 0.f ? 60.f : 30.f);
	if (bTracked)
	{
		// Mirror: the real reflection has no delay, so aim for the moment this frame is on screen.
		// CameraFeed: match the (equally delayed) image under the overlay instead.
		const bool bMatchFeed = Mode == EMirrorViewMode::CameraFeed && NowT - Tracking->FeedCaptureTime < 0.5;
		const double Target = bMatchFeed ? Tracking->FeedCaptureTime : NowT + Tracking->DisplayLatency() + LeadMs * 0.001;
		Tracking->Predict(Target, bPredict, bPredict && !bMatchFeed);
		if (!bHadBody) Shape.Reset(Rig);                  // a new person: measure them from scratch
		Rig.Update(Tracking->Joints, Tracking->Visibility, Dt, !bHadBody || NowT - LastTracked > 0.5);
		bHadBody = true;
		LastTracked = NowT;
	}
	// hold the last pose 0.5 s, then fade over 0.3 s; dim while out of range
	const float FadeTo = !bHadBody || NowT - LastTracked > 0.5 ? 0.f : (F.IsValid() && F->Range == TEXT("ok") ? 1.f : 0.35f);
	Visible = FMath::FInterpConstantTo(Visible, FadeTo, Dt, 1.f / 0.3f);
	if (Visible <= 0.f && NowT - LastTracked > 0.8) bHadBody = false;

	const bool bMirror = Mode == EMirrorViewMode::Mirror;
	MirrorWorld->SetRelativeScale3D(FVector(100.0, 100.0, bMirror ? -100.0 : 100.0));   // body z, after the basis change
	// key light from above, beside the screen, falling on the (real or reflected) front of the body
	const float Zs = bMirror ? 1.f : -1.f;
	KeyLight->SetWorldRotation(BodyDirToUE(V3(0.55f, -0.6f, Zs)).Rotation());
	FillLight->SetWorldRotation(BodyDirToUE(V3(-0.7f, -0.2f, Zs)).Rotation());

	// ---- garments: background builds for all, so swiping never waits; simulate the one worn
	if (Rig.Valid) for (FGarmentSlot& G : Garments) G.Sim->Prepare(NowT);
	FGarmentSlot& G = Garments[Current];
	const bool bShow = Visible > 0.001f && Rig.Valid;
	if (bShow)
	{
		G.Sim->Alpha = Visible;
		G.Sim->Step(Dt, NowT);
		if (G.Sim->HasMesh()) { UploadGarment(G); ApplyMaterials(G, Visible); }
	}
	G.Mesh->SetVisibility(bShow && G.Sim->HasMesh());
	G.Trims->SetVisibility(bShow && G.Sim->HasMesh() && G.bHasTrims);

	// ---- person mask: trims the occluders, and measures the user's build a few times a second
	const bool bMaskFresh = bTracked && Tracking->Mask() && NowT - Tracking->MaskCaptureTime < 0.3;
	const bool bMaskOn = bTrimToMask && bMaskFresh;
	if (bMaskFresh)
	{
		Tracking->PoseAt(Tracking->MaskCaptureTime, MaskJoints);
		if (F->Range == TEXT("ok") && GFrameCounter % 4 == 0 && F->Cam.Valid())
		{
			FVector4f Rows[3];
			MaskRows(F->Cam, Rows);
			Shape.Measure(Tracking->MaskBytes, Tracking->MaskW, Tracking->MaskH, Rows, MaskJoints, Rig, CamPosBody(F->Cam), Dt * 4.f);
		}
	}
	UpdateProxy(Rig.Valid && Visible > 0.f, bMaskOn);
	UpdateFeedQuad();
	Cube->SetVisibility(bCubeTest && bMirror);
	if (bCubeTest && bMirror && F.IsValid())
	{
		// MirrorWorld mirrors body z, so the cube sits at the reflected 0.75 m
		Cube->SetRelativeLocation(ToFV(V3(F->Screen.WM / 2, F->Screen.HM / 2, -0.75f)));
		Cube->SetRelativeRotation(FRotator(20, 30, 0));
	}
	UpdateCamera();
	SetParams(bMaskOn);
	LatencyLine = FString::Printf(TEXT("latency %.0f ms  ·  prediction %s  lead %+.0f ms  [P] [ [ ] ]  ·  occlusion %s [O]"),
		Tracking->LatencyMs, bPredict ? *FString::Printf(TEXT("+%.0f ms"), Tracking->HorizonMs) : TEXT("off"), LeadMs,
		bMaskOn ? TEXT("person mask") : TEXT("capsules"));
}

void AReflectARMirror::UpdateProxy(bool bOn, bool bMaskOn)
{
	const FGarmentSpec& Sp = GarmentCatalog()[Current];
	Proxy.Update(Rig, bOn, Sp.bTank ? 0.f : Sp.Sleeve, bMaskOn ? MaskJoints : nullptr, Garments[Current].Sim.Get());
	ProxyMesh->SetVisibility(Proxy.bVisible);
	if (!Proxy.bVisible) return;
	TArray<FVector> V, N; TArray<FVector2D> UV, U1, U2, U3; TArray<FProcMeshTangent> T;
	ToUE(Proxy.Mesh, V, N, UV, T);
	auto Conv = [](const TArray<V2>& In, TArray<FVector2D>& Out) { Out.SetNumUninitialized(In.Num()); for (int i = 0; i < In.Num(); i++) Out[i] = FVector2D(In[i].X, In[i].Y); };
	Conv(Proxy.UV1, U1); Conv(Proxy.UV2, U2); Conv(Proxy.UV3, U3);
	if (!bProxyCreated) { ProxyMesh->CreateMeshSection(0, V, Proxy.Mesh.Tris, N, UV, U1, U2, U3, TArray<FColor>(), T, false); bProxyCreated = true; }
	else ProxyMesh->UpdateMeshSection(0, V, N, UV, U1, U2, U3, TArray<FColor>(), T);
	ProxyMat->SetTextureParameterValue(TEXT("MaskTex"), Tracking->Mask());
	ProxyMat->SetTextureParameterValue(TEXT("FeedTex"), Tracking->Feed());
}

void AReflectARMirror::UpdateFeedQuad()
{
	const TSharedPtr<FTrackingFrame> F = Tracking->Latest;
	const bool bOn = Mode == EMirrorViewMode::CameraFeed && F.IsValid() && F->Cam.Valid() && Tracking->Feed();
	FeedQuad->SetVisibility(bOn);
	if (!bOn) return;
	FeedMat->SetTextureParameterValue(TEXT("FeedTex"), Tracking->Feed());
	FeedMat->SetScalarParameterValue(TEXT("Dim"), F->Range == TEXT("ok") || !F->bTracked ? 1.f : 0.6f);
	const FCamInfo& Ci = F->Cam;
	constexpr float D = 15.f;
	const V2 Px[4] = { V2(0, 0), V2(Ci.w, 0), V2(Ci.w, Ci.h), V2(0, Ci.h) };
	FMeshSection Q;
	for (int i = 0; i < 4; i++)
	{
		const V3 Pc((Px[i].X - Ci.cx) * D / Ci.fx, (Px[i].Y - Ci.cy) * D / Ci.fy, D);
		const V3 Pm = CamToMirror(Ci, Pc) + Ci.PosMirror;
		Q.Pos.Add(V3(Pm.X, Pm.Y, -Pm.Z));
		Q.Nrm.Add(-VFwd);
		Q.Tan.Add(V4(1, 0, 0, 1));
		Q.UV.Add(V2(Px[i].X / Ci.w, Px[i].Y / Ci.h));    // texture row 0 = image top
	}
	Q.Tris = { 0, 1, 2, 0, 2, 3 };
	Upload(FeedQuad, 0, Q, !bFeedCreated);
	bFeedCreated = true;
}

// ======================================================================== camera
static FMatrix ViewRotation(const FVector& R, const FVector& U, const FVector& Fw)
{
	// world -> view, row vectors: v_view = v_world * M, so the columns are the view axes
	return FMatrix(FPlane(R.X, U.X, Fw.X, 0), FPlane(R.Y, U.Y, Fw.Y, 0), FPlane(R.Z, U.Z, Fw.Z, 0), FPlane(0, 0, 0, 1));
}

/** Reversed-Z, infinite far: clip = (P00 x + P20 z, P11 y + P21 z, near, z). */
static FMatrix Projection(float P00, float P20, float P11, float P21, float NearCm)
{
	return FMatrix(FPlane(P00, 0, 0, 0), FPlane(0, P11, 0, 0), FPlane(P20, P21, 0, 1), FPlane(0, 0, NearCm, 0));
}

void AReflectARMirror::UpdateCamera()
{
	const TSharedPtr<FTrackingFrame> F = Tracking->Latest;
	if (!F.IsValid() || !F->Cam.Valid()) { MirrorView.bValid = false; return; }
	constexpr float Near = 0.05f;
	FMirrorView& V = MirrorView;
	if (Mode == EMirrorViewMode::CameraFeed)
	{
		// the webcam's pose and intrinsics; the image is mirrored by flipping the view's x axis,
		// so Unreal sees a reflected view and flips culling itself
		const FCamInfo& Ci = F->Cam;
		auto Dir = [&Ci](const V3& c) { const V3 m = CamToMirror(Ci, c); return V3(m.X, m.Y, -m.Z); };
		V.EyeB = CamPosBody(Ci);
		V.RightB = -Dir(VRight);
		V.UpB = Dir(-VUp);                                // camera y points down
		V.FwdB = Dir(VFwd);
		V.P00 = 2.f * Ci.fx / Ci.w; V.P20 = 1.f - 2.f * Ci.cx / Ci.w;
		V.P11 = 2.f * Ci.fy / Ci.h; V.P21 = 1.f - 2.f * Ci.cy / Ci.h;
	}
	else
	{
		// off-axis frustum from the eye through the screen rectangle (Kooima)
		const FScreenInfo& Sc = F->Screen;
		const V3 Eye = bHadBody ? Tracking->Eye : V3(Sc.WM / 2, Sc.HM / 2 + 0.25f, -1.5f);
		const V3 Pa = V3::ZeroVector, Pb(Sc.WM, 0, 0), Pc(0, Sc.HM, 0);
		const V3 Vr = Normalized(Pb - Pa), Vu = Normalized(Pc - Pa);
		const V3 Vn = Normalized(Cross(Vu, Vr));          // toward the viewer (body z < 0)
		const V3 Va = Pa - Eye, Vb = Pb - Eye, Vc = Pc - Eye;
		const float D = -Dot(Va, Vn);
		if (D < 0.05f) return;                            // keep last frame's view
		const float L = Dot(Vr, Va) * Near / D, R = Dot(Vr, Vb) * Near / D;
		const float B = Dot(Vu, Va) * Near / D, T = Dot(Vu, Vc) * Near / D;
		V.EyeB = Eye; V.RightB = Vr; V.UpB = Vu; V.FwdB = -Vn;
		V.P00 = 2.f * Near / (R - L); V.P20 = -(R + L) / (R - L);
		V.P11 = 2.f * Near / (T - B); V.P21 = -(T + B) / (T - B);
	}
	V.Origin = BodyToUE(V.EyeB);
	V.Rotation = ViewRotation(BodyDirToUE(V.RightB), BodyDirToUE(V.UpB), BodyDirToUE(V.FwdB));
	V.Projection = Projection(V.P00, V.P20, V.P11, V.P21, Near * 100.f);
	V.bValid = true;
}

bool AReflectARMirror::BodyToScreen(const V3& P, const FVector2D& Size, FVector2D& Out) const
{
	const FMirrorView& V = MirrorView;
	if (!V.bValid) return false;
	const V3 d = P - V.EyeB;
	const float x = Dot(d, V.RightB), y = Dot(d, V.UpB), z = Dot(d, V.FwdB);
	if (z < 1e-3f) return false;
	const float Nx = (V.P00 * x + V.P20 * z) / z, Ny = (V.P11 * y + V.P21 * z) / z;
	Out = FVector2D((Nx + 1.f) * 0.5f * Size.X, (1.f - Ny) * 0.5f * Size.Y);
	return true;
}

// ======================================================================== material globals
void AReflectARMirror::SetParams(bool bMaskOn)
{
	if (!Params) return;
	UMaterialParameterCollectionInstance* P = GetWorld()->GetParameterCollectionInstance(Params);
	if (!P) return;
	const TSharedPtr<FTrackingFrame> F = Tracking->Latest;
	if (F.IsValid() && F->Cam.Valid())
	{
		FVector4f Rows[3];
		MaskRows(F->Cam, Rows);
		for (int r = 0; r < 3; r++) P->SetVectorParameterValue(*FString::Printf(TEXT("MaskM%d"), r), FLinearColor(Rows[r].X, Rows[r].Y, Rows[r].Z, Rows[r].W));
	}
	// Unreal world (cm) -> body coords: rows of the inverse of MirrorWorld's transform
	const FMatrix W2B = MirrorWorld->GetComponentTransform().ToMatrixWithScale().Inverse();
	for (int k = 0; k < 3; k++)
		P->SetVectorParameterValue(*FString::Printf(TEXT("W2B%d"), k), FLinearColor(W2B.M[0][k], W2B.M[1][k], W2B.M[2][k], W2B.M[3][k]));
	// the viewpoint in body coords (MirrorWorld's frame: reflected in Mirror mode)
	if (MirrorView.bValid)
	{
		const FVector Cb = W2B.TransformPosition(MirrorView.Origin);
		P->SetVectorParameterValue(TEXT("CamBody"), FLinearColor(Cb.X, Cb.Y, Cb.Z, 0));
	}
	for (int i = 0; i < FBodyProxy::MaxCover; i++)
	{
		const FCapsuleB C = i < Proxy.Cover.Num() ? Proxy.Cover[i] : FCapsuleB();
		P->SetVectorParameterValue(*FString::Printf(TEXT("CoverA%d"), i), FLinearColor(C.a.X, C.a.Y, C.a.Z, C.r));
		P->SetVectorParameterValue(*FString::Printf(TEXT("CoverB%d"), i), FLinearColor(C.b.X, C.b.Y, C.b.Z, 0));
	}
	const bool bFeedOn = Mode == EMirrorViewMode::CameraFeed && F.IsValid() && F->Cam.Valid() && Tracking->Feed();
	const float FeedOn = bFeedOn ? (F->Range == TEXT("ok") || !F->bTracked ? 1.f : 0.6f) : 0.f;
	P->SetVectorParameterValue(TEXT("Flags"), FLinearColor(FeedOn, 0.5f, float(Proxy.Cover.Num()), bShowProxy ? 1.f : 0.f));
	P->SetVectorParameterValue(TEXT("Ambient"), FLinearColor(Ambient, Ambient, Ambient * 1.04f, 0.f));
}
