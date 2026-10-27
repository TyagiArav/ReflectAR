#include "TrackingClient.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/RunnableThread.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace ReflectAR
{
	FTrackingClient::FTrackingClient(const FString& InHost, int32 InPort) : Host(InHost), Port(InPort)
	{
		for (int i = 0; i < LM::Count; i++) Joints[i] = BasePos[i] = BaseVel[i] = Corr[i] = V3::ZeroVector;
		ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));   // game thread
		Thread = FRunnableThread::Create(this, TEXT("ReflectAR.Tracking"));
	}

	FTrackingClient::~FTrackingClient()
	{
		bRunning = false;
		if (Thread) { Thread->WaitForCompletion(); delete Thread; }
	}

	// ---------------------------------------------------------------- network thread
	bool FTrackingClient::ReadExactly(FSocket* S, uint8* Buf, int32 N)
	{
		int32 Off = 0;
		double Last = Now();
		while (Off < N && bRunning)
		{
			if (!S->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200)))
			{
				if (Now() - Last > 3.0) return false;               // nothing for 3 s: reconnect
				continue;
			}
			int32 Read = 0;
			if (!S->Recv(Buf + Off, N - Off, Read) || Read <= 0) return false;   // closed
			Off += Read;
			Last = Now();
		}
		return Off == N;
	}

	uint32 FTrackingClient::Run()
	{
		ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		uint8 Header[5];
		TArray<uint8> Body;
		while (bRunning)
		{
			FSocket* S = Subsystem->CreateSocket(NAME_Stream, TEXT("ReflectAR perception"), false);
			TSharedRef<FInternetAddr> Addr = Subsystem->CreateInternetAddr();
			bool bValid = false;
			Addr->SetIp(*Host, bValid);
			Addr->SetPort(Port);
			if (S && bValid)
			{
				S->SetNoDelay(true);
				if (S->Connect(*Addr))
				{
					bConnected = true;
					while (bRunning)
					{
						if (!ReadExactly(S, Header, 5)) break;
						const int32 Len = (int32(Header[1]) << 24) | (int32(Header[2]) << 16) | (int32(Header[3]) << 8) | Header[4];
						if (Len < 0 || Len > 64 * 1024 * 1024) break;
						Body.SetNumUninitialized(Len);
						if (!ReadExactly(S, Body.GetData(), Len)) break;
						Handle(Header[0], Body, Now());
					}
				}
			}
			bConnected = false;
			if (S) { S->Close(); Subsystem->DestroySocket(S); }
			if (bRunning) FPlatformProcess::Sleep(0.5f);
		}
		return 0;
	}

	static double ReadBEDouble(const uint8* P)
	{
		uint64 V = 0;
		for (int i = 0; i < 8; i++) V = (V << 8) | P[i];
		double D;
		FMemory::Memcpy(&D, &V, 8);
		return D;
	}

	static void ParseFrame(const TSharedPtr<FJsonObject>& O, FTrackingFrame& F)
	{
		auto Num = [](const TSharedPtr<FJsonObject>& J, const TCHAR* K, double Def) { double V; return J->TryGetNumberField(K, V) ? V : Def; };
		auto Floats = [](const TSharedPtr<FJsonObject>& J, const TCHAR* K, TArray<float>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* A;
			if (!J->TryGetArrayField(K, A)) return;
			Out.Reset(A->Num());
			for (const TSharedPtr<FJsonValue>& V : *A) Out.Add(float(V->AsNumber()));
		};
		F.t = Num(O, TEXT("t"), 0); F.t_send = Num(O, TEXT("t_send"), 0);
		F.Seq = int32(Num(O, TEXT("seq"), 0));
		O->TryGetBoolField(TEXT("tracked"), F.bTracked);
		O->TryGetStringField(TEXT("range"), F.Range);
		F.Dist = float(Num(O, TEXT("dist"), 0));
		O->TryGetStringArrayField(TEXT("events"), F.Events);
		F.FistProgress = float(Num(O, TEXT("fist_progress"), 0));
		O->TryGetStringField(TEXT("fist_hand"), F.FistHand);
		Floats(O, TEXT("joints"), F.Joints); Floats(O, TEXT("vel"), F.Vel); Floats(O, TEXT("vis"), F.Vis);
		Floats(O, TEXT("eye"), F.Eye); Floats(O, TEXT("eye_vel"), F.EyeVel);
		const TSharedPtr<FJsonObject>* Sub;
		if (O->TryGetObjectField(TEXT("cam"), Sub))
		{
			FCamInfo& C = F.Cam;
			C.fx = Num(*Sub, TEXT("fx"), 0); C.fy = Num(*Sub, TEXT("fy"), 0); C.cx = Num(*Sub, TEXT("cx"), 0); C.cy = Num(*Sub, TEXT("cy"), 0);
			C.w = Num(*Sub, TEXT("w"), 0); C.h = Num(*Sub, TEXT("h"), 0); C.tilt = Num(*Sub, TEXT("tilt"), 0);
			TArray<float> Pos;
			Floats(*Sub, TEXT("pos"), Pos);
			if (Pos.Num() == 3) C.PosMirror = V3(Pos[0], Pos[1], Pos[2]);
		}
		if (O->TryGetObjectField(TEXT("screen"), Sub))
		{
			F.bHasScreen = true;
			F.Screen.WM = Num(*Sub, TEXT("w_m"), 0.302); F.Screen.HM = Num(*Sub, TEXT("h_m"), 0.196);
		}
		if (O->TryGetObjectField(TEXT("predict"), Sub))
		{
			F.bHasPredict = true;
			F.Predict.DisplayLatency = Num(*Sub, TEXT("display_latency_s"), 0.025);
			F.Predict.HorizonTau = Num(*Sub, TEXT("horizon_tau_s"), 0.35);
			F.Predict.MaxHorizon = Num(*Sub, TEXT("max_horizon_s"), 0.25);
			F.Predict.BlendTc = Num(*Sub, TEXT("blend_tc_s"), 0.015);
		}
		F.ProcFps = Num(O, TEXT("proc_fps"), 0); F.ProcMs = Num(O, TEXT("proc_ms"), 0); F.DepthHz = Num(O, TEXT("depth_hz"), 0);
		F.Scale = Num(O, TEXT("scale"), 0); F.ZSkel = Num(O, TEXT("z_skel"), 0); F.ZDa = Num(O, TEXT("z_da"), 0);
	}

	void FTrackingClient::Handle(uint8 Type, const TArray<uint8>& Body, double Arrival)
	{
		const int32 Len = Body.Num();
		if (Type == 'J')
		{
			const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Len);
			const FString Text(Conv.Length(), Conv.Get());
			TSharedPtr<FJsonObject> O;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) || !O.IsValid()) return;
			TSharedPtr<FTrackingFrame> F = MakeShared<FTrackingFrame>();
			ParseFrame(O, *F);
			F->Arrival = Arrival;
			for (const FString& E : F->Events) Events.Enqueue(E);
			FScopeLock L(&Gate);
			PendingFrame = F;
		}
		else if (Type == 'I' && Len > 8)
		{
			// 8-byte big-endian capture time, then the JPEG
			TSharedPtr<IImageWrapper> W = ImageWrapper->CreateImageWrapper(EImageFormat::JPEG);
			TArray64<uint8> Raw;
			if (!W.IsValid() || !W->SetCompressed(Body.GetData() + 8, Len - 8) || !W->GetRaw(ERGBFormat::BGRA, 8, Raw)) return;
			FScopeLock L(&Gate);
			PendingImage = TArray<uint8>(Raw.GetData(), int32(Raw.Num()));
			PendingImageW = W->GetWidth(); PendingImageH = W->GetHeight();
			PendingImageT = ReadBEDouble(Body.GetData());
			bPendingImage = true;
		}
		else if (Type == 'R' && Len > 12)
		{
			// capture time (float64), width, height (uint16), then RGB24 rows bottom first
			const int32 Wd = (Body[8] << 8) | Body[9], Ht = (Body[10] << 8) | Body[11];
			if (Wd * Ht * 3 != Len - 12) return;
			TArray<uint8> Px;
			Px.SetNumUninitialized(Wd * Ht * 4);
			for (int y = 0; y < Ht; y++)
			{
				const uint8* Src = Body.GetData() + 12 + int64(Ht - 1 - y) * Wd * 3;
				uint8* Dst = Px.GetData() + int64(y) * Wd * 4;
				for (int x = 0; x < Wd; x++) { Dst[4 * x] = Src[3 * x + 2]; Dst[4 * x + 1] = Src[3 * x + 1]; Dst[4 * x + 2] = Src[3 * x]; Dst[4 * x + 3] = 255; }
			}
			FScopeLock L(&Gate);
			PendingImage = MoveTemp(Px);
			PendingImageW = Wd; PendingImageH = Ht;
			PendingImageT = ReadBEDouble(Body.GetData());
			bPendingImage = true;
		}
		else if (Type == 'M' && Len > 12)
		{
			// capture time (float64), width, height (uint16), then uint8 rows, top first
			const int32 Wd = (Body[8] << 8) | Body[9], Ht = (Body[10] << 8) | Body[11];
			if (Wd * Ht != Len - 12) return;
			FScopeLock L(&Gate);
			PendingMask = TArray<uint8>(Body.GetData() + 12, Wd * Ht);
			PendingMaskW = Wd; PendingMaskH = Ht;
			PendingMaskT = ReadBEDouble(Body.GetData());
			bPendingMask = true;
		}
	}

	// ---------------------------------------------------------------- game thread
	void FTrackingClient::Upload(TStrongObjectPtr<UTexture2D>& Tex, const TArray<uint8>& Px, int32 W, int32 H, bool bColor)
	{
		const EPixelFormat Fmt = bColor ? PF_B8G8R8A8 : PF_G8;
		const int32 Bpp = bColor ? 4 : 1;
		if (!Tex.IsValid() || Tex->GetSizeX() != W || Tex->GetSizeY() != H)
		{
			UTexture2D* T = UTexture2D::CreateTransient(W, H, Fmt);
			T->SRGB = bColor;
			T->AddressX = T->AddressY = TA_Clamp;
			T->Filter = TF_Bilinear;
			T->NeverStream = true;
			T->UpdateResource();
			Tex.Reset(T);
		}
		uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Px.Num()));
		FMemory::Memcpy(Copy, Px.GetData(), Px.Num());
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
		Tex->UpdateTextureRegions(0, 1, Region, W * Bpp, Bpp, Copy,
			[](uint8* Data, const FUpdateTextureRegion2D* Regions) { FMemory::Free(Data); delete Regions; });
	}

	void FTrackingClient::Poll()
	{
		TSharedPtr<FTrackingFrame> F;
		TArray<uint8> Img, Msk;
		bool bImg, bMsk;
		int32 IW, IH, MW, MH;
		double IT, MT;
		{
			FScopeLock L(&Gate);
			F = MoveTemp(PendingFrame); PendingFrame.Reset();
			bImg = bPendingImage; bPendingImage = false; if (bImg) Img = MoveTemp(PendingImage);
			bMsk = bPendingMask; bPendingMask = false; if (bMsk) Msk = MoveTemp(PendingMask);
			IW = PendingImageW; IH = PendingImageH; IT = PendingImageT;
			MW = PendingMaskW; MH = PendingMaskH; MT = PendingMaskT;
		}
		if (F.IsValid())
		{
			// Same machine: transit is ~0.1 ms, so the smallest (arrival - send) is the clock offset.
			Offsets[OffsetHead] = F->Arrival - F->t_send;
			OffsetHead = (OffsetHead + 1) % 64;
			OffsetCount = FMath::Min(OffsetCount + 1, 64);
			ClockOffset = TNumericLimits<double>::Max();
			for (int i = 0; i < OffsetCount; i++) ClockOffset = FMath::Min(ClockOffset, Offsets[i]);

			Latest = F;
			LastFrameTime = Now();
			if (F->bHasPredict) Tuning = F->Predict;
			LatencyMs = float((F->Arrival - (F->t + ClockOffset)) * 1000.0);
			if (F->bTracked && F->Joints.Num() >= 99 && F->Vis.Num() >= 33 && F->Eye.Num() == 3) Ingest(*F);
		}
		if (bImg && OffsetCount > 0 && IW > 0)
		{
			Upload(FeedTex, Img, IW, IH, true);
			FeedCaptureTime = IT + ClockOffset;
		}
		if (bMsk && OffsetCount > 0 && MW > 0)
		{
			Upload(MaskTex, Msk, MW, MH, false);
			MaskBytes = MoveTemp(Msk);
			MaskW = MW; MaskH = MH;
			MaskCaptureTime = MT + ClockOffset;
		}
	}

	void FTrackingClient::Ingest(const FTrackingFrame& F)
	{
		const double NowT = Now();
		const double At = LastTarget > 0 ? LastTarget : NowT;
		const bool bBlendable = bHasState && NowT - CaptureTime < 0.5;
		// what was being shown, so the hand-over to the new frame can be eased (see Predict)
		if (bBlendable)
		{
			for (int i = 0; i < LM::Count; i++) Corr[i] = Eval(BasePos[i], BaseVel[i], Corr[i], At, NowT);
			EyeCorr = Eval(EyePos, EyeVel, EyeCorr, At, NowT);
		}
		const bool bHasVel = F.Vel.Num() >= 99;
		for (int i = 0; i < LM::Count; i++)
		{
			BasePos[i] = MirrorToBody(F.Joints[3 * i], F.Joints[3 * i + 1], F.Joints[3 * i + 2]);
			BaseVel[i] = bHasVel ? MirrorToBody(F.Vel[3 * i], F.Vel[3 * i + 1], F.Vel[3 * i + 2]) : V3::ZeroVector;
			Visibility[i] = F.Vis[i];
		}
		EyePos = MirrorToBody(F.Eye[0], F.Eye[1], F.Eye[2]);
		EyeVel = F.EyeVel.Num() == 3 ? MirrorToBody(F.EyeVel[0], F.EyeVel[1], F.EyeVel[2]) : V3::ZeroVector;
		CaptureTime = F.t + ClockOffset;
		bHasState = true;

		const float H = Horizon(At);
		for (int i = 0; i < LM::Count; i++)
		{
			Corr[i] = bBlendable ? Corr[i] - Extrapolate(BasePos[i], BaseVel[i], H) : V3::ZeroVector;
			if (Corr[i].SizeSquared() > 0.3f * 0.3f) Corr[i] = V3::ZeroVector;       // re-detection: snap
		}
		EyeCorr = bBlendable ? EyeCorr - Extrapolate(EyePos, EyeVel, H) : V3::ZeroVector;
		if (EyeCorr.SizeSquared() > 0.3f * 0.3f) EyeCorr = V3::ZeroVector;
		CorrStart = NowT;
	}

	V3 FTrackingClient::Extrapolate(const V3& P, const V3& V, float H) const
	{
		// damped: long horizons extrapolate less than linearly, so stops overshoot less
		const float Tau = Tuning.HorizonTau;
		const float K = Tau > 0.f && H > 0.f ? Tau * (1.f - FMath::Exp(-H / Tau)) : H;
		return P + V * K;
	}

	V3 FTrackingClient::Eval(const V3& P, const V3& V, const V3& C, double Target, double NowT) const
	{
		const float Decay = Tuning.BlendTc > 0.f ? FMath::Exp(-float(NowT - CorrStart) / Tuning.BlendTc) : 0.f;
		return Extrapolate(P, V, Horizon(Target)) + C * Decay;
	}

	void FTrackingClient::Predict(double Target, bool bExtrapolate, bool bBlend)
	{
		if (!bHasState) return;
		if (!bExtrapolate) Target = CaptureTime;
		LastTarget = Target;
		const double NowT = Now();
		HorizonMs = Horizon(Target) * 1000.f;
		for (int i = 0; i < LM::Count; i++)
			Joints[i] = bBlend ? Eval(BasePos[i], BaseVel[i], Corr[i], Target, NowT) : Extrapolate(BasePos[i], BaseVel[i], Horizon(Target));
		Eye = bBlend ? Eval(EyePos, EyeVel, EyeCorr, Target, NowT) : Extrapolate(EyePos, EyeVel, Horizon(Target));
	}

	void FTrackingClient::PoseAt(double T, V3* Out) const
	{
		if (!bHasState) return;
		const float H = Horizon(T);
		for (int i = 0; i < LM::Count; i++) Out[i] = Extrapolate(BasePos[i], BaseVel[i], H);
	}
}
