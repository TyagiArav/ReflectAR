#include "GarmentSpec.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace ReflectAR
{
	static FLinearColor C(const TCHAR* Hex) { return FLinearColor(FColor::FromHex(Hex)); }

	const TArray<FGarmentSpec>& GarmentCatalog()
	{
		static TArray<FGarmentSpec> All = []
		{
			TArray<FGarmentSpec> A;
			auto Add = [&A](TFunctionRef<void(FGarmentSpec&)> Init) { FGarmentSpec S; Init(S); A.Add(S); };
			Add([](FGarmentSpec& g) { g.Name = TEXT("Classic Tee"); g.ColorA = C(TEXT("EDEBE6")); g.Pattern = EPattern::Knit; g.Sleeve = 0.34f;
				g.Hem = -0.12f; g.Ease = 0.018f; g.CuffFlare = 0.006f; g.Fit = 0.75f; g.Smooth = 0.2f; });
			Add([](FGarmentSpec& g) { g.Name = TEXT("V-Neck Tee"); g.ColorA = C(TEXT("2B3A55")); g.Pattern = EPattern::Knit; g.Sleeve = 0.32f;
				g.Hem = -0.12f; g.Ease = 0.014f; g.Neck = ENeckline::VNeck; g.Fit = 0.6f; g.Smooth = 0.2f; });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Breton Long-Sleeve"); g.ColorA = C(TEXT("F4F1E8")); g.ColorB = C(TEXT("1F2D5A"));
				g.Pattern = EPattern::Stripes; g.Tile = 0.07f; g.Sleeve = 0.97f; g.Hem = -0.12f; g.Ease = 0.012f; g.Fit = 0.45f; g.Smooth = 0.24f; g.Bump = 0.5f; });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Polo Shirt"); g.ColorA = C(TEXT("1D6B4E")); g.Pattern = EPattern::Pique; g.Sleeve = 0.3f;
				g.Hem = -0.14f; g.Ease = 0.016f; g.Neck = ENeckline::Collar; g.Bend = 1.2e-2f; g.Placket = 1; g.PlacketLen = 0.28f;
				g.Fit = 0.7f; g.Smooth = 0.26f; g.Trim = C(TEXT("E9E5DA")); });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Flannel Shirt"); g.ColorA = C(TEXT("A3262A")); g.ColorB = C(TEXT("1B1B1B")); g.Pattern = EPattern::Plaid;
				g.Tile = 0.16f; g.Sleeve = 0.97f; g.Hem = -0.2f; g.Ease = 0.026f; g.Neck = ENeckline::Collar; g.Bend = 1e-2f; g.Drape = 1.2f; g.Placket = 1;
				g.Fit = 0.85f; g.Smooth = 0.16f; g.Bump = 0.8f; g.Trim = C(TEXT("EDE6D6")); });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Pullover Hoodie"); g.ColorA = C(TEXT("8B8F96")); g.Pattern = EPattern::Knit; g.Sleeve = 0.98f;
				g.Hem = -0.16f; g.Ease = 0.035f; g.SleeveEase = 0.016f; g.Neck = ENeckline::HoodDown;
				g.Bend = 6e-3f; g.Compress = 8e-4f; g.Density = 1.4f; g.Drape = 1.3f;
				g.Fit = 0.95f; g.HemBand = 1.f; g.bCuffBand = true; g.Smooth = 0.1f; g.Bump = 0.7f; });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Bomber Jacket"); g.ColorA = C(TEXT("4B5B3A")); g.Pattern = EPattern::Solid; g.Sleeve = 0.98f;
				g.Hem = -0.03f; g.Ease = 0.04f; g.SleeveEase = 0.02f; g.Neck = ENeckline::Collar;
				g.Bend = 4e-3f; g.Compress = 3e-4f; g.Density = 1.5f; g.Sheen = 0.45f; g.Placket = 2;
				g.Fit = 0.8f; g.HemBand = 1.f; g.bCuffBand = true; g.Smooth = 0.5f; g.Bump = 0.3f; g.Trim = C(TEXT("2A3024")); g.Metal = C(TEXT("8E9096")); });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Denim Jacket"); g.ColorA = C(TEXT("41628C")); g.Pattern = EPattern::Denim; g.Sleeve = 0.98f;
				g.Hem = -0.04f; g.Ease = 0.035f; g.SleeveEase = 0.018f; g.Neck = ENeckline::Collar;
				g.Bend = 1.5e-3f; g.Compress = 1e-4f; g.Density = 1.8f; g.Drape = 0.7f; g.Sheen = 0.1f; g.Placket = 1;
				g.Fit = 0.7f; g.Smooth = 0.2f; g.Bump = 1.2f; g.bMetalButtons = true; g.Metal = C(TEXT("B08A57")); });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Puffer Jacket"); g.ColorA = C(TEXT("1E1E24")); g.Pattern = EPattern::Quilt; g.Tile = 0.12f; g.Sleeve = 0.98f;
				g.Hem = -0.06f; g.Ease = 0.038f; g.SleeveEase = 0.022f; g.Quilt = 0.009f; g.Neck = ENeckline::Collar;
				g.Bend = 8e-4f; g.Compress = 5e-5f; g.Density = 1.2f; g.Drape = 0.5f; g.Sheen = 0.6f; g.Placket = 2;
				g.Fit = 0.9f; g.HemBand = 0.6f; g.bCuffBand = true; g.Smooth = 0.58f; g.Bump = 0.4f; g.Trim = C(TEXT("121216")); g.Metal = C(TEXT("5E6066")); });
			Add([](FGarmentSpec& g) { g.Name = TEXT("Ribbed Tank"); g.ColorA = C(TEXT("E0533D")); g.Pattern = EPattern::Rib; g.Tile = 0.12f; g.Sleeve = 0.f;
				g.Hem = -0.1f; g.Ease = 0.006f; g.bTank = true; g.Neck = ENeckline::Scoop; g.Fit = 0.15f; g.Smooth = 0.24f; });
			return A;
		}();
		return All;
	}

	static float Rep(float x) { return x - FMath::FloorToFloat(x); }
	static float Perlin(float x, float y) { return FMath::PerlinNoise2D(FVector2D(x, y)) * 0.5f + 0.5f; }   // ~[0, 1] like Unity's

	void SamplePattern(const FGarmentSpec& S, float u, float v, FLinearColor& c, float& h)
	{
		const int x = FMath::FloorToInt(u * 256.f), y = FMath::FloorToInt(v * 256.f);
		c = S.ColorA;
		h = 0.5f;
		switch (S.Pattern)
		{
		case EPattern::Stripes:
			c = v < 0.5f ? S.ColorA : S.ColorB;
			h = 0.5f + 0.5f * FMath::Sin(u * PI * 2.f * 96.f) * FMath::Sin(v * PI * 2.f * 64.f);   // fine jersey
			break;
		case EPattern::Plaid:
		{
			const float bu = Rep(u) < 0.38f ? 1.f : 0.f, bv = Rep(v) < 0.38f ? 1.f : 0.f;
			const float Thin = (FMath::Abs(u - 0.7f) < 0.025f || FMath::Abs(v - 0.7f) < 0.025f) ? 1.f : 0.f;
			c = FMath::Lerp(S.ColorA, S.ColorB, 0.5f * (bu + bv));
			c = FMath::Lerp(c, FLinearColor(0.9f, 0.85f, 0.8f), Thin * 0.5f);
			h = ((x + y) % 4 < 2) ? 0.65f : 0.35f;          // twill
			break;
		}
		case EPattern::Quilt:
		{
			const float d = FMath::Abs(v - 0.5f) * 2.f;       // horizontal baffle seams
			c = FMath::Lerp(S.ColorA, S.ColorA * 0.45f, FMath::Pow(1.f - d, 12.f));
			h = FMath::Sqrt(1.f - (1.f - d) * (1.f - d));
			break;
		}
		case EPattern::Denim:
		{
			// 3/1 twill: diagonal wales of white weft over indigo warp, slight slub
			const float w = Rep((x + y) / 5.f);
			const float Slub = Perlin(u * 6.f, v * 90.f) * 0.25f + Perlin(u * 60.f, v * 60.f) * 0.1f;
			c = FMath::Lerp(S.ColorA * 0.8f, FMath::Lerp(S.ColorA, FLinearColor::White, 0.35f), (w < 0.35f ? 0.55f : 0.1f) + Slub);
			h = w < 0.35f ? 0.75f : 0.35f;
			break;
		}
		case EPattern::Knit:
		{
			const float cu = Rep(u * 64.f), cv = Rep(v * 96.f + (cu < 0.5f ? cu : 1.f - cu));
			h = 0.5f + 0.5f * FMath::Sin(cv * PI * 2.f) * FMath::Sin(cu * PI);
			c = S.ColorA * (0.92f + 0.08f * h);
			break;
		}
		case EPattern::Pique:
		{
			const float cu = Rep(u * 48.f) - 0.5f, cv = Rep(v * 48.f) - 0.5f;
			h = 1.f - Clamp01((cu * cu + cv * cv) * 5.f);
			c = S.ColorA * (0.88f + 0.12f * h);
			break;
		}
		case EPattern::Rib:
			h = 0.5f + 0.5f * FMath::Sin(u * PI * 2.f * 16.f);
			c = S.ColorA * (0.9f + 0.1f * h);
			break;
		default:                                              // plain weave: faint grain
			h = 0.5f + 0.25f * FMath::Sin(u * PI * 2.f * 128.f) * FMath::Sin(v * PI * 2.f * 128.f);
			break;
		}
		const float Noise = Perlin(u * 120.f, v * 120.f) * 0.06f - 0.03f;
		c = FLinearColor(c.R + Noise, c.G + Noise, c.B + Noise, 1.f);
	}

	/** Repeat-wrapped 8-bit BGRA texture with a full box-filtered mip chain. */
	static UTexture2D* MakeTexture(const TArray<FLinearColor>& Px, int N, bool bSRGB, const TCHAR* Name)
	{
		UTexture2D* T = UTexture2D::CreateTransient(N, N, PF_B8G8R8A8, FName(Name));
		T->SRGB = bSRGB;
		T->AddressX = T->AddressY = TA_Wrap;
		T->Filter = TF_Trilinear;
		T->NeverStream = true;
		FTexturePlatformData* PD = T->GetPlatformData();

		TArray<FLinearColor> Level = Px;
		int Size = N;
		for (int Mip = 0; Size >= 1; Mip++)
		{
			FTexture2DMipMap* M;
			if (Mip == 0) M = &PD->Mips[0];
			else { M = new FTexture2DMipMap(Size, Size, 1); PD->Mips.Add(M); }
			M->SizeX = M->SizeY = Size;
			M->BulkData.Lock(LOCK_READ_WRITE);
			FColor* Dst = static_cast<FColor*>(M->BulkData.Realloc(int64(Size) * Size * sizeof(FColor)));
			for (int i = 0; i < Size * Size; i++) Dst[i] = Level[i].ToFColor(bSRGB);     // FColor is BGRA in memory
			M->BulkData.Unlock();
			if (Size == 1) break;
			const int H = Size / 2;
			TArray<FLinearColor> Next;
			Next.SetNumUninitialized(H * H);
			for (int y = 0; y < H; y++)
				for (int x = 0; x < H; x++)
					Next[y * H + x] = (Level[(2 * y) * Size + 2 * x] + Level[(2 * y) * Size + 2 * x + 1]
						+ Level[(2 * y + 1) * Size + 2 * x] + Level[(2 * y + 1) * Size + 2 * x + 1]) * 0.25f;
			Level = MoveTemp(Next);
			Size = H;
		}
		T->UpdateResource();
		return T;
	}

	const FFabricMaps& FFabricMaps::Get(const FGarmentSpec& S)
	{
		// never destroyed: a static's destructor would release the textures after UObjects are gone
		static TMap<FString, FFabricMaps>& Cache = *new TMap<FString, FFabricMaps>();
		if (FFabricMaps* Found = Cache.Find(S.Name)) return *Found;
		constexpr int N = 512;
		TArray<FLinearColor> Col, Nrm, Msk;
		TArray<float> H;
		Col.SetNumUninitialized(N * N); Nrm.SetNumUninitialized(N * N); Msk.SetNumUninitialized(N * N); H.SetNumUninitialized(N * N);
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++)
				SamplePattern(S, x / float(N), y / float(N), Col[y * N + x], H[y * N + x]);
		// slope per texel of height: weaves are ~1 mm high over a few texels
		const float k = 2.5f * S.Bump;
		auto Ht = [&](int i, int j) { return H[((j + N) % N) * N + (i + N) % N]; };
		for (int y = 0; y < N; y++)
			for (int x = 0; x < N; x++)
			{
				const float dx = (Ht(x + 1, y) - Ht(x - 1, y)) * 0.5f * k, dy = (Ht(x, y + 1) - Ht(x, y - 1)) * 0.5f * k;
				const V3 n = Normalized(V3(-dx, -dy, 1.f));
				Nrm[y * N + x] = FLinearColor(n.X * 0.5f + 0.5f, n.Y * 0.5f + 0.5f, n.Z * 0.5f + 0.5f, 1.f);
				const float hv = Ht(x, y);
				Msk[y * N + x] = FLinearColor(0.f, FMath::Lerp(0.72f, 1.f, hv), 0.f, Clamp01(S.Smooth * FMath::Lerp(0.8f, 1.15f, hv)));
			}
		// colours are linear here (FLinearColor from the sRGB hex codes); the texture stores them as sRGB
		for (FLinearColor& c : Col) c = FLinearColor(FMath::Clamp(c.R, 0.f, 1.f), FMath::Clamp(c.G, 0.f, 1.f), FMath::Clamp(c.B, 0.f, 1.f), 1.f);
		FFabricMaps M;
		M.BaseColor.Reset(MakeTexture(Col, N, true, *(S.Name + TEXT("_BaseColor"))));
		M.Normal.Reset(MakeTexture(Nrm, N, false, *(S.Name + TEXT("_Normal"))));
		M.Mask.Reset(MakeTexture(Msk, N, false, *(S.Name + TEXT("_Mask"))));
		return Cache.Add(S.Name, MoveTemp(M));
	}
}
