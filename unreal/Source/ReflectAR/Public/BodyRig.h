#pragma once

#include "BodyMath.h"

namespace ReflectAR
{
	/**
	 * Stabilised skeleton plus derived body frames and measurements, in body coordinates of the
	 * real (unreflected) body. Garments are built from this; reflection is a parent transform.
	 *
	 * A single camera sees where a joint is in the image far better than how far away it is: the
	 * depth of each joint relative to the torso is several times noisier than its image position,
	 * and the pose model flattens the body when it is turned. Left alone, that noise swings
	 * everything built on the skeleton (a 1 cm depth error across the shoulders is 3 degrees of
	 * yaw, 3 cm at the hem), and a turned user looks half as wide and still facing the camera.
	 * So before anything is derived, Update():
	 *   - filters each joint's depth relative to the torso (One-Euro: heavy at rest, light when
	 *     the joint really moves in depth), leaving its image position untouched;
	 *   - sets the depth difference between the two shoulders (and hips) from foreshortening once
	 *     the torso is clearly turned: they are a known distance apart, so the narrower they look,
	 *     the further one is behind the other. The pose model only supplies which one.
	 * Depth changes move a joint along its line of sight, so it stays on the person in the image.
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

		/** Where depth is measured from: the camera's optical centre (body coords). */
		V3 CamPos = V3(0.151f, 0.204f, 0.f);
		/** Depth stabilisation (above). Off = use the tracked joints as they come. */
		bool bStabilise = true;
		float DepthMinCutoff = 0.35f;     // Hz at rest
		float DepthBeta = 4.f;            // Hz per m/s of depth speed
		/** How far the chest is turned away from the camera, degrees (0 = facing it), signed. */
		float YawDeg = 0.f;

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

		// depth stabilisation
		float RelDepth[LM::Count] = {}, RelRaw[LM::Count] = {}, RelVel[LM::Count] = {};
		float PairSign[2] = { 0.f, 0.f };          // which shoulder / hip is nearer the camera
		bool bFrontal = true;                      // shoulders seen (nearly) square-on: widths can be measured
		bool bArmSquare[4] = { true, true, true, true };   // L upper, R upper, L fore, R fore seen side-on
		void Stabilise(const V3* Target, float Dt, bool bSnap);
		float SolvePair(int a, int b, int Which, float Width, const V3* Dir, float* Rho);
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
