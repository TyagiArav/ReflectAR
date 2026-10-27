#pragma once

#include "BodyRig.h"
#include "GarmentTrims.h"

namespace ReflectAR
{
	class FClothGarment;

	/**
	 * Stand-ins for the user's body that hide whatever of the garment is behind it, built from the
	 * skeleton as one mesh (body coordinates) for M_Proxy, which draws them as the camera feed
	 * (CameraFeed mode) or black (Mirror), so they read as the real body.
	 *
	 * Parts under the garment (torso, neck, sleeved arm) are fitted capsules. Exposed parts (bare
	 * forearms, hands with fingers, head with hair) are drawn twice: a fitted core, and with a
	 * person mask an oversized shell that the material trims to that mask, so their outline follows
	 * the real silhouette. The shell is pushed back by however much it was enlarged, so its front
	 * stays at the real body surface. Over the garment the mask can't tell a hand from the torso
	 * behind it, so the material also drops shell fragments over the garment's outline (Cover).
	 *
	 * Per-vertex data for the material: UV1 = (motion since the mask, x y), UV2 = (motion z, depth
	 * push m), UV3 = (trim to mask 0/1, 0).
	 */
	class FBodyProxy
	{
	public:
		static constexpr int MaxCover = 12;

		FMeshSection Mesh;
		TArray<V2> UV1, UV2, UV3;
		TArray<FCapsuleB> Cover;                 // garment outline (body coords), for the material
		bool bVisible = false;

		FBodyProxy();
		/**
		 * @param Sleeve      fraction of the arm the current garment covers
		 * @param MaskJoints  the pose when the person mask was captured, or null without a mask
		 * @param Garment     the garment worn, whose outline the oversized shells stay off
		 */
		void Update(const FBodyRig& B, bool bOn, float Sleeve, const V3* MaskJoints, const FClothGarment* Garment);

	private:
		struct FPart
		{
			bool bSphere = false;
			FFrame F;                             // position + orientation
			V3 Scale = V3::ZeroVector;            // zero = hidden
			bool bTrim = false;
			float Push = 0;
			V3 Offset = V3::ZeroVector;
		};
		enum
		{
			Torso = 0, Neck = 1, Legs = 2,        // 4 leg capsules
			Covered = 6,                           // 2 per arm: sleeved segments
			Exposed = 10,                          // 2 per arm: bare segments
			Hands = 14, Thumbs = 16, Head = 18, Count = 19,
			Cores = Count, Total = Count + Count - Exposed,   // fitted copies of Exposed..Head
		};
		static int Core(int i) { return Cores + i - Exposed; }

		FPart Parts[Total];
		TArray<V3> CapsuleV, CapsuleN, SphereV, SphereN;
		TArray<int32> CapsuleT, SphereT;
		int VertsPerCapsule = 0, VertsPerSphere = 0;

		static void Place(FPart& P, const V3& a, const V3& b, float rx, float rz, const V3& FwdHint);
		void PlaceExposed(int i, const V3& a, const V3& b, float rx, float rz, const V3& FwdHint, float m, bool bMasked);
		void HideExposed(int i) { Parts[i].Scale = Parts[Core(i)].Scale = V3::ZeroVector; }
		void Props(int i, bool bTrim, float Push, const V3& Offset) { Parts[i].bTrim = bTrim; Parts[i].Push = Push; Parts[i].Offset = Offset; }
		void Emit();
	};
}
