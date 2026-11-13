// Headless test bench for ReflectAR's garment code: builds garments, drives them with a synthetic
// (optionally noisy) skeleton, measures jitter / poke-through and renders stills.
#define private public
#include "CoreMinimal.h"
#include "BodyMath.h"
#include "BodyRig.h"
#include "GarmentSpec.h"
#include "GarmentMesher.h"
#include "GarmentTrims.h"
#include "ClothGarment.h"
#include "BodyProxy.h"
#undef private
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace ReflectAR;

float FMath::PerlinNoise2D(const FVector2D& p)
{
	auto H = [](int x, int y) { uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); };
	auto G = [&](int ix, int iy, double dx, double dy) { const double a = (H(ix, iy) & 0xffff) / 65536.0 * 6.2831853; return std::cos(a) * dx + std::sin(a) * dy; };
	const int x0 = int(std::floor(p.X)), y0 = int(std::floor(p.Y));
	const double fx = p.X - x0, fy = p.Y - y0;
	auto F = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
	const double u = F(fx), v = F(fy);
	const double a = G(x0, y0, fx, fy), b = G(x0 + 1, y0, fx - 1, fy), c = G(x0, y0 + 1, fx, fy - 1), d = G(x0 + 1, y0 + 1, fx - 1, fy - 1);
	return float((a + (b - a) * u + (c - a + (a - b - c + d) * u) * v) * 1.41);
}

// ------------------------------------------------------------------ synthetic skeleton
struct FPose { float Yaw = 0; int Arms = 0; float Lean = 0; float T = 0; float Wide = 1; };   // Arms: 0 down, 1 forward, 2 belly, 3 raised, 4 swing, 5 T-pose, 6 one arm across chest

static void Skeleton(const FPose& P, V3* J)
{
	for (int i = 0; i < LM::Count; i++) J[i] = V3::ZeroVector;
	auto S = [&](int i, float x, float y, float z) { J[i] = V3(x, y, z); };
	// body frame: x = user's right, y up (hips 0), z = forward (the way the user faces)
	S(0, 0, 0.70f, 0.10f); S(2, -0.032f, 0.735f, 0.08f); S(5, 0.032f, 0.735f, 0.08f); S(7, -0.075f, 0.70f, 0); S(8, 0.075f, 0.70f, 0);
	const float sw = 0.18f * P.Wide;
	S(11, -sw, 0.50f, 0); S(12, sw, 0.50f, 0);
	S(23, -0.10f, 0, 0); S(24, 0.10f, 0, 0); S(25, -0.11f, -0.45f, 0.02f); S(26, 0.11f, -0.45f, 0.02f); S(27, -0.11f, -0.88f, 0); S(28, 0.11f, -0.88f, 0);
	for (int s = -1; s <= 1; s += 2)
	{
		const int sh = s < 0 ? 11 : 12, e = s < 0 ? 13 : 14, w = s < 0 ? 15 : 16, pk = s < 0 ? 17 : 18, ix = s < 0 ? 19 : 20, th = s < 0 ? 21 : 22;
		const V3 Sh = J[sh];
		V3 E, W, Hd(0, -0.07f, 0.02f);
		switch (P.Arms)
		{
		default: E = Sh + V3(s * 0.04f, -0.28f, 0); W = E + V3(s * 0.02f, -0.25f, 0.03f); break;
		case 1: E = Sh + V3(s * 0.03f, -0.10f, 0.26f); W = E + V3(0, 0.02f, 0.25f); Hd = V3(0, 0, 0.08f); break;
		case 2: E = Sh + V3(s * 0.04f, -0.28f, 0.03f); W = V3(s * 0.06f, 0.16f, 0.15f); Hd = V3(-s * 0.05f, -0.02f, 0.01f); break;
		case 3: E = Sh + V3(s * 0.20f, 0.12f, 0.03f); W = E + V3(s * 0.03f, 0.25f, 0.03f); Hd = V3(0, 0.08f, 0); break;
		case 4: { const float a = 0.5f * FMath::Sin(P.T * 2.f + (s < 0 ? 0 : PI)); E = Sh + V3(s * 0.04f, -0.28f * FMath::Cos(a), 0.28f * FMath::Sin(a)); W = E + V3(s * 0.02f, -0.25f * FMath::Cos(a), 0.25f * FMath::Sin(a) + 0.03f); break; }
		case 5: E = Sh + V3(s * 0.28f, 0, 0); W = E + V3(s * 0.25f, 0, 0); Hd = V3(s * 0.08f, 0, 0); break;
		case 6: if (s > 0) { E = Sh + V3(0.02f, -0.24f, 0.14f); W = V3(-0.05f, 0.33f, 0.17f); Hd = V3(-0.07f, 0.02f, 0); } else { E = Sh + V3(s * 0.04f, -0.28f, 0); W = E + V3(s * 0.02f, -0.25f, 0.03f); } break;
		}
		J[e] = E; J[w] = W; J[pk] = W + Hd + V3(s * 0.01f, 0, -0.02f); J[ix] = W + Hd * 1.2f + V3(-s * 0.01f, 0, 0.02f); J[th] = W + Hd * 0.5f + V3(-s * 0.02f, 0, 0.03f);
	}
	for (int k : { 1, 3, 4, 6, 9, 10 }) J[k] = J[0];
	for (int k = 29; k < 33; k++) J[k] = J[27 + (k & 1)];
	// lean (pitch about x at the hips), yaw about y, then stand 1.8 m in front of the screen
	const float cl = FMath::Cos(P.Lean), sl = FMath::Sin(P.Lean), cy = FMath::Cos(P.Yaw), sy = FMath::Sin(P.Yaw);
	for (int i = 0; i < LM::Count; i++)
	{
		V3 p = J[i];
		if (p.Y > 0) p = V3(p.X, p.Y * cl - p.Z * sl, p.Y * sl + p.Z * cl);
		p = V3(p.X * cy + p.Z * sy, p.Y, -p.X * sy + p.Z * cy);
		J[i] = p + V3(0.151f, -0.05f, -1.8f);
	}
}

struct FNoise
{
	std::mt19937 Rng{ 7 };
	V3 N[LM::Count];
	float SigXY = 0.0012f, SigZ = 0.006f, Tc = 0.13f;    // residual after perception's filters: slow wobble
	void Apply(V3* J, float Dt)
	{
		std::normal_distribution<float> D(0, 1);
		const float a = FMath::Exp(-Dt / Tc), s = FMath::Sqrt(1 - a * a);
		for (int i = 0; i < LM::Count; i++)
		{
			N[i] = N[i] * a + V3(D(Rng) * SigXY, D(Rng) * SigXY, D(Rng) * SigZ) * s;
			J[i] += N[i];
		}
	}
};

/** What a monocular pose model reports: image positions right, depth relative to the torso scaled by Flat. */
static void Flatten(V3* J, float Flat, const V3& Cam = V3(0.151f, 0.204f, 0.f))
{
	float Rho[LM::Count], Root = 0; V3 Dir[LM::Count];
	for (int i = 0; i < LM::Count; i++) { const V3 d = J[i] - Cam; Rho[i] = d.Size(); Dir[i] = d / Rho[i]; }
	for (int i : { 11, 12, 23, 24 }) Root += Rho[i] * 0.25f;
	for (int i = 0; i < LM::Count; i++) J[i] = Cam + Dir[i] * (Root + (Rho[i] - Root) * Flat);
}
static bool GStab = true;

// ------------------------------------------------------------------ software rasteriser
struct FCam { V3 Eye, R, U, F; float Foc; int W, H; };
static FCam LookAt(const V3& Eye, const V3& At, int W, int H, float FovDeg)
{
	FCam C; C.Eye = Eye; C.F = Normalized(At - Eye); C.R = Normalized(Cross(VUp, C.F)); C.U = Cross(C.F, C.R);
	if (Dot(C.R, VRight) < 0 && FMath::Abs(C.F.Z) > 0.7f) { }   // keep handedness as is
	C.W = W; C.H = H; C.Foc = 0.5f * W / FMath::Tan(FMath::DegreesToRadians(FovDeg) * 0.5f);
	return C;
}
struct FImage
{
	int W, H; std::vector<float> Z; std::vector<uint8> Rgb; std::vector<int> Id;
	FImage(int w, int h) : W(w), H(h), Z(size_t(w) * h, 1e9f), Rgb(size_t(w) * h * 3, 30), Id(size_t(w) * h, 0) {}
	void Save(const std::string& Path) const { FILE* f = fopen(Path.c_str(), "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(Rgb.data(), 1, Rgb.size(), f); fclose(f); }
};
typedef std::function<V3(float u, float v, const V3& n, const V3& base)> FShade;
static void Raster(FImage& Im, const FCam& C, const TArray<V3>& P, const TArray<V3>& N, const TArray<int32>& T, const V3& Color, int Id,
	const TArray<V2>* UV = nullptr, const std::function<V3(V2)>* Tex = nullptr, const std::vector<char>* SkipVert = nullptr, float Flip = 1.f, const TArray<FColor>* VC = nullptr)
{
	const V3 Key = Normalized(V3(0.7f, 0.6f, 0.45f)), Fill = Normalized(V3(-0.7f, 0.2f, 0.5f));
	std::vector<V3> S(size_t(P.Num()));
	for (int i = 0; i < P.Num(); i++)
	{
		const V3 d = P[i] - C.Eye; const float z = Dot(d, C.F);
		S[size_t(i)] = V3(C.W * 0.5f + Flip * C.Foc * Dot(d, C.R) / z, C.H * 0.5f - C.Foc * Dot(d, C.U) / z, z);
	}
	for (int t = 0; t < T.Num(); t += 3)
	{
		const int ia = T[t], ib = T[t + 1], ic = T[t + 2];
		if (SkipVert && ((*SkipVert)[size_t(ia)] || (*SkipVert)[size_t(ib)] || (*SkipVert)[size_t(ic)])) continue;
		const V3 a = S[size_t(ia)], b = S[size_t(ib)], c = S[size_t(ic)];
		if (a.Z < 0.05f || b.Z < 0.05f || c.Z < 0.05f) continue;
		const float Area = (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
		if (FMath::Abs(Area) < 1e-9f) continue;
		const int x0 = FMath::Max(0, int(FMath::Min3(a.X, b.X, c.X))), x1 = FMath::Min(Im.W - 1, int(FMath::Max3(a.X, b.X, c.X)) + 1);
		const int y0 = FMath::Max(0, int(FMath::Min3(a.Y, b.Y, c.Y))), y1 = FMath::Min(Im.H - 1, int(FMath::Max3(a.Y, b.Y, c.Y)) + 1);
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
			{
				const float px = x + 0.5f, py = y + 0.5f;
				const float w0 = ((b.X - px) * (c.Y - py) - (c.X - px) * (b.Y - py)) / Area;
				const float w1 = ((c.X - px) * (a.Y - py) - (a.X - px) * (c.Y - py)) / Area;
				const float w2 = 1 - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				const float z = 1.f / (w0 / a.Z + w1 / b.Z + w2 / c.Z);
				const size_t k = size_t(y) * Im.W + x;
				if (z >= Im.Z[k]) continue;
				Im.Z[k] = z; Im.Id[k] = Id;
				const float p0 = w0 / a.Z * z, p1 = w1 / b.Z * z, p2 = w2 / c.Z * z;
				V3 n = Normalized(N[ia] * p0 + N[ib] * p1 + N[ic] * p2);
				const V3 wp = P[ia] * p0 + P[ib] * p1 + P[ic] * p2;
				if (Dot(n, C.Eye - wp) < 0) n = -n;
				V3 Base = Color;
				if (UV && Tex) Base = (*Tex)((*UV)[ia] * p0 + (*UV)[ib] * p1 + (*UV)[ic] * p2);
				const float L = 0.28f + 0.62f * FMath::Max(0.f, Dot(n, Key)) + 0.18f * FMath::Max(0.f, Dot(n, Fill));
				float Shade = 1.f;
				if (VC && VC->Num() == P.Num())
				{
					const float Ao = ((*VC)[ia].R * p0 + (*VC)[ib].R * p1 + (*VC)[ic].R * p2) / 255.f, Ed = ((*VC)[ia].G * p0 + (*VC)[ib].G * p1 + (*VC)[ic].G * p2) / 255.f * 0.06f;
					Shade = Ao * (1.f - 0.2f * Clamp01(1.f - FMath::Abs(Ed - 0.016f) / 0.0025f));
				}
				const V3 Out = Base * (L * Shade);
				auto Enc = [](float v) { return uint8(255.f * FMath::Pow(Clamp01(v), 1 / 2.2f)); };
				Im.Rgb[k * 3] = Enc(Out.X); Im.Rgb[k * 3 + 1] = Enc(Out.Y); Im.Rgb[k * 3 + 2] = Enc(Out.Z);
			}
	}
}

// ------------------------------------------------------------------ scene
struct FScene
{
	FBodyRig Rig;
	FBodyProxy Proxy;
	std::unique_ptr<FClothGarment> G;
	const FGarmentSpec* Spec = nullptr;
	double Now = 0;
	bool bFirst = true;
	void Init(int Gi) { Spec = &GarmentCatalog()[Gi]; G.reset(new FClothGarment(*Spec, Rig)); Rig.bStabilise = GStab; }
	void Step(const V3* J, float Dt)
	{
		float Vis[LM::Count]; for (float& v : Vis) v = 0.99f;
		Rig.Update(J, Vis, Dt, bFirst); bFirst = false;
		Now += Dt;
		G->Step(Dt, Now);
		Proxy.Update(Rig, true, Spec->bTank ? 0.f : Spec->Sleeve, nullptr, G.get());
	}
	// part id of each proxy vertex
	std::vector<int> ProxyPart() const
	{
		std::vector<int> Out;
		for (int i = 0; i < FBodyProxy::Total; i++)
		{
			const int n = Proxy.Parts[i].bSphere ? Proxy.SphereV.Num() : Proxy.CapsuleV.Num();
			for (int k = 0; k < n; k++) Out.push_back(i);
		}
		return Out;
	}
	/** Renders garment + body proxies; returns the number of pixels where a part that should be
	 * under the garment (sleeved arm = 2, torso = 3) shows in front of it. */
	void Render(const FCam& C, const std::string& Path, int* PokeArm = nullptr, int* PokeTorso = nullptr, bool bProxy = true)
	{
		FImage Im(C.W, C.H);
		const V3 Col(Spec->ColorA.R, Spec->ColorA.G, Spec->ColorA.B);
		// garment alone first: its coverage
		FImage Gm(C.W, C.H);
		if (G->HasMesh()) Raster(Gm, C, G->BodySection.Pos, G->BodySection.Nrm, G->BodySection.Tris, Col, 1);
		if (G->HasMesh()) Raster(Im, C, G->BodySection.Pos, G->BodySection.Nrm, G->BodySection.Tris, Col, 1, nullptr, nullptr, nullptr, 1.f, &G->BodySection.Col);
		if (G->HasMesh() && G->Trims.IsValid())
			for (int s = 0; s < 3; s++) Raster(Im, C, G->Trims->Sections[s].Pos, G->Trims->Sections[s].Nrm, G->Trims->Sections[s].Tris, s == 0 ? Col * 0.9f : V3(0.8f, 0.8f, 0.78f), 1);
		if (bProxy)
		{
			const std::vector<int> Part = ProxyPart();
			// pushed-back copy, like M_Proxy's world position offset
			TArray<V3> Pp = Proxy.Mesh.Pos;
			for (int i = 0; i < Pp.Num(); i++) Pp[i] += Normalized(Pp[i] - C.Eye) * Proxy.UV2[i].Y;
			for (int Pass = 0; Pass < 3; Pass++)
			{
				std::vector<char> Skip(size_t(Proxy.Mesh.Pos.Num()));
				for (size_t i = 0; i < Skip.size(); i++)
				{
					const int p = Part[i];
					const bool bArm = p >= FBodyProxy::Covered && p < FBodyProxy::Exposed, bTorso = p == FBodyProxy::Torso;
					const bool bShell = Proxy.UV3[int(i)].X > 0.5f;
					Skip[i] = bShell || (Pass == 0 ? !bArm : Pass == 1 ? !bTorso : (bArm || bTorso));
				}
				Raster(Im, C, Pp, Proxy.Mesh.Nrm, Proxy.Mesh.Tris, Pass == 0 ? V3(1, 0.1f, 0.6f) : Pass == 1 ? V3(0.2f, 0.9f, 1) : V3(0.75f, 0.58f, 0.48f), 2 + Pass, nullptr, nullptr, &Skip);
			}
		}
		int Pa = 0, Pt = 0;
		for (size_t k = 0; k < Im.Id.size(); k++)
			if (Gm.Id[k] == 1) { if (Im.Id[k] == 2) Pa++; if (Im.Id[k] == 3) Pt++; }
		if (PokeArm) *PokeArm = Pa;
		if (PokeTorso) *PokeTorso = Pt;
		if (!Path.empty()) Im.Save(Path);
	}
};

static FCam Webcam(int W = 640, int H = 800) { return LookAt(V3(0.151f, 0.25f, 0.f), V3(0.151f, 0.20f, -1.8f), W, H, 30.f); }
static FCam Orbit(float Deg, int W = 640, int H = 800)
{
	const float a = FMath::DegreesToRadians(Deg);
	const V3 At(0.151f, 0.20f, -1.8f);
	return LookAt(At + V3(FMath::Sin(a), 0.03f, FMath::Cos(a)) * 1.8f, At, W, H, 30.f);
}

int main(int argc, char** argv)
{
	if (getenv("STAB")) GStab = atoi(getenv("STAB")) != 0;
	const std::string Mode = argc > 1 ? argv[1] : "stills";
	const std::string Out = argc > 2 ? argv[2] : "out";
	const TArray<FGarmentSpec>& Cat = GarmentCatalog();

	if (Mode == "stills" || Mode == "poses")
	{
		// every garment, settled 2 s in a pose, from the webcam and from 60 degrees to the side
		const int Arms = argc > 3 ? atoi(argv[3]) : 0;
		const float Yaw = argc > 4 ? FMath::DegreesToRadians(float(atof(argv[4]))) : 0.f;
		for (int g = 0; g < Cat.Num(); g++)
		{
			FScene S; S.Init(g);
			V3 J[LM::Count]; FPose P; P.Arms = Arms; P.Yaw = Yaw;
			for (int f = 0; f < 150; f++)
			{
				// ease into the pose over the first second, like a user moving there
				FPose Q = P; Q.Arms = f < 30 ? 0 : P.Arms; Q.Yaw = Yaw * Clamp01((f - 30) / 45.f);
				Skeleton(Q, J); S.Step(J, 1 / 60.f);
			}
			int Pa, Pt;
			S.Render(Webcam(), Out + "_" + std::to_string(g) + "_front.ppm", &Pa, &Pt);
			S.Render(Orbit(55), Out + "_" + std::to_string(g) + "_side.ppm");
			printf("%-20s verts %4d  build %.0f ms  step %.2f ms  poke arm %d torso %d px\n", *Cat[g].Name, S.G->VertexCount(), S.G->LastBuildMs, S.G->LastStepMs, Pa, Pt);
		}
	}
	else if (Mode == "jitter")
	{
		// a user standing still: how much does the cloth move per frame, with and without tracking noise
		printf("%-20s | still: cloth mm/frame (p95) | noisy, on screen: target mm/f, cloth mm/f (p95), hem mm/f\n", "garment");
		for (int g = 0; g < Cat.Num(); g++)
		{
			float Res[2][4] = {};
			for (int Noisy = 0; Noisy < 2; Noisy++)
			{
				FScene S; S.Init(g);
				V3 J[LM::Count]; FPose P; FNoise Nz;
				TArray<V3> PrevX, PrevT;
				double SumX = 0, SumT = 0, SumHem = 0; int Cnt = 0, CntHem = 0; std::vector<float> All;
				for (int f = 0; f < 480; f++)
				{
					Skeleton(P, J);
					if (Noisy) Nz.Apply(J, 1 / 60.f);
					S.Step(J, 1 / 60.f);
					if (f >= 240)
					{
						for (int i = 0; i < S.G->X.Num(); i++)
						{
							// what the viewer sees: motion across the line of sight
							const V3 Ray = Normalized(S.G->X[i] - V3(0.151f, 0.204f, 0.f));
							const float dx = ProjectOnPlane(S.G->X[i] - PrevX[i], Ray).Size() * 1000.f, dt = ProjectOnPlane(S.G->Target[i] - PrevT[i], Ray).Size() * 1000.f;
							SumX += dx; SumT += dt; Cnt++; All.push_back(dx);
							if (S.G->Data->Looseness[i] > 0.8f) { SumHem += dx; CntHem++; }
						}
					}
					PrevX = S.G->X; PrevT = S.G->Target;
				}
				std::sort(All.begin(), All.end());
				Res[Noisy][0] = float(SumX / Cnt); Res[Noisy][1] = All[size_t(All.size() * 0.95)]; Res[Noisy][2] = float(SumT / Cnt); Res[Noisy][3] = float(SumHem / FMath::Max(CntHem, 1));
			}
			printf("%-20s | %6.3f (%6.3f) | %6.3f  %6.3f (%6.3f)  %6.3f\n", *Cat[g].Name, Res[0][0], Res[0][1], Res[1][2], Res[1][0], Res[1][1], Res[1][3]);
		}
	}
	else if (Mode == "one")
	{
		// one garment, one pose, several views: harness one <out> <garment> <arms> <yawdeg> [noise]
		const int g = argc > 3 ? atoi(argv[3]) : 0, Arms = argc > 4 ? atoi(argv[4]) : 0;
		const float Yaw = argc > 5 ? FMath::DegreesToRadians(float(atof(argv[5]))) : 0.f;
		const bool bNoise = argc > 6 && atoi(argv[6]);
		const float Flat = argc > 7 ? float(atof(argv[7])) : 1.f;
		GStab = !(argc > 8 && atoi(argv[8]) == 0);
		FScene S; S.Init(g);
		V3 J[LM::Count]; FPose P; P.Arms = Arms; FNoise Nz;
		for (int f = 0; f < 180; f++)
		{
			FPose Q = P; Q.Arms = f < 30 ? 0 : P.Arms; Q.Yaw = Yaw * Clamp01((f - 30) / 45.f); Q.T = f / 60.f;
			Skeleton(Q, J); Flatten(J, Flat); if (bNoise) Nz.Apply(J, 1 / 60.f); S.Step(J, 1 / 60.f);
		}
		int Pa, Pt;
		S.Render(Webcam(), Out + "_front.ppm", &Pa, &Pt);
		S.Render(Orbit(40), Out + "_q.ppm"); S.Render(Orbit(90), Out + "_side.ppm"); S.Render(Orbit(180), Out + "_back.ppm");
		S.Render(Webcam(), Out + "_noproxy.ppm", nullptr, nullptr, false);
		if (getenv("ZOOM"))
		{
			float x, y, z, deg, dist; sscanf(getenv("ZOOM"), "%f,%f,%f,%f,%f", &x, &y, &z, &deg, &dist);
			const float a = FMath::DegreesToRadians(deg); const V3 At = V3(0.151f + x, -0.05f + y, -1.8f + z);
			S.Render(LookAt(At + V3(FMath::Sin(a), 0.1f, FMath::Cos(a)) * dist, At, 800, 800, 30.f), Out + "_zoom.ppm", nullptr, nullptr, getenv("NOPROXY") == nullptr);
		}
		printf("%s verts %d poke arm %d torso %d\n", *Cat[g].Name, S.G->VertexCount(), Pa, Pt);
	}
	else if (Mode == "settle")
	{
		// the user sidesteps back and forth for 2 s (10 cm at 1.2 Hz), then stands still
		printf("%-20s | moving: mean / max distance from the fitted shape (mm) | after stopping: ms until < 0.1 mm/frame, peak swing mm\n", "garment");
		for (int g = 0; g < Cat.Num(); g++)
		{
			FScene S; S.Init(g);
			V3 J[LM::Count]; FPose P;
			double SumD = 0; float MaxD = 0, Peak = 0; int Cnt = 0, SettleF = -1; TArray<V3> PrevX, StopX;
			for (int f = 0; f < 60 * 6; f++)
			{
				const float t = f / 60.f, Sw = t > 1 && t < 3 ? 0.1f * FMath::Sin((t - 1) * 2 * PI * 1.25f) : 0.f;
				Skeleton(P, J); for (int i = 0; i < LM::Count; i++) J[i].X += Sw;
				S.Step(J, 1 / 60.f);
				if (t > 1 && t < 3) for (int i = 0; i < S.G->X.Num(); i++) { const float d = (S.G->X[i] - S.G->Target[i]).Size() * 1000; SumD += d; MaxD = FMath::Max(MaxD, d); Cnt++; }
				if (f == 180) StopX = S.G->X;
				if (f > 180)
				{
					double Sp = 0; for (int i = 0; i < S.G->X.Num(); i++) { Sp += (S.G->X[i] - PrevX[i]).Size() * 1000; Peak = FMath::Max(Peak, (S.G->X[i] - StopX[i]).Size() * 1000.f); }
					Sp /= S.G->X.Num();
					if (Sp > 0.1) SettleF = f - 180;
				}
				PrevX = S.G->X;
			}
			printf("%-20s | %5.1f / %5.1f | %4.0f ms, %5.1f mm\n", *Cat[g].Name, SumD / Cnt, MaxD, (SettleF + 1) * 1000 / 60.f, Peak);
		}
	}
	else if (Mode == "tex")
	{
		// each fabric: a 256 px crop of its base colour at 1:1, and the same lit from the upper left with its normal map
		auto Enc = [](float v) { return uint8(255.f * FMath::Pow(Clamp01(v), 1 / 2.2f)); };
		const V3 L = Normalized(V3(-0.5f, 0.6f, 0.62f));
		for (int g = 0; g < Cat.Num(); g++)
		{
			constexpr int N = 512, W = 256;
			TArray<FLinearColor> Col, Nrm, Msk; BuildFabricPixels(Cat[g], N, Col, Nrm, Msk);
			FImage A(W, W), B(W, W), Cc(W, W);
			for (int y = 0; y < W; y++) for (int x = 0; x < W; x++)
			{
				const FLinearColor c = Col[y * N + x], n = Nrm[y * N + x], m = Msk[y * N + x];
				const size_t k = size_t(y) * W + x;
				A.Rgb[k * 3] = Enc(c.R); A.Rgb[k * 3 + 1] = Enc(c.G); A.Rgb[k * 3 + 2] = Enc(c.B);
				const V3 nn = Normalized(V3(n.R * 2 - 1, n.G * 2 - 1, n.B * 2 - 1));
				const float Sh = (0.35f + 0.75f * FMath::Max(0.f, Dot(nn, L))) * m.G;
				B.Rgb[k * 3] = Enc(c.R * Sh); B.Rgb[k * 3 + 1] = Enc(c.G * Sh); B.Rgb[k * 3 + 2] = Enc(c.B * Sh);
				// whole repeat tiled 2x2 and shrunk to a quarter: how it reads from a distance, and whether it tiles
				FLinearColor Acc(0, 0, 0, 0);
				for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) { const FLinearColor q = Col[((y * 4 + j) % N) * N + (x * 4 + i) % N]; const FLinearColor qn = Nrm[((y * 4 + j) % N) * N + (x * 4 + i) % N];
					const float s2 = 0.35f + 0.75f * FMath::Max(0.f, Dot(Normalized(V3(qn.R * 2 - 1, qn.G * 2 - 1, qn.B * 2 - 1)), L)); Acc = Acc + q * (s2 / 16.f); }
				Cc.Rgb[k * 3] = Enc(Acc.R); Cc.Rgb[k * 3 + 1] = Enc(Acc.G); Cc.Rgb[k * 3 + 2] = Enc(Acc.B);
			}
			A.Save(Out + "_" + std::to_string(g) + "_a.ppm"); B.Save(Out + "_" + std::to_string(g) + "_b.ppm"); Cc.Save(Out + "_" + std::to_string(g) + "_c.ppm");
		}
		constexpr int N = 512;
		TArray<FLinearColor> F; BuildFoldPixels(N, F);
		FImage A(N, N), B(N, N);
		for (int k = 0; k < N * N; k++)
		{
			const FLinearColor f = F[k];
			const V3 nn = Normalized(V3(f.R * 2 - 1, f.G * 2 - 1, FMath::Sqrt(FMath::Max(1.f - FMath::Square(f.R * 2 - 1) - FMath::Square(f.G * 2 - 1), 0.f))));
			const float Sh = 0.3f + 0.8f * FMath::Max(0.f, Dot(nn, L));
			const float Dye = 0.86f + 0.28f * f.A;
			for (int ch = 0; ch < 3; ch++) { A.Rgb[size_t(k) * 3 + ch] = Enc(0.6f * Sh * Dye); B.Rgb[size_t(k) * 3 + ch] = Enc(f.A); }
		}
		A.Save(Out + "_fold_lit.ppm"); B.Save(Out + "_fold_dye.ppm");
	}
	else if (Mode == "topo")
	{
		// openings of each rest mesh: there should be a neck, a hem and two cuffs (or armholes)
		for (int g = 0; g < Cat.Num(); g++)
		{
			FBodyRig Rig; V3 J[LM::Count]; float Vis[LM::Count]; for (float& v : Vis) v = 1; FPose P; Skeleton(P, J); Rig.Update(J, Vis, 1 / 60.f, true);
			const FRestSkeleton Sk = FRestSkeleton::From(Rig);
			TSharedPtr<FGarmentMeshData> D = GarmentMesher::Build(Sk, Cat[g]);
			std::unordered_map<int64, int> Ec; std::unordered_map<int, std::vector<int>> Nb;
			auto Key = [](int a, int b) { return a < b ? (int64(a) << 32) | uint32(b) : (int64(b) << 32) | uint32(a); };
			for (int t = 0; t < D->Tris.Num(); t += 3) for (int e = 0; e < 3; e++) Ec[Key(D->Tris[t + e], D->Tris[t + (e + 1) % 3])]++;
			int NonManifold = 0;
			for (auto& kv : Ec) { if (kv.second == 1) { int a = int(kv.first >> 32), b = int(kv.first & 0xffffffff); Nb[a].push_back(b); Nb[b].push_back(a); } if (kv.second > 2) NonManifold++; }
			std::unordered_map<int, bool> Seen; printf("%-20s verts %d nonmanifold %d loops:", *Cat[g].Name, D->Rest.Num(), NonManifold);
			for (auto& kv : Nb)
			{
				if (Seen[kv.first]) continue;
				std::vector<int> St{ kv.first }; Seen[kv.first] = true; V3 Sum = V3::ZeroVector; int n = 0;
				while (!St.empty()) { int v = St.back(); St.pop_back(); Sum += D->Rest[v]; n++; for (int w : Nb[v]) if (!Seen[w]) { Seen[w] = true; St.push_back(w); } }
				Sum /= float(n); printf("  [%d @ %.2f %.2f %.2f]", n, Sum.X, Sum.Y, Sum.Z);
			}
			printf("\n");
		}
	}
	else if (Mode == "turn")
	{
		// the user turns from facing the camera to side-on and back; the tracker flattens depth
		const float Flat = argc > 3 ? float(atof(argv[3])) : 0.5f;
		for (int Stab = 0; Stab < 2; Stab++)
		{
			GStab = Stab == 1;
			FScene S; S.Init(0);
			V3 J[LM::Count]; FNoise Nz;
			printf("stabilise %d (tracker depth x%.2f)\n  true yaw | rig yaw | shoulder width | unit\n", Stab, Flat);
			for (int f = 0; f < 60 * 14; f++)
			{
				const float t = f / 60.f;
				FPose P; P.Yaw = FMath::DegreesToRadians(t < 2 ? 0 : t < 6 ? (t - 2) * 22.5f : t < 8 ? 90 : t < 12 ? 90 - (t - 8) * 45 : -90);
				Skeleton(P, J); Flatten(J, Flat); Nz.Apply(J, 1 / 60.f); S.Step(J, 1 / 60.f);
				if (f % 30 == 29) printf("  %7.1f | %7.1f | %.3f | %.2f | rebuilds %d\n", FMath::RadiansToDegrees(P.Yaw), S.Rig.YawDeg, S.Rig.ShoulderW, S.Rig.Unit(), S.G->RebuildCount);
				if (f == 60 * 5 || f == 60 * 7) S.Render(Webcam(), Out + "_stab" + std::to_string(Stab) + "_" + std::to_string(f / 60) + ".ppm");
			}
		}
	}
	return 0;
}
