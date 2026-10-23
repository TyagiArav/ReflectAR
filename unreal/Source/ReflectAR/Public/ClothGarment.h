#pragma once

#include "GarmentTrims.h"
#include "Async/Future.h"

namespace ReflectAR
{
	/**
	 * A garment simulated as cloth. The seamless mesh from GarmentMesher is skinned to six bones
	 * driven by the tracked skeleton; that skinned shape is the "body-fitted" target. An XPBD
	 * solver then simulates the fabric: inextensible stretch, soft compression (so it buckles into
	 * wrinkles), bending, gravity, inertia, damping and collisions against capsules fitted to the
	 * user's body. Each vertex is tethered to its skinned target by a max distance that is ~0 at
	 * the collar and shoulders and grows toward the hem and cuffs, which is how real garments hang.
	 *
	 * The solver runs on the welded mesh. What is drawn (BodySection) is a copy split along the UV
	 * seams, with normals from the welded mesh (no shading seams) and tangents for the normal map,
	 * plus the front closure (Trims) riding on it. Pure C++: AReflectARMirror uploads the sections.
	 */
	class FClothGarment
	{
	public:
		FClothGarment(const FGarmentSpec& InSpec, const FBodyRig& InBody) : Spec(InSpec), Body(InBody) {}

		const FGarmentSpec& Spec;
		int Substeps = 2;                 // the cloth inherits body motion (Follow), so 2 is enough
		float Follow = 0.9f;              // share of body motion the cloth inherits directly
		float Alpha = 1.f;

		float LastBuildMs = 0, LastStepMs = 0;
		int RebuildCount = 0;
		int VertexCount() const { return X.Num(); }
		bool HasMesh() const { return bHasMesh; }

		FMeshSection BodySection;
		TSharedPtr<FGarmentTrims> Trims;

		void ResetCloth() { bSimInit = false; }
		/** Start (re)generating this garment for the current body if needed. Cheap; call every frame. */
		void Prepare(double Now);
		/** Simulates one frame and refreshes BodySection / Trims. */
		void Step(float Dt, double Now);
		/** True once after a new mesh was swapped in: its sections must be re-created. */
		bool ConsumeTopologyChanged() { const bool b = bTopologyChanged; bTopologyChanged = false; return b; }

		/** How far the cloth may stand off the body (ease plus drape), with a little to spare. */
		float Room() const { return ((0.03f + Spec.Ease) * Spec.Drape + 0.01f) * Body.Unit(); }
		/** Where the garment covers the torso: its torso and shoulder colliders grown by Room(). */
		void Outline(TArray<FCapsuleB>& Into) const;

	private:
		struct FBuilt
		{
			FRestSkeleton Skel;
			TSharedPtr<FGarmentMeshData> Data;
			TArray<float> MaxDist, SkinK, ERest, BRest;
			TArray<int32> EA, EB, BA, BB;
			float Ms = 0;
		};
		static TSharedPtr<FBuilt> Generate(FRestSkeleton Skel, const FGarmentSpec* Spec);
		void ApplyPending(double Now);
		bool Drifted() const;
		void SkinTargets();
		void BuildColliders();
		void UpdateRenderMesh();
		void Tangents();

		const FBodyRig& Body;
		TSharedPtr<FGarmentMeshData> Data;
		FRestSkeleton Built;
		bool bHasMesh = false, bSimInit = false, bTopologyChanged = false;
		TFuture<TSharedPtr<FBuilt>> Pending;
		double DriftSince = -1, LastApplied = -100, FirstApplied = -100;

		// simulation state
		TArray<V3> X, Prev, Target, LastTarget;
		TArray<float> MaxDist, SkinK, ERest, BRest;
		TArray<int32> EA, EB, BA, BB;
		FFrame RestFrames[6];
		FAffine SkinM[6];
		TArray<FCapsuleB> Colliders;
		TArray<V3> ColCenter;
		TArray<float> ColBound;

		// render mesh (split at UV seams)
		TArray<V3> SimN, TAcc, BAcc;
	};
}
