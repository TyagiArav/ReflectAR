#pragma once
template <class T> class TStrongObjectPtr { public: T* p = nullptr; T* Get() const { return p; } void Reset(T* q = nullptr) { p = q; } };
