#include "GarmentSpec.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

namespace ReflectAR
{
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
		BuildFabricPixels(S, N, Col, Nrm, Msk);
		FFabricMaps M;
		M.BaseColor.Reset(MakeTexture(Col, N, true, *(S.Name + TEXT("_BaseColor"))));
		M.Normal.Reset(MakeTexture(Nrm, N, false, *(S.Name + TEXT("_Normal"))));
		M.Mask.Reset(MakeTexture(Msk, N, false, *(S.Name + TEXT("_Mask"))));
		return Cache.Add(S.Name, MoveTemp(M));
	}

	UTexture2D* FFabricMaps::Fold()
	{
		static TStrongObjectPtr<UTexture2D>& Tex = *new TStrongObjectPtr<UTexture2D>();     // never destroyed, like the cache above
		if (!Tex.IsValid())
		{
			constexpr int N = 512;
			TArray<FLinearColor> Px;
			BuildFoldPixels(N, Px);
			Tex.Reset(MakeTexture(Px, N, false, TEXT("ReflectAR_Fold")));
		}
		return Tex.Get();
	}
}
