#include "BodyProxy.h"
#include "ClothGarment.h"

namespace ReflectAR
{
	// extra radius of exposed parts (m, for a 0.36 m shoulder span); the mask trims it back
	static constexpr float ArmMargin = 0.03f, HandMargin = 0.035f, HeadMargin = 0.05f;

	struct FLegSeg { int a, b; float r; };
	static const FLegSeg LegSegs[4] =
	{
		{ LM::LHip, LM::LKnee, 0.07f }, { LM::RHip, LM::RKnee, 0.07f }, { LM::LKnee, LM::LAnkle, 0.055f }, { LM::RKnee, LM::RAnkle, 0.055f },
	};

	/** Unit capsule like Unity's primitive: radius 0.5, 2 tall along Y. Or a sphere of radius 0.5. */
	static void UnitShape(bool bSphere, TArray<V3>& V, TArray<V3>& N, TArray<int32>& T)
	{
		constexpr int Seg = 16, Rings = 5;                   // rings per hemisphere
		// latitude rows from the top pole to the bottom pole; a capsule splits at the equator
		TArray<float> Lat, Off;
		for (int i = 0; i <= Rings; i++) { Lat.Add(HALF_PI * (1.f - i / float(Rings))); Off.Add(bSphere ? 0.f : 0.5f); }
		for (int i = 0; i <= Rings; i++) { Lat.Add(-HALF_PI * i / float(Rings)); Off.Add(bSphere ? 0.f : -0.5f); }
		const int Rows = Lat.Num();
		for (int r = 0; r < Rows; r++)
			for (int s = 0; s <= Seg; s++)
			{
				const float Lon = 2.f * PI * s / Seg, cl = FMath::Cos(Lat[r]);
				const V3 n(cl * FMath::Cos(Lon), FMath::Sin(Lat[r]), cl * FMath::Sin(Lon));
				N.Add(n);
				V.Add(n * 0.5f + V3(0, Off[r], 0));
			}
		for (int r = 0; r + 1 < Rows; r++)
			for (int s = 0; s < Seg; s++)
			{
				const int a = r * (Seg + 1) + s, b = a + 1, c = a + Seg + 1, d = c + 1;
				T.Append({ a, c, b, b, c, d });
			}
	}

	FBodyProxy::FBodyProxy()
	{
		UnitShape(false, CapsuleV, CapsuleN, CapsuleT);
		UnitShape(true, SphereV, SphereN, SphereT);
		Parts[Head].bSphere = Parts[Core(Head)].bSphere = true;
		// fixed topology: hidden parts collapse to a point instead of leaving the mesh
		for (int i = 0; i < Total; i++)
		{
			const TArray<int32>& Tr = Parts[i].bSphere ? SphereT : CapsuleT;
			const int Base = Mesh.Pos.Num(), Nv = Parts[i].bSphere ? SphereV.Num() : CapsuleV.Num();
			for (int t : Tr) Mesh.Tris.Add(Base + t);
			Mesh.Pos.AddZeroed(Nv); Mesh.Nrm.AddZeroed(Nv); Mesh.Tan.AddZeroed(Nv); Mesh.UV.AddZeroed(Nv);
			UV1.AddZeroed(Nv); UV2.AddZeroed(Nv); UV3.AddZeroed(Nv);
		}
	}

	void FBodyProxy::Place(FPart& P, const V3& a, const V3& b, float rx, float rz, const V3& FwdHint)
	{
		const V3 D = b - a;
		const float Len = D.Size();
		if (Len < 1e-4f || rx <= 0.f) { P.Scale = V3::ZeroVector; return; }
		const V3 Up = D / Len;
		V3 F = ProjectOnPlane(FwdHint, Up);
		if (F.SizeSquared() < 1e-4f) F = ProjectOnPlane(VFwd, Up);
		P.F = FFrame::Look((a + b) * 0.5f, Normalized(F), Up);
		P.Scale = V3(rx * 2.f, (Len + 2.f * FMath::Min(rx, rz)) * 0.5f, rz * 2.f);
	}

	void FBodyProxy::PlaceExposed(int i, const V3& a, const V3& b, float rx, float rz, const V3& FwdHint, float m, bool bMasked)
	{
		Place(Parts[Core(i)], a, b, rx, rz, FwdHint);
		if (bMasked) Place(Parts[i], a, b, rx + m, rz + m, FwdHint); else Parts[i].Scale = V3::ZeroVector;
	}

	/** How far a part has moved since the mask was captured (mean of two joints). */
	static V3 Offset(const V3* MaskJ, const V3* J, int a, int b)
	{
		return MaskJ ? ((J[a] - MaskJ[a]) + (J[b] - MaskJ[b])) * 0.5f : V3::ZeroVector;
	}

	void FBodyProxy::Update(const FBodyRig& B, bool bOn, float Sleeve, const V3* MaskJoints, const FClothGarment* Garment)
	{
		bVisible = bOn;
		if (!bOn) return;
		const float u = B.Unit();
		const V3* J = B.J;
		const bool bMasked = MaskJoints != nullptr;
		const float g = bMasked ? u : 0.f;           // without a mask, oversizing would cut holes around the body
		Cover.Reset();
		if (Garment) Garment->Outline(Cover);
		const float Room = Garment ? Garment->Room() : 0.f;

		// ---- under the garment: fitted, never trimmed
		Place(Parts[Torso], B.HipC, B.ShC, B.ShoulderW * 0.42f, 0.095f * u, B.Fwd);
		Place(Parts[Neck], B.ShC, B.HeadC - B.Up * 0.05f * u, 0.05f * u, 0.05f * u, B.Fwd);
		for (int i = 0; i < 4; i++)
			Place(Parts[Legs + i], J[LegSegs[i].a], J[LegSegs[i].b], LegSegs[i].r * u, LegSegs[i].r * u, B.Fwd);

		for (int s = 0; s < 2; s++)
		{
			const bool bLeft = s == 0;
			const int Si = bLeft ? LM::LShoulder : LM::RShoulder, Ei = bLeft ? LM::LElbow : LM::RElbow, Wi = bLeft ? LM::LWrist : LM::RWrist;
			const int Ii = bLeft ? LM::LIndex : LM::RIndex, Pi = bLeft ? LM::LPinky : LM::RPinky, Ti = bLeft ? LM::LThumb : LM::RThumb;
			const V3 S = J[Si], E = J[Ei], W = J[Wi];
			const float L1 = V3::Distance(S, E), L2 = V3::Distance(E, W);
			const float RUpper = 0.045f * u, RFore = 0.04f * u, m = ArmMargin * g;

			// arm: covered from the shoulder to the cuff, bare (oversized + trimmed) after it
			const float Cuff = Clamp01(Sleeve) * (L1 + L2);
			const float Bare = Cuff + (Cuff > 0.f ? 0.02f * u + RFore + m : 0.06f * u);   // clear of the cuff / armhole
			auto P = [&](float x) { return x <= L1 ? LerpV(S, E, x / FMath::Max(L1, 1e-4f)) : LerpV(E, W, (x - L1) / FMath::Max(L2, 1e-4f)); };

			if (Cuff > 0.f) Place(Parts[Covered + 2 * s], S, P(FMath::Min(Cuff, L1)), RUpper, RUpper, B.Fwd); else Parts[Covered + 2 * s].Scale = V3::ZeroVector;
			if (Cuff > L1) Place(Parts[Covered + 2 * s + 1], E, P(Cuff), RFore, RFore, B.Fwd); else Parts[Covered + 2 * s + 1].Scale = V3::ZeroVector;
			if (Cuff > 0.f) Cover.Emplace(S, P(FMath::Min(Cuff, L1)), RUpper + Room);      // the sleeve
			if (Cuff > L1) Cover.Emplace(E, P(Cuff), RFore + Room);

			const int X0 = Exposed + 2 * s, X1 = X0 + 1;
			if (Bare < L1)
			{
				PlaceExposed(X0, P(Bare), E, RUpper, RUpper, B.Fwd, m, bMasked);
				PlaceExposed(X1, E, W, RFore, RFore, B.Fwd, m, bMasked);
			}
			else if (Bare < L1 + L2)
			{
				PlaceExposed(X0, P(Bare), W, RFore, RFore, B.Fwd, m, bMasked);
				HideExposed(X1);
			}
			else { HideExposed(X0); HideExposed(X1); }
			Props(X0, bMasked, m, Offset(MaskJoints, J, Ei, Wi));
			Props(X1, bMasked, m, Offset(MaskJoints, J, Ei, Wi));

			// hand: wrist -> past the knuckles to cover the fingers; thumb separately
			const V3 K = (J[Ii] + J[Pi]) * 0.5f;
			V3 Dir = K - W;
			const float Dl = Dir.Size();
			Dir = Dl > 1e-4f ? Dir / Dl : B.Up;
			const float RHand = FMath::Clamp(V3::Distance(J[Ii], J[Pi]) * 0.6f, 0.03f * u, 0.05f * u), Mh = HandMargin * g;
			const V3 H0 = W + Dir * (Sleeve > 0.9f ? RHand + Mh : 0.f);              // long sleeves: clear of the cuff
			PlaceExposed(Hands + s, H0, K + Dir * 0.07f * u, RHand, 0.6f * RHand, Cross(Dir, B.Right), Mh, bMasked);
			Props(Hands + s, bMasked, Mh, Offset(MaskJoints, J, Wi, Ii));
			const V3 T = J[Ti];
			const V3 Td = (T - W).SizeSquared() > 1e-6f ? Normalized(T - W) : Dir;
			PlaceExposed(Thumbs + s, W + Td * 0.02f * u, T + Td * 0.035f * u, 0.018f * u, 0.018f * u, B.Fwd, Mh * 0.6f, bMasked);
			Props(Thumbs + s, bMasked, Mh * 0.6f, Offset(MaskJoints, J, Wi, Ti));
		}

		// ---- head, with room for hair
		const float Mhd = HeadMargin * g;
		const V3 HeadSize(B.HeadR * 1.9f, B.HeadR * 2.3f, B.HeadR * 2.1f);
		const FFrame Hf = FFrame::Look(B.HeadC + B.Up * 0.02f * u, B.Fwd, B.Up);
		Parts[Head].F = Parts[Core(Head)].F = Hf;
		Parts[Core(Head)].Scale = HeadSize;
		Parts[Head].Scale = bMasked ? HeadSize + V3(2.f * Mhd) : V3::ZeroVector;
		Props(Head, bMasked, Mhd, Offset(MaskJoints, J, LM::LEar, LM::REar));
		for (int i = 0; i < Exposed; i++) Props(i, false, 0.f, V3::ZeroVector);
		for (int i = Cores; i < Total; i++) Props(i, false, 0.f, V3::ZeroVector);
		if (Cover.Num() > MaxCover) Cover.SetNum(MaxCover);
		Emit();
	}

	void FBodyProxy::Emit()
	{
		int Base = 0;
		for (int i = 0; i < Total; i++)
		{
			const FPart& P = Parts[i];
			const TArray<V3>& Vs = P.bSphere ? SphereV : CapsuleV;
			const TArray<V3>& Ns = P.bSphere ? SphereN : CapsuleN;
			const bool bHidden = P.Scale.IsNearlyZero();
			const V2 A1(P.Offset.X, P.Offset.Y), A2(P.Offset.Z, P.Push), A3(P.bTrim ? 1.f : 0.f, 0.f);
			for (int k = 0; k < Vs.Num(); k++)
			{
				const int Vi = Base + k;
				if (bHidden) { Mesh.Pos[Vi] = P.F.o; Mesh.Nrm[Vi] = VUp; }
				else
				{
					const V3 l = Vs[k] * P.Scale;
					Mesh.Pos[Vi] = P.F.ToWorld(l);
					const V3 n = Ns[k];
					Mesh.Nrm[Vi] = Normalized(P.F.Dir(V3(n.X / FMath::Max(P.Scale.X, 1e-5f), n.Y / FMath::Max(P.Scale.Y, 1e-5f), n.Z / FMath::Max(P.Scale.Z, 1e-5f))));
				}
				Mesh.Tan[Vi] = V4(P.F.x.X, P.F.x.Y, P.F.x.Z, 1.f);
				UV1[Vi] = A1; UV2[Vi] = A2; UV3[Vi] = A3;
			}
			Base += Vs.Num();
		}
	}
}
