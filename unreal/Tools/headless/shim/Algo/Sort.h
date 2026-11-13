#pragma once
#include <algorithm>
namespace Algo { template <class T, size_t N, class P> void Sort(T (&a)[N], P p) { std::sort(a, a + N, p); } }
