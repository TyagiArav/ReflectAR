#pragma once
#include "Async/Future.h"
enum class EAsyncExecution { ThreadPool };
template <class F> auto Async(EAsyncExecution, F&& f) -> TFuture<decltype(f())> { TFuture<decltype(f())> r; r.Val = f(); r.bValid = true; return r; }
