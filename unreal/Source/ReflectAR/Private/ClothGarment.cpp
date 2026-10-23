#include "ClothGarment.h"
#include "Async/Async.h"

namespace ReflectAR
{
	static constexpr int MaxVerts = 4000;

	// ---------------------------------------------------------------- bones
	/** Frame with Y along axisY and Z toward refZ (Unity: TRS(origin, LookRotation(z, y))). */
	static FFrame Frame(const V3& Origin, const V3& AxisY, const V3& RefZ)
	{
		const V3 y = Normalized(AxisY);
		V3 z = ProjectOnPlane(RefZ, y);
		if (z.SizeSquared() < 1e-6f) z = ProjectOnPlane(VFwd, y);
		return FFrame::Look(Origin, Normalized(z), y);
	}

	static void RestFramesOf(const FRestSkeleton& s, FFrame Out[6])
	{
		const V3 dl = FRestSkeleton::ArmDir(-1), dr = FRestSkeleton::ArmDir(1);
		const FFrame UpL = Frame(s.lSh, dl, VFwd), UpR = Frame(s.rSh, dr, VFwd);
		Out[0] = Frame(s.hipC, VUp, VFwd);
		Out[1] = Frame(s.shC, VUp, VFwd);
		Out[2] = UpL; Out[3] = UpR;
		Out[4] = Frame(s.lEl, dl, UpL.z);
		Out[5] = Frame(s.rEl, dr, UpR.z);
	}

	// ---------------------------------------------------------------- (re)build
	// Generating a garment takes 100-250 ms, so the mesh and its constraints are built on a worker
	// thread; only the swap happens on the game thread.

	bool FClothGarment::Drifted() const
	{
		auto D = [](float a, float b) { return FMath::Abs(a - b) / FMath::Max(b, 1e-3f); };
		if (D(Body.ShoulderW, Built.sw) > 0.25f || D(Body.TorsoLen, Built.L) > 0.25f
			|| D(Body.UpperL, Built.upperL) > 0.3f || D(Body.UpperR, Built.upperR) > 0.3f
			|| D(Body.ForeL, Built.foreL) > 0.3f || D(Body.ForeR, Built.foreR) > 0.3f) return true;
		for (int i = 0; i < 6; i++)
			if (FMath::Abs(Body.Profile[i] - Built.Profile[i]) > 0.08f) return true;
		return false;
	}

	void FClothGarment::Prepare(double Now)
	{
		if (!Body.Valid || Pending.IsValid()) return;
		bool bRebuild = !bHasMesh;
		if (bHasMesh)
		{
			// only a sustained change is worth a rebuild (it resets the cloth); tracking jitter is not
			const bool bDrift = Drifted();
			if (!bDrift) DriftSince = -1;
			else if (DriftSince < 0) DriftSince = Now;
			const double Cooldown = Now - FirstApplied < 15 ? 3 : 10;     // the measured profile settles first
			bRebuild = bDrift && Now - DriftSince > 1.5 && Now - LastApplied > Cooldown;
		}
		if (!bRebuild) return;
		const FRestSkeleton Skel = FRestSkeleton::From(Body);
		const FGarmentSpec* S = &Spec;
		Pending = Async(EAsyncExecution::ThreadPool, [Skel, S]() { return Generate(Skel, S); });
	}

	TSharedPtr<FClothGarment::FBuilt> FClothGarment::Generate(FRestSkeleton Skel, const FGarmentSpec* SpecPtr)
	{
		const FGarmentSpec& S = *SpecPtr;
		const double T0 = FPlatformTime::Seconds();
		// detail relative to the body, capped: solver cost grows with the vertex count
		const float Cell = GarmentMesher::DefaultCell * FMath::Max(Skel.u, 0.8f);
		TSharedPtr<FGarmentMeshData> D = GarmentMesher::Build(Skel, S, Cell);
		if (D->Rest.Num() > MaxVerts)
			D = GarmentMesher::Build(Skel, S, Cell * FMath::Sqrt(D->Rest.Num() / (MaxVerts * 0.9f)));
		TSharedPtr<FBuilt> R = MakeShared<FBuilt>();
		R->Skel = Skel; R->Data = D;
		const int N = D->Rest.Num();
		R->MaxDist.SetNum(N); R->SkinK.SetNum(N);
		for (int i = 0; i < N; i++)
		{
			const float l = D->Looseness[i];
			R->MaxDist[i] = Lerp(0.002f, (0.03f + S.Ease) * S.Drape * Skel.u, FMath::Pow(l, 1.4f));
			R->SkinK[i] = Lerp(0.6f, 0.015f, FMath::Pow(l, 0.7f));
		}
		const TArray<int32>& Tris = D->Tris;
		TMap<int64, int32> EdgeSet;                                 // edge -> first opposite vertex
		auto Key = [](int32 a, int32 b) { return a < b ? (int64(a) << 32) | uint32(b) : (int64(b) << 32) | uint32(a); };
		for (int t = 0; t < Tris.Num(); t += 3)
			for (int e = 0; e < 3; e++)
			{
				const int a = Tris[t + e], b = Tris[t + (e + 1) % 3], Opp = Tris[t + (e + 2) % 3];
				const int64 K = Key(a, b);
				if (const int32* O = EdgeSet.Find(K)) { if (*O != Opp) { R->BA.Add(*O); R->BB.Add(Opp); } }   // bending across the shared edge
				else { EdgeSet.Add(K, Opp); R->EA.Add(a); R->EB.Add(b); }
			}
		R->ERest.SetNum(R->EA.Num()); R->BRest.SetNum(R->BA.Num());
		for (int i = 0; i < R->EA.Num(); i++) R->ERest[i] = V3::Distance(D->Rest[R->EA[i]], D->Rest[R->EB[i]]);
		for (int i = 0; i < R->BA.Num(); i++) R->BRest[i] = V3::Distance(D->Rest[R->BA[i]], D->Rest[R->BB[i]]);
		R->Ms = float((FPlatformTime::Seconds() - T0) * 1000.0);
		return R;
	}

	void FClothGarment::ApplyPending(double Now)
	{
		if (!Pending.IsValid() || !Pending.IsReady()) return;
		TSharedPtr<FBuilt> R = Pending.Get();
		Pending.Reset();
		if (!R.IsValid() || !R->Data.IsValid() || R->Data->Rest.Num() == 0) return;
		Built = R->Skel; Data = R->Data;
		MaxDist = MoveTemp(R->MaxDist); SkinK = MoveTemp(R->SkinK);
		EA = MoveTemp(R->EA); EB = MoveTemp(R->EB); BA = MoveTemp(R->BA); BB = MoveTemp(R->BB);
		ERest = MoveTemp(R->ERest); BRest = MoveTemp(R->BRest);
		const int N = Data->Rest.Num();
		X.SetNum(N); Prev.SetNum(N); Target.SetNum(N); LastTarget.SetNum(N); SimN.SetNum(N);

		const int Rn = Data->RenderToSim.Num();
		BodySection = FMeshSection();
		BodySection.Pos.SetNum(Rn); BodySection.Nrm.SetNum(Rn); BodySection.Tan.SetNum(Rn);
		BodySection.UV = Data->UV;
		BodySection.Tris = Data->RenderTris;
		TAcc.SetNum(Rn); BAcc.SetNum(Rn);
		for (int i = 0; i < Rn; i++)
		{
			BodySection.Pos[i] = Data->Rest[Data->RenderToSim[i]];
			BodySection.Nrm[i] = Data->RestN[Data->RenderToSim[i]];
			BodySection.Tan[i] = V4(1, 0, 0, 1);
		}
		Trims = FGarmentTrims::Build(*Data, Built, Spec);
		RestFramesOf(Built, RestFrames);
		bHasMesh = true;
		bSimInit = false;
		bTopologyChanged = true;
		LastApplied = Now;
		if (RebuildCount == 0) FirstApplied = Now;
		DriftSince = -1;
		LastBuildMs = R->Ms;
		RebuildCount++;
	}

	void FClothGarment::SkinTargets()
	{
		const V3* J = Body.J;
		const V3 dUL = J[LM::LElbow] - J[LM::LShoulder], dUR = J[LM::RElbow] - J[LM::RShoulder];
		const FFrame UpL = Frame(J[LM::LShoulder], dUL, Body.Fwd), UpR = Frame(J[LM::RShoulder], dUR, Body.Fwd);
		const FFrame Cur[6] =
		{
			Body.PelvisFrame(), Body.ChestFrame(), UpL, UpR,
			Frame(J[LM::LElbow], J[LM::LWrist] - J[LM::LElbow], UpL.z),
			Frame(J[LM::RElbow], J[LM::RWrist] - J[LM::RElbow], UpR.z),
		};
		for (int b = 0; b < 6; b++) SkinM[b] = FAffine::Between(RestFrames[b], Cur[b]);
		const TArray<FBoneWeights>& W = Data->Weights; const TArray<V3>& R = Data->Rest;
		for (int i = 0; i < R.Num(); i++)
		{
			const FBoneWeights& Bw = W[i];
			Target[i] = SkinM[Bw.Bone[0]].Apply(R[i]) * Bw.W[0] + SkinM[Bw.Bone[1]].Apply(R[i]) * Bw.W[1]
				+ SkinM[Bw.Bone[2]].Apply(R[i]) * Bw.W[2] + SkinM[Bw.Bone[3]].Apply(R[i]) * Bw.W[3];
		}
	}

	/** Capsules approximating the actual body (no ease), for cloth collisions. */
	void FClothGarment::BuildColliders()
	{
		Colliders.Reset();
		const float u = Body.Unit();
		for (int k = 0; k < 5; k++)
		{
			const float y = FMath::Lerp(Spec.Hem - 0.1f, 0.92f, k / 4.f) * Body.TorsoLen;
			float a, d;
			Built.TorsoRadii(y, a, d);
			a *= Body.ShoulderW / Built.sw; d *= 0.95f;
			const float w = SmoothStep(0.f, 1.f, (y / Body.TorsoLen - 0.1f) / 0.7f);
			const V3 r = SlerpDir(Body.PRight, Body.Right, w);
			const V3 c = Body.HipC + Body.Up * y;
			const float Half = FMath::Max(a - d, 0.005f);
			Colliders.Emplace(c - r * Half, c + r * Half, d);
		}
		const V3* J = Body.J;
		Colliders.Emplace(J[LM::LShoulder], J[LM::RShoulder], 0.05f * u);
		Colliders.Emplace(J[LM::LShoulder], J[LM::LElbow], 0.043f * u);
		Colliders.Emplace(J[LM::RShoulder], J[LM::RElbow], 0.043f * u);
		Colliders.Emplace(J[LM::LElbow], J[LM::LWrist], 0.036f * u);
		Colliders.Emplace(J[LM::RElbow], J[LM::RWrist], 0.036f * u);
		Colliders.Emplace(Body.ShC, Body.HeadC - Body.Up * 0.04f * u, 0.055f * u);
	}

	void FClothGarment::Outline(TArray<FCapsuleB>& Into) const
	{
		const float R = Room();
		for (int i = 0; i < 6 && i < Colliders.Num(); i++)      // 5 torso levels + the shoulder line
			Into.Emplace(Colliders[i].a, Colliders[i].b, Colliders[i].r + R);
	}

	// ---------------------------------------------------------------- simulation
	void FClothGarment::Step(float Dt, double Now)
	{
		if (!Body.Valid) return;
		ApplyPending(Now);
		Prepare(Now);
		if (!bHasMesh) return;
		const double T0 = FPlatformTime::Seconds();
		SkinTargets();
		BuildColliders();
		const int N = X.Num();
		if (!bSimInit) { X = Target; Prev = Target; LastTarget = Target; bSimInit = true; }
		// Carry the cloth along with the body's own motion: only (1 - Follow) of each movement is
		// left to inertia, so the garment doesn't trail behind the user but still swings locally.
		for (int i = 0; i < N; i++)
		{
			const V3 Moved = (Target[i] - LastTarget[i]) * Follow;
			X[i] += Moved; Prev[i] += Moved;
			LastTarget[i] = Target[i];
		}

		Dt = FMath::Min(Dt, 1.f / 30.f);
		const float h = Dt / Substeps, h2 = h * h;
		const V3 G = V3(0.f, -9.81f, 0.f) * Spec.Density * h2;
		const float Keep = 1.f - Clamp01(Spec.Damping * h * 60.f);
		const float AS = Spec.Stretch / h2, AC = Spec.Compress / h2, AB = Spec.Bend / h2;
		constexpr float Thick = 0.004f;
		const int NC = Colliders.Num();
		ColCenter.SetNum(NC); ColBound.SetNum(NC);
		for (int c = 0; c < NC; c++)
		{
			ColCenter[c] = (Colliders[c].a + Colliders[c].b) * 0.5f;
			const float Rr = V3::Distance(Colliders[c].a, Colliders[c].b) * 0.5f + Colliders[c].r + Thick;
			ColBound[c] = Rr * Rr;
		}
		const float SkinRate = h * 60.f;

		for (int s = 0; s < Substeps; s++)
		{
			// predict (Verlet with damping)
			for (int i = 0; i < N; i++)
			{
				const V3 V = (X[i] - Prev[i]) * Keep;
				Prev[i] = X[i];
				X[i] += V + G;
			}
			// stretch / compression (XPBD, one iteration per small substep)
			for (int k = 0; k < EA.Num(); k++)
			{
				const int a = EA[k], b = EB[k];
				const V3 D = X[a] - X[b];
				const float Len = D.Size();
				if (Len < 1e-7f) continue;
				const float C = Len - ERest[k];
				const float Dl = -C / (2.f + (C > 0 ? AS : AC));
				const V3 Corr = D * (Dl / Len);
				X[a] += Corr; X[b] -= Corr;
			}
			// bending (distance across each pair of triangles)
			for (int k = 0; k < BA.Num(); k++)
			{
				const int a = BA[k], b = BB[k];
				const V3 D = X[a] - X[b];
				const float Len = D.Size();
				if (Len < 1e-7f) continue;
				const float Dl = -(Len - BRest[k]) / (2.f + AB);
				const V3 Corr = D * (Dl / Len);
				X[a] += Corr; X[b] -= Corr;
			}
			// tether to the body-fitted shape + body collisions
			for (int i = 0; i < N; i++)
			{
				V3 P = X[i];
				const V3 T = Target[i];
				P = LerpU(P, T, 1.f - FMath::Pow(1.f - SkinK[i], SkinRate));
				const V3 Off = P - T;
				const float M = MaxDist[i];
				if (Off.SizeSquared() > M * M) P = T + Normalized(Off) * M;
				for (int c = 0; c < NC; c++)
				{
					if ((ColCenter[c] - P).SizeSquared() > ColBound[c]) continue;   // outside the capsule's bounding sphere
					const FCapsuleB& Cap = Colliders[c];
					const V3 Nrm = P - Cap.Closest(P);
					const float R = Cap.r + Thick, Dd = Nrm.SizeSquared();
					if (Dd < R * R && Dd > 1e-10f)
					{
						const float Dist = FMath::Sqrt(Dd);
						P += Nrm * ((R - Dist) / Dist);
						// friction: kill some tangential motion while in contact
						const V3 Vel = P - Prev[i];
						const V3 Vn = Nrm * (Dot(Vel, Nrm) / Dd);
						Prev[i] += (Vel - Vn) * 0.3f;
					}
				}
				X[i] = P;
			}
		}
		UpdateRenderMesh();
		if (Trims.IsValid()) Trims->Update(X, SimN);
		LastStepMs = float((FPlatformTime::Seconds() - T0) * 1000.0);
	}

	void FClothGarment::UpdateRenderMesh()
	{
		// normals on the welded mesh, so UV seams don't show in the shading
		GarmentMesher::Normals(X, Data->Tris, SimN);
		const TArray<int32>& ToSim = Data->RenderToSim;
		for (int r = 0; r < ToSim.Num(); r++) { BodySection.Pos[r] = X[ToSim[r]]; BodySection.Nrm[r] = SimN[ToSim[r]]; }
		Tangents();
	}

	/** Per-vertex tangent along the fabric's u direction, with handedness. */
	void FClothGarment::Tangents()
	{
		const TArray<V3>& P = BodySection.Pos;
		for (V3& t : TAcc) t = V3::ZeroVector;
		for (V3& b : BAcc) b = V3::ZeroVector;
		const TArray<int32>& Rt = Data->RenderTris; const TArray<V2>& UV = Data->UV;
		for (int t = 0; t < Rt.Num(); t += 3)
		{
			const int i0 = Rt[t], i1 = Rt[t + 1], i2 = Rt[t + 2];
			const V3 E1 = P[i1] - P[i0], E2 = P[i2] - P[i0];
			const V2 D1 = UV[i1] - UV[i0], D2 = UV[i2] - UV[i0];
			const float Det = D1.X * D2.Y - D2.X * D1.Y;
			if (FMath::Abs(Det) < 1e-12f) continue;
			const float Inv = 1.f / Det;
			const V3 T = (E1 * D2.Y - E2 * D1.Y) * Inv, B = (E2 * D1.X - E1 * D2.X) * Inv;
			TAcc[i0] += T; TAcc[i1] += T; TAcc[i2] += T;
			BAcc[i0] += B; BAcc[i1] += B; BAcc[i2] += B;
		}
		for (int r = 0; r < P.Num(); r++)
		{
			const V3 n = BodySection.Nrm[r];
			V3 T = TAcc[r] - n * Dot(n, TAcc[r]);
			if (T.SizeSquared() < 1e-12f) T = Cross(n, FMath::Abs(n.Y) < 0.9f ? VUp : VRight);
			T = Normalized(T);
			const float W = Dot(Cross(n, T), BAcc[r]) < 0.f ? -1.f : 1.f;
			BodySection.Tan[r] = V4(T.X, T.Y, T.Z, W);
		}
	}
}
