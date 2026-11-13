#pragma once
// Minimal stand-ins for the Unreal core types used by ReflectAR's pure C++ (headless tests only).
#include <cmath>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <utility>

using int32 = int32_t; using int64 = int64_t; using uint8 = uint8_t; using uint32 = uint32_t; using uint64 = uint64_t;
using TCHAR = char;
#define TEXT(x) x
#define PI 3.14159265358979f
#define HALF_PI 1.5707963267949f
#define UE_BIG_NUMBER 3.4e38f
#define MoveTemp(x) std::move(x)
#define REFLECTAR_API
template <class T> void Swap(T& a, T& b) { std::swap(a, b); }

struct FVector2D { double X = 0, Y = 0; FVector2D() {} FVector2D(double x, double y) : X(x), Y(y) {} };

struct FMath
{
	template <class T> static T Clamp(T x, T a, T b) { return x < a ? a : (x > b ? b : x); }
	template <class T> static T Abs(T x) { return x < 0 ? -x : x; }
	template <class T> static T Max(T a, T b) { return a > b ? a : b; }
	template <class T> static T Min(T a, T b) { return a < b ? a : b; }
	template <class T> static T Max3(T a, T b, T c) { return Max(Max(a, b), c); }
	template <class T> static T Min3(T a, T b, T c) { return Min(Min(a, b), c); }
	template <class T, class U> static T Lerp(const T& a, const T& b, U t) { return a + (b - a) * t; }
	static float Sqrt(float x) { return std::sqrt(x); }
	static float Pow(float a, float b) { return std::pow(a, b); }
	static float Cos(float x) { return std::cos(x); }
	static float Sin(float x) { return std::sin(x); }
	static float Tan(float x) { return std::tan(x); }
	static float Acos(float x) { return std::acos(x); }
	static float Asin(float x) { return std::asin(x); }
	static float Atan2(float y, float x) { return std::atan2(y, x); }
	static float Exp(float x) { return std::exp(x); }
	static float Loge(float x) { return std::log(x); }
	static int FloorToInt(float x) { return int(std::floor(x)); }
	static int CeilToInt(float x) { return int(std::ceil(x)); }
	static int RoundToInt(float x) { return int(std::lround(x)); }
	static float FloorToFloat(float x) { return std::floor(x); }
	static float Fmod(float a, float b) { return std::fmod(a, b); }
	template <class T> static T Square(T x) { return x * x; }
	static float DegreesToRadians(float d) { return d * PI / 180.f; }
	static float RadiansToDegrees(float r) { return r * 180.f / PI; }
	static bool IsFinite(float x) { return std::isfinite(x); }
	static float PerlinNoise2D(const FVector2D& p);   // harness.cpp
};

struct FVector3f
{
	float X = 0, Y = 0, Z = 0;
	FVector3f() {}
	explicit FVector3f(float s) : X(s), Y(s), Z(s) {}
	template <class A, class B, class C> FVector3f(A x, B y, C z) : X(float(x)), Y(float(y)), Z(float(z)) {}
	static const FVector3f ZeroVector;
	FVector3f operator+(const FVector3f& o) const { return { X + o.X, Y + o.Y, Z + o.Z }; }
	FVector3f operator-(const FVector3f& o) const { return { X - o.X, Y - o.Y, Z - o.Z }; }
	FVector3f operator*(const FVector3f& o) const { return { X * o.X, Y * o.Y, Z * o.Z }; }
	FVector3f operator*(float s) const { return { X * s, Y * s, Z * s }; }
	FVector3f operator/(float s) const { return { X / s, Y / s, Z / s }; }
	FVector3f operator-() const { return { -X, -Y, -Z }; }
	FVector3f& operator+=(const FVector3f& o) { X += o.X; Y += o.Y; Z += o.Z; return *this; }
	FVector3f& operator-=(const FVector3f& o) { X -= o.X; Y -= o.Y; Z -= o.Z; return *this; }
	FVector3f& operator*=(float s) { X *= s; Y *= s; Z *= s; return *this; }
	FVector3f& operator/=(float s) { X /= s; Y /= s; Z /= s; return *this; }
	float SizeSquared() const { return X * X + Y * Y + Z * Z; }
	float Size() const { return std::sqrt(SizeSquared()); }
	bool IsNearlyZero(float t = 1e-4f) const { return std::fabs(X) <= t && std::fabs(Y) <= t && std::fabs(Z) <= t; }
	static float DotProduct(const FVector3f& a, const FVector3f& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
	static FVector3f CrossProduct(const FVector3f& a, const FVector3f& b) { return { a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X }; }
	static float Distance(const FVector3f& a, const FVector3f& b) { return (a - b).Size(); }
};
inline const FVector3f FVector3f::ZeroVector(0.f, 0.f, 0.f);

struct FVector2f
{
	float X = 0, Y = 0;
	FVector2f() {}
	template <class A, class B> FVector2f(A x, B y) : X(float(x)), Y(float(y)) {}
	static const FVector2f ZeroVector;
	FVector2f operator+(const FVector2f& o) const { return { X + o.X, Y + o.Y }; }
	FVector2f operator-(const FVector2f& o) const { return { X - o.X, Y - o.Y }; }
	FVector2f operator*(float s) const { return { X * s, Y * s }; }
	FVector2f operator/(float s) const { return { X / s, Y / s }; }
	float Size() const { return std::sqrt(X * X + Y * Y); }
	float SizeSquared() const { return X * X + Y * Y; }
	static float Distance(const FVector2f& a, const FVector2f& b) { return (a - b).Size(); }
};
inline const FVector2f FVector2f::ZeroVector(0.f, 0.f);

struct FVector4f
{
	float X = 0, Y = 0, Z = 0, W = 0;
	FVector4f() {}
	FVector4f(float x, float y, float z, float w) : X(x), Y(y), Z(z), W(w) {}
};

struct FVector
{
	double X = 0, Y = 0, Z = 0;
	FVector() {}
	explicit FVector(double s) : X(s), Y(s), Z(s) {}
	FVector(double x, double y, double z) : X(x), Y(y), Z(z) {}
	FVector operator*(double s) const { return { X * s, Y * s, Z * s }; }
	static const FVector ZeroVector;
};
inline const FVector FVector::ZeroVector(0, 0, 0);
struct FQuat {};
struct FPlane { FPlane(double, double, double, double) {} };
struct FMatrix { FMatrix() {} FMatrix(FPlane, FPlane, FPlane, FPlane) {} FQuat ToQuat() const { return {}; } };

struct FColor
{
	uint8 B = 0, G = 0, R = 0, A = 255;
	FColor() {}
	FColor(uint8 r, uint8 g, uint8 b, uint8 a = 255) : B(b), G(g), R(r), A(a) {}
	static FColor FromHex(const char* h)
	{
		auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
		FColor c; c.R = uint8(v(h[0]) * 16 + v(h[1])); c.G = uint8(v(h[2]) * 16 + v(h[3])); c.B = uint8(v(h[4]) * 16 + v(h[5]));
		return c;
	}
};
struct FLinearColor
{
	float R = 0, G = 0, B = 0, A = 1;
	FLinearColor() {}
	FLinearColor(float r, float g, float b, float a = 1.f) : R(r), G(g), B(b), A(a) {}
	static float ToLin(uint8 c) { const float s = c / 255.f; return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
	FLinearColor(const FColor& c) : R(ToLin(c.R)), G(ToLin(c.G)), B(ToLin(c.B)), A(c.A / 255.f) {}
	FLinearColor operator*(float s) const { return { R * s, G * s, B * s, A * s }; }
	FLinearColor operator*(const FLinearColor& o) const { return { R * o.R, G * o.G, B * o.B, A * o.A }; }
	FLinearColor operator+(const FLinearColor& o) const { return { R + o.R, G + o.G, B + o.B, A + o.A }; }
	FLinearColor operator-(const FLinearColor& o) const { return { R - o.R, G - o.G, B - o.B, A - o.A }; }
	static const FLinearColor White, Gray, Black;
};
inline const FLinearColor FLinearColor::White(1, 1, 1), FLinearColor::Gray(0.5f, 0.5f, 0.5f), FLinearColor::Black(0, 0, 0);

struct FString
{
	std::string s;
	FString() {}
	FString(const char* c) : s(c) {}
	FString(const std::string& c) : s(c) {}
	FString operator+(const FString& o) const { return FString(s + o.s); }
	FString operator+(const char* o) const { return FString(s + o); }
	const char* operator*() const { return s.c_str(); }
	bool operator==(const FString& o) const { return s == o.s; }
};

template <class K, class V> struct TPair { K Key; V Value; TPair() {} TPair(const K& k, const V& v) : Key(k), Value(v) {} };

template <class T> class TArray
{
public:
	std::vector<T> v;
	TArray() {}
	TArray(std::initializer_list<T> l) : v(l) {}
	int32 Num() const { return int32(v.size()); }
	bool IsEmpty() const { return v.empty(); }
	void SetNum(int n) { v.resize(size_t(n)); }
	void SetNumZeroed(int n) { v.resize(size_t(n)); }
	void SetNumUninitialized(int n) { v.resize(size_t(n)); }
	int32 Add(const T& x) { v.push_back(x); return Num() - 1; }
	template <class... A> int32 Emplace(A&&... a) { v.emplace_back(std::forward<A>(a)...); return Num() - 1; }
	int32 AddZeroed(int n = 1) { const int b = Num(); v.resize(v.size() + size_t(n)); return b; }
	T& AddDefaulted_GetRef() { v.emplace_back(); return v.back(); }
	void Reserve(int n) { v.reserve(size_t(n)); }
	void Reset() { v.clear(); }
	void Empty() { v.clear(); }
	void Init(const T& x, int n) { v.assign(size_t(n), x); }
	void Append(std::initializer_list<T> l) { v.insert(v.end(), l.begin(), l.end()); }
	void Append(const TArray<T>& o) { v.insert(v.end(), o.v.begin(), o.v.end()); }
	T& operator[](int i) { return v[size_t(i)]; }
	const T& operator[](int i) const { return v[size_t(i)]; }
	T& Last() { return v.back(); }
	const T& Last() const { return v.back(); }
	T* GetData() { return v.data(); }
	const T* GetData() const { return v.data(); }
	auto begin() { return v.begin(); }
	auto end() { return v.end(); }
	auto begin() const { return v.begin(); }
	auto end() const { return v.end(); }
};

template <class K, class V> class TMap
{
public:
	std::unordered_map<K, size_t> idx;
	std::deque<TPair<K, V>> e;
	void Reserve(int n) { idx.reserve(size_t(n)); }
	V* Find(const K& k) { auto it = idx.find(k); return it == idx.end() ? nullptr : &e[it->second].Value; }
	const V* Find(const K& k) const { auto it = idx.find(k); return it == idx.end() ? nullptr : &e[it->second].Value; }
	V& Add(const K& k, const V& v) { auto it = idx.find(k); if (it != idx.end()) { e[it->second].Value = v; return e[it->second].Value; } idx[k] = e.size(); e.emplace_back(k, v); return e.back().Value; }
	V& FindOrAdd(const K& k) { auto it = idx.find(k); if (it != idx.end()) return e[it->second].Value; idx[k] = e.size(); e.emplace_back(k, V()); return e.back().Value; }
	bool Contains(const K& k) const { return idx.count(k) > 0; }
	int32 Num() const { return int32(e.size()); }
	auto begin() { return e.begin(); }
	auto end() { return e.end(); }
	auto begin() const { return e.begin(); }
	auto end() const { return e.end(); }
};
template <class T> class TSet
{
public:
	std::unordered_map<T, bool> s;
	void Add(const T& k) { s[k] = true; }
	bool Contains(const T& k) const { return s.count(k) > 0; }
};

template <class T> class TSharedPtr
{
public:
	std::shared_ptr<T> p;
	TSharedPtr() {}
	TSharedPtr(std::nullptr_t) {}
	TSharedPtr(std::shared_ptr<T> q) : p(std::move(q)) {}
	bool IsValid() const { return bool(p); }
	T* Get() const { return p.get(); }
	T* operator->() const { return p.get(); }
	T& operator*() const { return *p; }
	void Reset() { p.reset(); }
};
template <class T, class... A> TSharedPtr<T> MakeShared(A&&... a) { return TSharedPtr<T>(std::make_shared<T>(std::forward<A>(a)...)); }
template <class Sig> using TFunctionRef = std::function<Sig>;
template <class Sig> using TFunction = std::function<Sig>;

struct FPlatformTime
{
	static double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
};
