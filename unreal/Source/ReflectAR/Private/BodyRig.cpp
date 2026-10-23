#include "BodyRig.h"
#include "GarmentMesher.h"

namespace ReflectAR
{
	FBodyRig::FBodyRig()
	{
		for (V3& j : J) j = V3::ZeroVector;
		HipC = ShC = NeckBase = HeadC = V3::ZeroVector;
	}

	void FBodyRig::Update(const V3* Target, const float* InVis, float Dt, bool bSnap)
	{
		const float a = bSnap || Smoothing <= 0.f ? 1.f : 1.f - FMath::Exp(-Smoothing * Dt);
		for (int i = 0; i < LM::Count; i++)
		{
			J[i] = LerpV(J[i], Target[i], a);
			Vis[i] = InVis[i];
		}
		Derive(bSnap || !bMeasured);
		Valid = true;
	}

	void FBodyRig::Derive(bool bResetMeasures)
	{
		HipC = (J[LM::LHip] + J[LM::RHip]) * 0.5f;
		ShC = (J[LM::LShoulder] + J[LM::RShoulder]) * 0.5f;
		Up = Normalized(ShC - HipC);
		Right = Normalized(ProjectOnPlane(J[LM::RShoulder] - J[LM::LShoulder], Up));
		Fwd = Cross(Right, Up);
		PRight = Normalized(ProjectOnPlane(J[LM::RHip] - J[LM::LHip], Up));
		if (PRight.SizeSquared() < 0.5f) PRight = Right;
		PFwd = Cross(PRight, Up);

		// Body measurements change slowly; smooth hard so garments don't breathe.
		const float k = bResetMeasures ? 1.f : 0.03f;
		ShoulderW = Lerp(ShoulderW, FMath::Clamp(Seg(LM::LShoulder, LM::RShoulder), 0.25f, 0.55f), k);
		HipW = Lerp(HipW, FMath::Clamp(Seg(LM::LHip, LM::RHip), 0.15f, 0.40f), k);
		TorsoLen = Lerp(TorsoLen, FMath::Clamp(FVector3f::Distance(HipC, ShC), 0.35f, 0.75f), k);
		const float earW = Seg(LM::LEar, LM::REar);
		HeadR = Lerp(HeadR, FMath::Clamp(earW * 0.62f, 0.075f, 0.13f), k);
		ArmLenL = Lerp(ArmLenL, Seg(LM::LShoulder, LM::LElbow) + Seg(LM::LElbow, LM::LWrist), k);
		ArmLenR = Lerp(ArmLenR, Seg(LM::RShoulder, LM::RElbow) + Seg(LM::RElbow, LM::RWrist), k);
		UpperL = Lerp(UpperL, FMath::Clamp(Seg(LM::LShoulder, LM::LElbow), 0.18f, 0.40f), k);
		UpperR = Lerp(UpperR, FMath::Clamp(Seg(LM::RShoulder, LM::RElbow), 0.18f, 0.40f), k);
		ForeL = Lerp(ForeL, FMath::Clamp(Seg(LM::LElbow, LM::LWrist), 0.16f, 0.36f), k);
		ForeR = Lerp(ForeR, FMath::Clamp(Seg(LM::RElbow, LM::RWrist), 0.16f, 0.36f), k);
		bMeasured = true;

		NeckBase = ShC + (Up * 0.03f - Fwd * 0.01f) * Unit();
		HeadC = (J[LM::LEar] + J[LM::REar]) * 0.5f;
	}

	// ------------------------------------------------------------------ body shape
	void FBodyShape::Reset(FBodyRig& B)
	{
		B.ResetShape();
		for (int& n : LevelSamples) n = 0;
		Samples = 0;
	}

	float FBodyShape::ProfileAt(const float* Profile, float t)
	{
		if (t <= Levels[0]) return Profile[0];
		for (int i = 1; i < 6; i++)
			if (t <= Levels[i]) return FMath::Lerp(Profile[i - 1], Profile[i], (t - Levels[i - 1]) / (Levels[i] - Levels[i - 1]));
		return Profile[5];
	}

	static int SampleMask(const TArray<uint8>& Mask, int MW, int MH, const V2& p)
	{
		const int x = FMath::FloorToInt(p.X), y = FMath::FloorToInt(p.Y);
		return x < 0 || y < 0 || x >= MW || y >= MH ? -1 : Mask[y * MW + x];
	}

	/** Distance (m) from c along dir to where the mask stops being the person, or -1. */
	static float ScanEdge(const TArray<uint8>& Mask, int MW, int MH, const FVector4f M[3], const V3& c, const V3& dir, float MaxLen)
	{
		V2 p0, p1;
		if (!ProjectRows(M, c, p0) || !ProjectRows(M, c + dir * MaxLen, p1)) return -1.f;
		p0.X *= MW; p0.Y *= MH; p1.X *= MW; p1.Y *= MH;
		const int Steps = FMath::CeilToInt(V2::Distance(p0, p1) * 2.f);      // half-pixel steps
		if (Steps < 4 || SampleMask(Mask, MW, MH, p0) < 128) return -1.f;
		for (int i = 1; i <= Steps; i++)
		{
			const int m = SampleMask(Mask, MW, MH, p0 + (p1 - p0) * (i / float(Steps)));
			if (m < 0) return -1.f;
			if (m < 128) return MaxLen * (i - 0.5f) / Steps;
		}
		return -1.f;
	}

	/** Whether this side's arm (or hand) is at this height and within a few cm of the silhouette edge. */
	static bool ArmTouches(const V3* J, bool bRight, const V3& HipC, const V3& Up, const V3& c, const V3& Dir, float H, float Edge, float U)
	{
		const int s = bRight ? LM::RShoulder : LM::LShoulder, e = bRight ? LM::RElbow : LM::LElbow;
		const int w = bRight ? LM::RWrist : LM::LWrist, k = bRight ? LM::RIndex : LM::LIndex;
		auto Near = [&](const V3& a, const V3& b)
		{
			const float ha = Dot(a - HipC, Up) - H, hb = Dot(b - HipC, Up) - H, Margin = 0.06f * U;
			if (FMath::Min(ha, hb) > Margin || FMath::Max(ha, hb) < -Margin) return false;
			const float t = FMath::Abs(ha - hb) > 1e-4f ? Clamp01(ha / (ha - hb)) : 0.5f;
			const float Lat = Dot(LerpU(a, b, t) - c, Dir), Ra = 0.055f * U;
			return Lat + Ra > Edge - 0.015f && Lat - Ra < Edge + 0.03f;
		};
		return Near(J[s], J[e]) || Near(J[e], J[w]) || Near(J[w], J[k]);
	}

	void FBodyShape::Measure(const TArray<uint8>& Mask, int MW, int MH, const FVector4f M[3], const V3* InJ,
		FBodyRig& B, const V3& CamPos, float Dt)
	{
		if (MW <= 0 || MH <= 0 || Mask.Num() < MW * MH) return;
		if (B.Vis[LM::LShoulder] < 0.6f || B.Vis[LM::RShoulder] < 0.6f || B.Vis[LM::LHip] < 0.5f || B.Vis[LM::RHip] < 0.5f) return;
		const V3 HipC = (InJ[LM::LHip] + InJ[LM::RHip]) * 0.5f, ShC = (InJ[LM::LShoulder] + InJ[LM::RShoulder]) * 0.5f;
		const float L = FVector3f::Distance(HipC, ShC);
		if (L < 0.2f) return;
		const V3 Up = (ShC - HipC) / L;
		const V3 ChestR = Normalized(ProjectOnPlane(InJ[LM::RShoulder] - InJ[LM::LShoulder], Up));
		V3 PelvisR = Normalized(ProjectOnPlane(InJ[LM::RHip] - InJ[LM::LHip], Up));
		if (PelvisR.SizeSquared() < 0.5f) PelvisR = ChestR;
		const V3 Fwd = Cross(ChestR, Up);
		if (FMath::Abs(Dot(Fwd, Normalized(CamPos - ShC))) < FMath::Cos(FMath::DegreesToRadians(MaxYawDeg))) return;

		const float U = B.Unit(), Rate = 1.f - FMath::Exp(-Dt / TimeConstant);
		for (int k = 0; k < 6; k++)
		{
			const float t = Levels[k];
			const V3 R = SlerpDir(PelvisR, ChestR, SmoothStep(0.f, 1.f, (t - 0.1f) / 0.7f));
			const V3 c = HipC + Up * (t * L);
			float Sum = 0.f;
			int N = 0;
			for (int Side = -1; Side <= 1; Side += 2)
			{
				const V3 Dir = R * float(Side);
				const float Edge = ScanEdge(Mask, MW, MH, M, c, Dir, 0.45f * U);
				if (Edge <= 0.f || ArmTouches(InJ, Side > 0, HipC, Up, c, Dir, t * L, Edge, U)) continue;
				Sum += Edge;
				N++;
			}
			if (N == 0) continue;
			float a, d;
			FRestSkeleton::BodyModel(t, B.HipW, B.ShoulderW, U, a, d);
			const float r = Sum / N / a;
			if (r < 0.7f || r > 1.6f) continue;
			if (LevelSamples[k] > 15 && FMath::Abs(r - B.Profile[k]) > 0.2f) continue;   // a bag, a loose sleeve, ...
			B.Profile[k] = LevelSamples[k] == 0 ? r : Lerp(B.Profile[k], r, Rate);
			LevelSamples[k]++;
			Samples++;
		}
	}
}
