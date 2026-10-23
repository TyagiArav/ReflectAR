#include "ReflectARGameMode.h"
#include "ReflectARMirror.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"

using namespace ReflectAR;

AReflectARGameMode::AReflectARGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = AReflectARHUD::StaticClass();
}

void AReflectARGameMode::StartPlay()
{
	Super::StartPlay();
	if (!AReflectARMirror::Get(GetWorld())) GetWorld()->SpawnActor<AReflectARMirror>();
}

// ======================================================================== HUD
static const FLinearColor PanelC(0, 0, 0, 0.55f), Accent(1.f, 0.82f, 0.35f, 1.f), CardC(0.12f, 0.12f, 0.12f, 0.85f);

void AReflectARHUD::Panel(float X, float Y, float W, float H, const FLinearColor& C)
{
	FCanvasTileItem Tile(FVector2D(X, Y), FVector2D(W, H), C);
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
}

void AReflectARHUD::Text(const FString& S, float X, float Y, float Scale, const FLinearColor& C, bool bCentre)
{
	UFont* Font = GEngine->GetLargeFont();
	FCanvasTextItem T(FVector2D(X, Y), FText::FromString(S), Font, C);
	T.Scale = FVector2D(Scale, Scale);
	T.bCentreX = bCentre; T.bCentreY = bCentre;
	T.EnableShadow(FLinearColor(0, 0, 0, 0.8f * C.A), FVector2D(2, 2));
	Canvas->DrawItem(T);
}

void AReflectARHUD::DrawHUD()
{
	Super::DrawHUD();
	const AReflectARMirror* M = AReflectARMirror::Get(GetWorld());
	if (!M || !M->Client() || !Canvas) return;
	const FTrackingClient& C = *M->Client();
	const TSharedPtr<FTrackingFrame> F = C.Latest;
	const float W = Canvas->SizeX, H = Canvas->SizeY;
	const float S = FMath::Sqrt((W / 1920.f) * (H / 1080.f));          // scale with the screen (Unity's CanvasScaler, match 0.5)
	const float Fh = 24.f;                                             // GetLargeFont's size in pixels, roughly
	const float Dt = GetWorld()->GetDeltaSeconds();
	const double Now = FTrackingClient::Now();
	const bool bConnected = C.IsConnected();

	// ---- range prompt
	FString P, Sub;
	if (!bConnected) { P = TEXT("Starting…"); Sub = TEXT("Waiting for the perception service (perception/server.py)"); }
	else if (!F.IsValid() || !F->bTracked || F->Range == TEXT("none")) { P = TEXT("Step into view"); Sub = TEXT("Stand 1–3 m from the mirror, facing it"); }
	else if (F->Range == TEXT("too_close")) { P = TEXT("Please step back"); Sub = FString::Printf(TEXT("You're %.1f m away – stand 1–3 m from the mirror"), F->Dist); }
	else if (F->Range == TEXT("too_far")) { P = TEXT("Come a little closer"); Sub = FString::Printf(TEXT("You're %.1f m away – stand 1–3 m from the mirror"), F->Dist); }
	if (!P.IsEmpty())
	{
		const float Pw = 900 * S, Ph = 150 * S, Px = (W - Pw) / 2, Py = 40 * S;
		Panel(Px, Py, Pw, Ph, PanelC);
		Text(P, W / 2, Py + Ph / 2 - 22 * S, 64.f / Fh * S, FLinearColor::White);
		Text(Sub, W / 2, Py + Ph / 2 + 42 * S, 28.f / Fh * S, FLinearColor::White);
	}

	// ---- carousel: slide so the current card is centred
	const TArray<FGarmentSpec>& Specs = GarmentCatalog();
	CardScale.SetNum(Specs.Num());
	const float StripH = 200 * S, StripY = H - 20 * S - StripH;
	Panel(0, StripY, W, StripH, FLinearColor(0, 0, 0, 0.35f));
	CarouselX = FMath::Lerp(CarouselX, -M->Current * 200.f, 1.f - FMath::Exp(-12.f * Dt));
	for (int i = 0; i < Specs.Num(); i++)
	{
		const bool bCur = i == M->Current;
		if (CardScale[i] <= 0) CardScale[i] = 1;
		CardScale[i] = FMath::Lerp(CardScale[i], bCur ? 1.12f : 0.92f, 1.f - FMath::Exp(-14.f * Dt));
		const float K = CardScale[i] * S, Cx = W / 2 + (CarouselX + i * 200.f) * S, Cy = StripY + StripH / 2;
		const float Cw = 180 * K, Ch = 160 * K;
		if (Cx + Cw < 0 || Cx - Cw > W) continue;
		Panel(Cx - Cw / 2, Cy - Ch / 2, Cw, Ch, bCur ? FLinearColor(Accent.R, Accent.G, Accent.B, 0.95f) : FLinearColor(0.12f, 0.12f, 0.12f, 0.8f));
		// swatch: a piece of the actual fabric
		const float Sw = 150 * K, Sh = 80 * K, Sx = Cx - Sw / 2, Sy = Cy - Ch / 2 + 12 * K;
		if (UTexture2D* Tex = M->Swatch(i))
		{
			FCanvasTileItem Tile(FVector2D(Sx, Sy), Tex->GetResource(), FVector2D(Sw, Sh), FVector2D(0, 0), FVector2D(1.5f, 0.8f), FLinearColor::White);
			Canvas->DrawItem(Tile);
		}
		else Panel(Sx, Sy, Sw, Sh, Specs[i].ColorA);
		Text(Specs[i].Name, Cx, Cy + 40 * K, 20.f / Fh * K, FLinearColor::White);
		if (i == M->Selected)
		{
			// tick in the corner
			const FVector2D A(Cx + 52 * K, Cy - 56 * K), B(Cx + 62 * K, Cy - 46 * K), E(Cx + 80 * K, Cy - 70 * K);
			FCanvasLineItem L1(A, B), L2(B, E);
			L1.LineThickness = L2.LineThickness = 5 * K;
			L1.SetColor(Accent); L2.SetColor(Accent);
			Canvas->DrawItem(L1); Canvas->DrawItem(L2);
		}
	}
	if (bConnected && F.IsValid() && F->bTracked && F->Range == TEXT("ok"))
		Text(TEXT("Swipe your hand  <  >  to browse     ·     Raise a fist for 5 s to select"), W / 2, H - 245 * S, 26.f / Fh * S, FLinearColor::White);

	// ---- fist ring at the hand
	const float Prog = F.IsValid() && F->bTracked ? F->FistProgress : 0.f;
	if (Prog > 0.01f && M->Body().Valid)
	{
		V3 Wp = M->Body().J[F->FistHand == TEXT("left") ? LM::LWrist : LM::RWrist];
		if (M->Mode == EMirrorViewMode::Mirror) Wp.Z = -Wp.Z;          // the reflected hand
		FVector2D Sp;
		if (M->BodyToScreen(Wp, FVector2D(W, H), Sp))
		{
			Sp.X = FMath::Clamp(Sp.X, 90 * S, W - 90 * S);
			Sp.Y = FMath::Clamp(Sp.Y, 90 * S, H - 310 * S);
			const float R = 66 * S;
			const int N = 64, Filled = FMath::CeilToInt(Prog * N);
			for (int k = 0; k < N; k++)
			{
				const float A0 = -HALF_PI + k * 2 * PI / N, A1 = A0 + 2 * PI / N;   // clockwise from the top
				FCanvasLineItem L(Sp + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * R, Sp + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * R);
				L.LineThickness = 12 * S;
				L.SetColor(k < Filled ? Accent : FLinearColor(1, 1, 1, 0.15f));
				Canvas->DrawItem(L);
			}
			Text(FString::FromInt(FMath::CeilToInt((1.f - Prog) * 5.f)), Sp.X, Sp.Y, 46.f / Fh * S, FLinearColor::White);
		}
	}

	// ---- toast
	const float Ta = FMath::Clamp(2.5f - float(Now - M->ToastTime), 0.f, 1.f);
	if (Ta > 0) Text(M->ToastText, W / 2, H / 2 - 120 * S, 54.f / Fh * S, FLinearColor(Accent.R, Accent.G, Accent.B, Ta));

	// ---- debug line
	if (M->bShowDebug)
	{
		const TCHAR* ModeName = M->Mode == EMirrorViewMode::Mirror ? TEXT("Mirror") : TEXT("CameraFeed");
		const float Ts = 20.f / Fh * S;
		if (!F.IsValid())
			Text(FString::Printf(TEXT("%s  ·  [M] mode  [</>] browse  [Space] select  [B] proxy  [C] cube  [D] debug"),
				bConnected ? TEXT("connected") : TEXT("no perception")), 16 * S, 12 * S, Ts, FLinearColor::White, false);
		else
		{
			Text(FString::Printf(TEXT("%s  ·  perception %.0f fps  depth %.0f Hz  ·  dist %.2f m (skel %.2f / mono %.2f, s=%.2f)  ·  render %.0f fps  ·  [M] mode [B] proxy [C] cube [D] debug"),
				ModeName, F->ProcFps, F->DepthHz, F->Dist, F->ZSkel, F->ZDa, F->Scale, M->RenderFps), 16 * S, 12 * S, Ts, FLinearColor::White, false);
			Text(M->LatencyLine, 16 * S, 12 * S + 26 * S, Ts, FLinearColor::White, false);
		}
	}
}
