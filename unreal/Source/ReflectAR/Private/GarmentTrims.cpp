#include "GarmentTrims.h"

namespace ReflectAR
{
	struct FHit { int32 a = 0, b = 0, c = 0; float wa = 0, wb = 0, wc = 0; };

	static TArray<int32> FrontTriangles(const FGarmentMeshData& D)
	{
		TArray<int32> List;
		const TArray<V3>& R = D.Rest; const TArray<int32>& Tr = D.Tris;
		for (int t = 0; t < Tr.Num(); t += 3)
		{
			const V3 a = R[Tr[t]], b = R[Tr[t + 1]], c = R[Tr[t + 2]];
			if (Normalized(Cross(b - a, c - a)).Z > 0.2f && FMath::Max3(a.Z, b.Z, c.Z) > 0.f) List.Add(t);
		}
		return List;
	}

	/** First front-facing hit of the ray from (x, y, +inf) toward -z. */
	static bool RayFront(const FGarmentMeshData& D, const TArray<int32>& Front, float x, float y, FHit& Hit, V3& P)
	{
		const TArray<V3>& R = D.Rest; const TArray<int32>& Tr = D.Tris;
		float BestZ = -UE_BIG_NUMBER;
		for (int t : Front)
		{
			const V3 a = R[Tr[t]], b = R[Tr[t + 1]], c = R[Tr[t + 2]];
			// barycentrics in the xy plane
			const float Det = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
			if (FMath::Abs(Det) < 1e-12f) continue;
			const float l0 = ((b.Y - c.Y) * (x - c.X) + (c.X - b.X) * (y - c.Y)) / Det;
			const float l1 = ((c.Y - a.Y) * (x - c.X) + (a.X - c.X) * (y - c.Y)) / Det;
			const float l2 = 1.f - l0 - l1;
			if (l0 < 0.f || l1 < 0.f || l2 < 0.f) continue;
			const float z = a.Z * l0 + b.Z * l1 + c.Z * l2;
			if (z <= BestZ) continue;
			BestZ = z;
			Hit = { Tr[t], Tr[t + 1], Tr[t + 2], l0, l1, l2 };
			P = V3(x, y, z);
		}
		return BestZ > 0.f;
	}

	/** Right and up axes along a chain of points with normals (top first). */
	static void Frames(const TArray<V3>& P, const TArray<V3>& N, TArray<V3>& Right, TArray<V3>& Up)
	{
		const int C = P.Num();
		Right.SetNum(C); Up.SetNum(C);
		for (int i = 0; i < C; i++)
		{
			const V3 u = P[FMath::Max(i - 1, 0)] - P[FMath::Min(i + 1, C - 1)];
			Right[i] = Normalized(Cross(u, N[i]));
			Up[i] = Cross(N[i], Right[i]);
		}
	}

	/** Collects trim vertices at (arc length s, local x, z) along the chain. */
	class FTrimBuilder
	{
	public:
		FGarmentTrims& T;
		const TArray<V3>& RestP; const TArray<V3>& RestN; const TArray<V3>& RestR; const TArray<V3>& RestU;
		TArray<V3> Flat[FGarmentTrims::NumSections], FlatN[FGarmentTrims::NumSections];   // (x, -s, z) + local normal: to check winding

		FTrimBuilder(FGarmentTrims& InT, const TArray<V3>& P, const TArray<V3>& N, const TArray<V3>& R, const TArray<V3>& U)
			: T(InT), RestP(P), RestN(N), RestR(R), RestU(U) {}

		int Vtx(int M, float s, float x, float z, const V3& Ln)
		{
			const TArray<float>& Arc = T.Arc;
			const int Last = Arc.Num() - 1;
			s = FMath::Clamp(s, 0.f, Arc[Last]);
			int i = 0;
			while (i < Last - 1 && Arc[i + 1] <= s) i++;
			const float f = Clamp01((s - Arc[i]) / FMath::Max(Arc[i + 1] - Arc[i], 1e-6f));
			FMeshSection& Sec = T.Sections[M];
			const V3 N0 = Normalized(Ln);
			const V3 R = LerpV(RestR[i], RestR[i + 1], f), U = LerpV(RestU[i], RestU[i + 1], f), N = LerpV(RestN[i], RestN[i + 1], f);
			const V3 P = LerpV(RestP[i], RestP[i + 1], f) + R * x + N * z;
			const int Index = Sec.Pos.Num();
			Sec.Pos.Add(P);
			Sec.Nrm.Add(Normalized(R * N0.X + U * N0.Y + N * N0.Z));
			Sec.Tan.Add(V4(R.X, R.Y, R.Z, 1.f));
			Sec.UV.Add(V2(x, P.Y));                                  // centre-front fabric coordinates (metres)
			Flat[M].Add(V3(x, -s, z));
			FlatN[M].Add(N0);
			T.Verts.Add({ M, Index, i, f, V3(x, 0.f, z), N0 });
			return Index;
		}

		/** Triangle facing along its vertices' normals. */
		void Tri(int M, int a, int b, int c)
		{
			const TArray<V3>& Fl = Flat[M];
			const V3 Fn = Cross(Fl[b] - Fl[a], Fl[c] - Fl[a]);
			if (Dot(Fn, FlatN[M][a] + FlatN[M][b] + FlatN[M][c]) < 0.f) Swap(b, c);
			T.Sections[M].Tris.Append({ a, b, c });
		}
		void Quad(int M, int a, int b, int c, int d) { Tri(M, a, b, c); Tri(M, a, c, d); }

		/** Raised band of half-width Hw and height Hz from arc length S0 to S1. */
		void Strip(int M, float S0, float S1, float Hw, float Hz)
		{
			int PL = -1, PR = -1, PWl = -1, PWl0 = -1, PWr = -1, PWr0 = -1;
			for (int i = 0; i < T.Arc.Num(); i++)
			{
				const float s = T.Arc[i];
				if (s < S0 || s > S1) continue;
				const int L = Vtx(M, s, -Hw, Hz, VFwd), R = Vtx(M, s, Hw, Hz, VFwd);
				const int Wl = Vtx(M, s, -Hw, Hz, -VRight), Wl0 = Vtx(M, s, -Hw, 0.f, -VRight);
				const int Wr = Vtx(M, s, Hw, Hz, VRight), Wr0 = Vtx(M, s, Hw, 0.f, VRight);
				if (PL >= 0)
				{
					Quad(M, PL, PR, R, L);
					Quad(M, PWl, Wl, Wl0, PWl0);
					Quad(M, PWr, Wr, Wr0, PWr0);
				}
				PL = L; PR = R; PWl = Wl; PWl0 = Wl0; PWr = Wr; PWr0 = Wr0;
			}
		}

		/** Box centred at arc length s and offset x, half-sizes Hx (across) and Hs (along), from
		 * height Z0 to Z1 above the cloth. Top and four sides. */
		void Box(int M, float s, float x, float Hx, float Hs, float Z0, float Z1)
		{
			const float Sa = s - Hs, Sb = s + Hs, Xa = x - Hx, Xb = x + Hx;
			Quad(M, Vtx(M, Sa, Xa, Z1, VFwd), Vtx(M, Sa, Xb, Z1, VFwd), Vtx(M, Sb, Xb, Z1, VFwd), Vtx(M, Sb, Xa, Z1, VFwd));
			Quad(M, Vtx(M, Sa, Xa, Z1, VUp), Vtx(M, Sa, Xb, Z1, VUp), Vtx(M, Sa, Xb, Z0, VUp), Vtx(M, Sa, Xa, Z0, VUp));
			Quad(M, Vtx(M, Sb, Xa, Z1, -VUp), Vtx(M, Sb, Xb, Z1, -VUp), Vtx(M, Sb, Xb, Z0, -VUp), Vtx(M, Sb, Xa, Z0, -VUp));
			Quad(M, Vtx(M, Sa, Xa, Z1, -VRight), Vtx(M, Sb, Xa, Z1, -VRight), Vtx(M, Sb, Xa, Z0, -VRight), Vtx(M, Sa, Xa, Z0, -VRight));
			Quad(M, Vtx(M, Sa, Xb, Z1, VRight), Vtx(M, Sb, Xb, Z1, VRight), Vtx(M, Sb, Xb, Z0, VRight), Vtx(M, Sa, Xb, Z0, VRight));
		}

		/** Round button of radius r and height h on top of Z0: dished face and rim (sewn button),
		 * or a dome (metal shank). */
		void Button(int M, float s, float r, float h, float Z0, bool bDome)
		{
			constexpr int N = 16;
			int Rim[N], Inner[N], Side[N], Foot[N];
			const float Ri = bDome ? 0.6f * r : 0.72f * r, Zi = bDome ? Z0 + 0.95f * h : Z0 + 0.8f * h;
			for (int k = 0; k < N; k++)
			{
				const float a = k * PI * 2.f / N, ca = FMath::Cos(a), sa = FMath::Sin(a);
				// local y (up) is -s along the chain
				Rim[k] = Vtx(M, s - sa * r, ca * r, Z0 + (bDome ? 0.55f : 1.f) * h, V3(ca * 0.6f, sa * 0.6f, 0.8f));
				Inner[k] = Vtx(M, s - sa * Ri, ca * Ri, Zi, bDome ? V3(ca * 0.35f, sa * 0.35f, 0.94f) : V3(-ca * 0.3f, -sa * 0.3f, 0.95f));
				Side[k] = Vtx(M, s - sa * r, ca * r, Z0 + (bDome ? 0.55f : 1.f) * h, V3(ca, sa, 0.f));
				Foot[k] = Vtx(M, s - sa * r, ca * r, Z0, V3(ca, sa, 0.f));
			}
			const int Centre = Vtx(M, s, 0.f, bDome ? Z0 + h : Z0 + 0.75f * h, VFwd);
			for (int k = 0; k < N; k++)
			{
				const int K1 = (k + 1) % N;
				Tri(M, Centre, Inner[k], Inner[K1]);
				Quad(M, Inner[k], Rim[k], Rim[K1], Inner[K1]);
				Quad(M, Side[k], Foot[k], Foot[K1], Side[K1]);
			}
		}
	};

	TSharedPtr<FGarmentTrims> FGarmentTrims::Build(const FGarmentMeshData& D, const FRestSkeleton& S, const FGarmentSpec& G)
	{
		if (G.Placket == 0) return nullptr;
		const float u = S.u;
		const float YTop = S.L + (G.Neck == ENeckline::Collar ? 0.075f : 0.03f) * u;
		const float YBottom = FMath::Lerp(G.Hem * S.L, YTop, 1.f - Clamp01(G.PlacketLen)) + 0.012f * u;

		// ---- chain down the centre front, from the first hit below the top to the first miss
		const TArray<int32> Front = FrontTriangles(D);
		TArray<FHit> Pts;
		TArray<V3> RestP, RestN;
		for (float y = YTop; y >= YBottom; y -= 0.01f)
		{
			FHit Hit; V3 P;
			if (!RayFront(D, Front, 0.f, y, Hit, P))
			{
				if (Pts.Num() > 0) break;
				continue;
			}
			Pts.Add(Hit);
			RestP.Add(P);
			RestN.Add(Normalized(D.RestN[Hit.a] * Hit.wa + D.RestN[Hit.b] * Hit.wb + D.RestN[Hit.c] * Hit.wc));
		}
		const int N = Pts.Num();
		if (N < 3) return nullptr;

		TSharedPtr<FGarmentTrims> T = MakeShared<FGarmentTrims>();
		T->Ta.SetNum(N); T->Tb.SetNum(N); T->Tc.SetNum(N);
		T->Wa.SetNum(N); T->Wb.SetNum(N); T->Wc.SetNum(N); T->Arc.SetNum(N);
		T->Cp.SetNum(N); T->Cr.SetNum(N); T->Cu.SetNum(N); T->Cn.SetNum(N);
		for (int i = 0; i < N; i++)
		{
			T->Ta[i] = Pts[i].a; T->Tb[i] = Pts[i].b; T->Tc[i] = Pts[i].c;
			T->Wa[i] = Pts[i].wa; T->Wb[i] = Pts[i].wb; T->Wc[i] = Pts[i].wc;
			T->Arc[i] = i == 0 ? 0.f : T->Arc[i - 1] + V3::Distance(RestP[i], RestP[i - 1]);
		}
		TArray<V3> RR, RU;
		Frames(RestP, RestN, RR, RU);

		// ---- geometry, in the chain's frames
		FTrimBuilder B(*T, RestP, RestN, RR, RU);
		const float End = T->Arc[N - 1];
		if (G.Placket == 1)
		{
			const float Hw = 0.017f * u, Hz = 0.0012f * u;
			B.Strip(Fabric, 0.f, End, Hw, Hz);
			const bool bShank = G.bMetalButtons;
			const float r = (bShank ? 0.008f : 0.0065f) * u, Pitch = 0.085f * u;
			for (float Sc = 0.03f * u; Sc < End - r; Sc += Pitch)
				B.Button(bShank ? Metal : Plastic, Sc, r, (bShank ? 0.0032f : 0.0022f) * u, Hz, bShank);
		}
		else
		{
			const float Tape = 0.007f * u, Hz = 0.0008f * u, Pitch = 0.0028f * u;
			B.Strip(Plastic, 0.f, End, Tape, Hz);
			int k = 0;
			for (float Sc = 0.026f * u; Sc < End - 0.004f * u; Sc += Pitch, k++)
				B.Box(Metal, Sc, (k % 2 == 0 ? -1.f : 1.f) * 0.0013f * u, 0.0022f * u, 0.0008f * u, Hz, Hz + 0.0014f * u);
			B.Box(Metal, 0.016f * u, 0.f, 0.0055f * u, 0.009f * u, Hz, Hz + 0.0045f * u);                  // slider
			B.Box(Metal, 0.032f * u, 0.f, 0.003f * u, 0.011f * u, Hz + 0.0045f * u, Hz + 0.006f * u);      // pull tab
		}
		return T;
	}

	void FGarmentTrims::Update(const TArray<V3>& X, const TArray<V3>& N)
	{
		const int C = Cp.Num();
		for (int i = 0; i < C; i++)
		{
			Cp[i] = X[Ta[i]] * Wa[i] + X[Tb[i]] * Wb[i] + X[Tc[i]] * Wc[i];
			Cn[i] = Normalized(N[Ta[i]] * Wa[i] + N[Tb[i]] * Wb[i] + N[Tc[i]] * Wc[i]);
		}
		for (int i = 0; i < C; i++)
		{
			const V3 Up = Cp[FMath::Max(i - 1, 0)] - Cp[FMath::Min(i + 1, C - 1)];
			Cr[i] = Normalized(Cross(Up, Cn[i]));
			Cu[i] = Cross(Cn[i], Cr[i]);
		}
		for (const FVert& V : Verts)
		{
			const int i = V.Chain, j = FMath::Min(i + 1, C - 1);
			const float f = V.F;
			const V3 R = LerpU(Cr[i], Cr[j], f), U = LerpU(Cu[i], Cu[j], f), Nn = LerpU(Cn[i], Cn[j], f);
			FMeshSection& S = Sections[V.Section];
			S.Pos[V.Index] = LerpU(Cp[i], Cp[j], f) + R * V.Local.X + U * V.Local.Y + Nn * V.Local.Z;
			S.Nrm[V.Index] = Normalized(R * V.LocalN.X + U * V.LocalN.Y + Nn * V.LocalN.Z);
			S.Tan[V.Index] = V4(R.X, R.Y, R.Z, 1.f);
		}
	}
}
