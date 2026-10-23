#pragma once

#include "GarmentMesher.h"

namespace ReflectAR
{
	/** A mesh section in body coordinates, ready for UProceduralMeshComponent. */
	struct FMeshSection
	{
		TArray<V3> Pos, Nrm;
		TArray<V4> Tan;               // xyz tangent, w handedness
		TArray<V2> UV;
		TArray<int32> Tris;
	};

	/**
	 * Front closures as real geometry riding on the simulated cloth: a raised button placket with
	 * sewn buttons (or metal shanks), or a zipper with tape, teeth and slider.
	 *
	 * They hang off a chain of points down the centre front of the rest mesh, each found by a ray
	 * and stored as a triangle + barycentrics. Every frame each chain point follows its triangle on
	 * the cloth and carries a frame (right, up, normal), and every trim vertex sits at a fixed
	 * offset in the frame of its place along the chain, so the closure bends with the fabric.
	 * Sections: 0 fabric (placket band), 1 plastic (buttons / zipper tape), 2 metal.
	 */
	class FGarmentTrims
	{
	public:
		static constexpr int Fabric = 0, Plastic = 1, Metal = 2, NumSections = 3;

		/** Builds the closure, or returns null if the garment has none (or the centre front isn't found). */
		static TSharedPtr<FGarmentTrims> Build(const FGarmentMeshData& D, const FRestSkeleton& S, const FGarmentSpec& G);

		/** Moves the closure with the cloth. X: simulated positions, N: their normals. */
		void Update(const TArray<V3>& X, const TArray<V3>& N);

		FMeshSection Sections[NumSections];

	private:
		struct FVert { int Section, Index, Chain; float F; V3 Local, LocalN; };
		TArray<int32> Ta, Tb, Tc;
		TArray<float> Wa, Wb, Wc, Arc;
		TArray<V3> Cp, Cr, Cu, Cn;
		TArray<FVert> Verts;
		friend class FTrimBuilder;
	};
}
