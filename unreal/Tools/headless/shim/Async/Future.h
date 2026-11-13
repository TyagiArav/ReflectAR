#pragma once
#include "CoreMinimal.h"
template <class T> class TFuture
{
public:
	bool bValid = false; T Val;
	bool IsValid() const { return bValid; }
	bool IsReady() const { return bValid; }
	T Get() { return Val; }
	void Reset() { bValid = false; Val = T(); }
};
