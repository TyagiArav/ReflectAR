#pragma once

#include "BodyMath.h"

namespace ReflectAR
{
	/**
	 * Smoothed skeleton plus derived body frames and measurements, in body coordinates of the
	 * real (unreflected) body. Garments are built from this; reflection is a parent transform.
	 */
	class FBodyRig
	{
	public:
		V3 J[LM::Count];
		float Vis[LM::Count] = {};
		bool Valid = false;

		V3 HipC, ShC, NeckBase, HeadC;
		V3 Right = VRight, Up = VUp, Fwd = VFwd;     // chest frame (Right = user's right)
		V3 PRight = VRight, PFwd = VFwd;             // pelvis frame (shares Up)
		float ShoulderW = 0.36f, HipW = 0.22f, TorsoLen = 0.5f, HeadR = 0.1f;
		float ArmLenL = 0.55f, ArmLenR = 0.55f;
		float UpperL = 0.29f, UpperR = 0.29f, ForeL = 0.26f, ForeR = 0.26f;

		/** Measured torso half-width relative to the generic model at FBodyShape::Levels (1 = average). */
		float Profile[6] = { 1, 1, 1, 1, 1, 1 };

		/** 1/s; 0 = follow the target exactly (the tracking client already filters and predicts). */
		float Smoothing = 0.f;

		/** Body-relative unit: 1 for a 0.36 m shoulder-joint span. */
		float Unit() const { return ShoulderW / 0.36f; }
		FFrame ChestFrame() const { return FFrame::Look(ShC, Fwd, Up); }
		FFrame PelvisFrame() const { return FFrame::Look(HipC, PFwd, Up); }

		FBodyRig();
		void Update(const V3* Target, const float* InVis, float Dt, bool bSnap);
		void ResetShape() { for (float& p : Profile) p = 1.f; }

	private:
		bool bMeasured = false;
		void Derive(bool bResetMeasures);
		float Seg(int a, int b) const { return FVector3f::Distance(J[a], J[b]); }
	};

	/**
	 * Measures the user's torso from perception's person mask, so garments are cut to their
	 * actual build. At a few heights between the hips and the shoulders it walks the mask outward
	 * from the torso's centre line until it leaves the person; a side is skipped while the arm
	 * touches that edge, and the whole sample unless the user faces the camera. The ratio to the
	 * generic model is averaged over ~1.5 s into FBodyRig::Profile.
	 */
	class FBodyShape
	{
	public:
		static constexpr float Levels[6] = { -0.05f, 0.12f, 0.28f, 0.44f, 0.58f, 0.72f };
		float TimeConstant = 1.5f;
		float MaxYawDeg = 25.f;
		int Samples = 0;

		void Reset(FBodyRig& B);
		static float ProfileAt(const float* Profile, float t);

		/**
		 * @param Mask       person mask, row 0 = image top, 0..255
		 * @param BodyToImage rows of body coords -> (u, v, 1) * depth, (u, v) in [0, 1] of the image, v down
		 * @param InJ        the pose when the mask was captured
		 * @param CamPos     camera position (body coords)
		 */
		void Measure(const TArray<uint8>& Mask, int MW, int MH, const FVector4f BodyToImage[3], const V3* InJ,
			FBodyRig& B, const V3& CamPos, float Dt);

	private:
		int LevelSamples[6] = {};
	};

	/** Project body point p with three affine rows -> (u, v); false if behind the camera. */
	inline bool ProjectRows(const FVector4f M[3], const V3& p, V2& uv)
	{
		const float qx = M[0].X * p.X + M[0].Y * p.Y + M[0].Z * p.Z + M[0].W;
		const float qy = M[1].X * p.X + M[1].Y * p.Y + M[1].Z * p.Z + M[1].W;
		const float qz = M[2].X * p.X + M[2].Y * p.Y + M[2].Z * p.Z + M[2].W;
		if (qz <= 1e-3f) { uv = V2::ZeroVector; return false; }
		uv = V2(qx / qz, qy / qz);
		return true;
	}
}
