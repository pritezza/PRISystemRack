#pragma once

#include <atomic>

namespace pri::apo
{
// DllCanUnloadNow の判定に使う。生きている APO の数と、クラスファクトリーへの参照・ロックの数
inline std::atomic<long> g_objectCount{ 0 };
inline std::atomic<long> g_lockCount{ 0 };
} // namespace pri::apo
