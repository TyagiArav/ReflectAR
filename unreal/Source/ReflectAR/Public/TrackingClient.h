#pragma once

#include "BodyMath.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

class FSocket;
class FRunnableThread;
class UTexture2D;
class IImageWrapperModule;

namespace ReflectAR
{
	struct FCamInfo
	{
		float fx = 0, fy = 0, cx = 0, cy = 0, w = 0, h = 0, tilt = 0;
		V3 PosMirror = V3::ZeroVector;            // optical centre, Mirror frame (m)
		bool Valid() const { return fx > 0 && w > 0 && h > 0; }
	};

	struct FScreenInfo { float WM = 0.302f, HM = 0.196f; };

	/** Prediction tuning, from the "predict" block of perception/calib/config.json. */
	struct FPredictInfo { float DisplayLatency = 0.025f, HorizonTau = 0.35f, MaxHorizon = 0.25f, BlendTc = 0.015f; };

	/** One perception frame. Coordinates are in the Mirror frame (metres, right-handed). */
	struct FTrackingFrame
	{
		double t = 0, t_send = 0;     // capture / send time, perception clock (s)
		int32 Seq = 0;
		bool bTracked = false;
		FString Range = TEXT("none");
		float Dist = 0;
		TArray<FString> Events;
		float FistProgress = 0;
		FString FistHand;
		TArray<float> Joints, Vel, Vis, Eye, EyeVel;
		FCamInfo Cam;
		FScreenInfo Screen;
		bool bHasScreen = false, bHasPredict = false;
		FPredictInfo Predict;
		float ProcFps = 0, ProcMs = 0, DepthHz = 0, Scale = 0, ZSkel = 0, ZDa = 0;
		double Arrival = 0;            // local clock
	};

	/**
	 * Connects to perception/server.py over TCP and predicts the pose to any render time. Mirror ->
	 * body conversion (z flips) happens here and only here.
	 *
	 * Perception sends each frame's joints with velocities at the frame's capture time. Predict()
	 * extrapolates them to when the rendered frame will be on screen, so the overlay lands on the
	 * user's current pose rather than one from ~80 ms ago, and moves every render frame instead of
	 * in 30 Hz steps. Messages: 'J' JSON frame, 'I' JPEG, 'R' raw RGB24, 'M' person mask.
	 */
	class FTrackingClient : public FRunnable
	{
	public:
		FTrackingClient(const FString& InHost = TEXT("127.0.0.1"), int32 InPort = 5566);
		virtual ~FTrackingClient() override;

		TSharedPtr<FTrackingFrame> Latest;
		double LastFrameTime = -100;
		bool IsConnected() const { return bConnected; }
		TQueue<FString> Events;

		V3 Joints[LM::Count];          // body coords, real (unreflected) body, predicted
		float Visibility[LM::Count] = {};
		V3 Eye = V3::ZeroVector;

		float LatencyMs = 0, HorizonMs = 0;
		double FeedCaptureTime = -1e9, MaskCaptureTime = -1e9;
		/** Camera image (sRGB BGRA, row 0 = image top) and person mask (G8, row 0 = top). */
		UTexture2D* Feed() const { return FeedTex.Get(); }
		UTexture2D* Mask() const { return MaskTex.Get(); }
		/** The same mask on the CPU, for measuring the body's silhouette. */
		TArray<uint8> MaskBytes;
		int32 MaskW = 0, MaskH = 0;

		/** Monotonic local clock (s), usable from any thread. */
		static double Now() { return FPlatformTime::Seconds(); }

		/** Takes in whatever arrived since the last call. Game thread, once per frame, before Predict. */
		void Poll();
		/** Fills Joints / Eye with the pose at local time Target. */
		void Predict(double Target, bool bExtrapolate, bool bBlend);
		/** The pose at local time T without the hand-over easing (e.g. at the mask's capture time). */
		void PoseAt(double T, V3* Out) const;
		float DisplayLatency() const { return Tuning.DisplayLatency; }

		// FRunnable
		virtual uint32 Run() override;
		virtual void Stop() override { bRunning = false; }

	private:
		FString Host;
		int32 Port;
		FRunnableThread* Thread = nullptr;
		IImageWrapperModule* ImageWrapper = nullptr;
		std::atomic<bool> bRunning{ true }, bConnected{ false };

		FCriticalSection Gate;
		TSharedPtr<FTrackingFrame> PendingFrame;
		TArray<uint8> PendingImage, PendingMask;          // BGRA top-first / G8 top-first
		bool bPendingImage = false, bPendingMask = false;
		int32 PendingImageW = 0, PendingImageH = 0, PendingMaskW = 0, PendingMaskH = 0;
		double PendingImageT = 0, PendingMaskT = 0;

		TStrongObjectPtr<UTexture2D> FeedTex, MaskTex;

		// perception clock -> local clock: offset = min over recent frames of (arrival - t_send)
		double Offsets[64] = {};
		int OffsetCount = 0, OffsetHead = 0;
		double ClockOffset = 0;

		// prediction state, body coords
		V3 BasePos[LM::Count], BaseVel[LM::Count], Corr[LM::Count];
		V3 EyePos = V3::ZeroVector, EyeVel = V3::ZeroVector, EyeCorr = V3::ZeroVector;
		double CaptureTime = 0, CorrStart = 0, LastTarget = 0;
		bool bHasState = false;
		FPredictInfo Tuning;

		bool ReadExactly(FSocket* S, uint8* Buf, int32 N);
		void Handle(uint8 Type, const TArray<uint8>& Body, double Arrival);
		void Ingest(const FTrackingFrame& F);
		float Horizon(double Target) const { return FMath::Clamp(float(Target - CaptureTime), -0.1f, Tuning.MaxHorizon); }
		V3 Extrapolate(const V3& P, const V3& V, float H) const;
		V3 Eval(const V3& P, const V3& V, const V3& C, double Target, double NowT) const;
		static void Upload(TStrongObjectPtr<UTexture2D>& Tex, const TArray<uint8>& Px, int32 W, int32 H, bool bColor);
	};
}
