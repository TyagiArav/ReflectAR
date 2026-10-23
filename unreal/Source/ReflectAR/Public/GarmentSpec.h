#pragma once

#include "BodyMath.h"
#include "UObject/StrongObjectPtr.h"

class UTexture2D;

namespace ReflectAR
{
	enum class EPattern : uint8 { Solid, Stripes, Plaid, Quilt, Denim, Knit, Pique, Rib };
	enum class ENeckline : uint8 { Crew, VNeck, Collar, HoodDown, Scoop };

	/**
	 * One upper-body garment: its cut (used to generate a seamless mesh), its fabric (used by the
	 * XPBD cloth solver) and its look. Lengths are in metres for a 0.36 m shoulder-joint span and
	 * scale with the user's body.
	 */
	struct FGarmentSpec
	{
		FString Name = TEXT("Garment");
		FLinearColor ColorA = FLinearColor::White, ColorB = FLinearColor::Gray;   // sRGB-authored colours
		EPattern Pattern = EPattern::Solid;
		float Tile = 0.25f;               // metres per pattern repeat

		// ---- cut
		float Hem = -0.08f;               // hem height, fraction of torso length (0 = hip joints)
		float Ease = 0.015f;              // looseness around the torso
		float Sleeve = 0.3f;              // fraction of the arm covered (0 = sleeveless)
		float SleeveEase = 0.008f;
		ENeckline Neck = ENeckline::Crew;
		bool bTank = false;               // narrow straps instead of shoulders
		float Quilt = 0.f;                // puffer baffle bulge height (m)
		float CuffFlare = 0.f;            // extra radius at the sleeve opening
		int Placket = 0;                  // front closure: 0 none, 1 buttons, 2 zipper
		float PlacketLen = 1.f;           // fraction of the front it runs down (polo = short)

		// ---- fabric (XPBD)
		float Stretch = 1e-6f;            // stretch compliance (lower = less stretchy)
		float Compress = 2e-4f;           // compression compliance (higher = buckles easily)
		float Bend = 2e-2f;               // bending compliance (higher = softer, drapier)
		float Damping = 0.04f;
		float Drape = 1.f;                // how far loose regions may hang from the body
		float Sheen = 0.25f;              // cloth shading model: fuzz / sheen amount
		float Density = 1.f;              // relative weight (gravity sag)

		// ---- fit: how the cut follows the measured body (GarmentMesher)
		float Fit = 0.7f;                 // below the chest: 0 = follows the waist, 1 = hangs straight
		float HemBand = 0.f;              // ribbed hem band pulled in to the hips (0..1)
		bool bCuffBand = false;           // gathered cuffs at the wrist (long sleeves)

		// ---- material and trims
		float Smooth = 0.22f;             // perceptual smoothness of the fibres (roughness = 1 - this)
		float Bump = 1.f;                 // weave normal-map strength
		FLinearColor Trim = FLinearColor(0.92f, 0.9f, 0.85f);    // buttons, or the zipper tape
		FLinearColor Metal = FLinearColor(0.74f, 0.74f, 0.76f);  // zipper teeth / slider, metal buttons
		bool bMetalButtons = false;
	};

	/** The 10 upper-body garments in the catalogue. */
	const TArray<FGarmentSpec>& GarmentCatalog();

	/** Colour and weave height (0..1) at (u, v) in [0, 1) of one repeat of the pattern. */
	void SamplePattern(const FGarmentSpec& S, float u, float v, FLinearColor& OutColor, float& OutHeight);

	/**
	 * Material maps for one repeat of a fabric: base colour (sRGB), tangent-space normal map from
	 * the weave height (linear), and a mask (linear: G = ambient occlusion in the weave's valleys,
	 * A = smoothness). Game thread only.
	 */
	struct FFabricMaps
	{
		TStrongObjectPtr<UTexture2D> BaseColor, Normal, Mask;
		static const FFabricMaps& Get(const FGarmentSpec& S);
	};
}
