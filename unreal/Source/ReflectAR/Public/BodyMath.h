#pragma once

#include "CoreMinimal.h"

/**
 * Shared math for the body / garment code.
 *
 * All tracking, cloth and garment code works in "body coordinates": metres, +X to the right
 * as seen facing the screen (the user's right), +Y up, +Z into the screen, left-handed: the
 * frame the Unity renderer used, so the algorithms port 1:1. The perception service's Mirror frame is the same
 * with Z flipped. AReflectARMirror's MirrorWorld component maps body coordinates to Unreal world
 * space (centimetres, X forward, Y right, Z up) with BodyToUE: a proper rotation plus x100, so
 * triangle winding is preserved.
 */
namespace ReflectAR
{
	using V3 = FVector3f;
	using V2 = FVector2f;
	using V4 = FVector4f;

	/** BlazePose landmark indices used by the renderer. */
	namespace LM
	{
		constexpr int Nose = 0, LEye = 2, REye = 5, LEar = 7, REar = 8,
			LShoulder = 11, RShoulder = 12, LElbow = 13, RElbow = 14, LWrist = 15, RWrist = 16,
			LPinky = 17, RPinky = 18, LIndex = 19, RIndex = 20, LThumb = 21, RThumb = 22,
			LHip = 23, RHip = 24, LKnee = 25, RKnee = 26, LAnkle = 27, RAnkle = 28;
		constexpr int Count = 33;
	}

	inline const V3 VUp(0.f, 1.f, 0.f), VRight(1.f, 0.f, 0.f), VFwd(0.f, 0.f, 1.f);

	inline float Clamp01(float x) { return FMath::Clamp(x, 0.f, 1.f); }
	/** Clamped lerp (Unity's Mathf.Lerp). */
	inline float Lerp(float a, float b, float t) { return a + (b - a) * Clamp01(t); }
	inline float SmoothStep(float from, float to, float t)
	{
		t = Clamp01(t);
		t = -2.f * t * t * t + 3.f * t * t;
		return to * t + from * (1.f - t);
	}
	inline float Dot(const V3& a, const V3& b) { return V3::DotProduct(a, b); }
	inline V3 Cross(const V3& a, const V3& b) { return V3::CrossProduct(a, b); }
	inline V3 LerpV(const V3& a, const V3& b, float t) { return a + (b - a) * Clamp01(t); }
	inline V3 LerpU(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
	inline V3 Normalized(const V3& v)
	{
		const float m = v.Size();
		return m > 1e-5f ? v / m : V3::ZeroVector;
	}
	inline V3 ProjectOnPlane(const V3& v, const V3& n)
	{
		const float s = n.SizeSquared();
		return s < 1e-12f ? v : v - n * (Dot(v, n) / s);
	}
	/** Spherical interpolation between two directions (result normalised). */
	inline V3 SlerpDir(V3 a, V3 b, float t)
	{
		a = Normalized(a); b = Normalized(b);
		const float d = FMath::Clamp(Dot(a, b), -1.f, 1.f);
		if (d > 0.9995f) return Normalized(LerpU(a, b, t));
		const float th = FMath::Acos(d) * Clamp01(t);
		const V3 rel = Normalized(b - a * d);
		return a * FMath::Cos(th) + rel * FMath::Sin(th);
	}

	struct FCapsuleB
	{
		V3 a = V3::ZeroVector, b = V3::ZeroVector;
		float r = 0.f;
		FCapsuleB() = default;
		FCapsuleB(const V3& A, const V3& B, float R) : a(A), b(B), r(R) {}
		V3 Closest(const V3& p) const
		{
			const V3 ab = b - a;
			const float t = Clamp01(Dot(p - a, ab) / FMath::Max(ab.SizeSquared(), 1e-6f));
			return a + ab * t;
		}
	};

	/** An orthonormal frame. Look() builds the same axes as Unity's Quaternion.LookRotation. */
	struct FFrame
	{
		V3 o = V3::ZeroVector, x = VRight, y = VUp, z = VFwd;

		static FFrame Look(const V3& origin, const V3& fwd, const V3& up)
		{
			FFrame f;
			f.o = origin;
			f.z = Normalized(fwd);
			if (f.z.IsNearlyZero()) f.z = VFwd;
			f.x = Normalized(Cross(up, f.z));
			if (f.x.IsNearlyZero()) f.x = Normalized(Cross(FMath::Abs(f.z.Y) < 0.9f ? VUp : VRight, f.z));
			f.y = Cross(f.z, f.x);
			return f;
		}
		V3 ToWorld(const V3& l) const { return o + x * l.X + y * l.Y + z * l.Z; }
		V3 Dir(const V3& l) const { return x * l.X + y * l.Y + z * l.Z; }
		V3 ToLocal(const V3& w) const { const V3 d = w - o; return V3(Dot(d, x), Dot(d, y), Dot(d, z)); }
	};

	/** Affine map from one frame's local space to another's: p -> to.ToWorld(from.ToLocal(p)). */
	struct FAffine
	{
		V3 c0 = VRight, c1 = VUp, c2 = VFwd, t = V3::ZeroVector;

		static FAffine Between(const FFrame& from, const FFrame& to)
		{
			FAffine m;
			m.c0 = to.x * from.x.X + to.y * from.y.X + to.z * from.z.X;
			m.c1 = to.x * from.x.Y + to.y * from.y.Y + to.z * from.z.Y;
			m.c2 = to.x * from.x.Z + to.y * from.y.Z + to.z * from.z.Z;
			m.t = to.o - (m.c0 * from.o.X + m.c1 * from.o.Y + m.c2 * from.o.Z);
			return m;
		}
		V3 Apply(const V3& p) const { return c0 * p.X + c1 * p.Y + c2 * p.Z + t; }
	};

	/** Perception's Mirror frame -> body coordinates (z flips). */
	inline V3 MirrorToBody(float x, float y, float z) { return V3(x, y, -z); }

	/** Body coordinates (m) -> Unreal world (cm), without the mirror reflection. */
	inline FVector BodyToUE(const V3& p) { return FVector(p.Z, p.X, p.Y) * 100.0; }
	inline FVector BodyDirToUE(const V3& v) { return FVector(v.Z, v.X, v.Y); }
	inline V3 UEToBody(const FVector& p) { return V3(float(p.Y), float(p.Z), float(p.X)) / 100.f; }

	/** Rotation taking body axes to Unreal axes (x -> Y, y -> Z, z -> X). */
	inline FQuat BodyBasis()
	{
		return FMatrix(FPlane(0, 1, 0, 0), FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 0, 0, 1)).ToQuat();
	}

	inline FVector ToFV(const V3& v) { return FVector(v.X, v.Y, v.Z); }
}
