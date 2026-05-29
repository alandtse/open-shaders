#pragma once

// Test prelude (force-included into the cpp_tests target).
//
// The plugin's PCH (include/PCH.h) defines these DirectXMath/SimpleMath
// aliases project-wide, but the cpp_tests target doesn't use that PCH (it
// would drag in RE/Skyrim.h, SKSE, detours, etc.). Force-including this gives
// units that rely on the aliases (e.g. SphericalHarmonics) the same float
// types so they compile standalone. Harmless for units that don't use them.
#include <SimpleMath.h>

// std facilities the plugin PCH provides transitively and that unit-under-test
// .cpp files rely on (e.g. SphericalHarmonics.cpp uses std::clamp).
#include <algorithm>
#include <cmath>

using float2 = DirectX::SimpleMath::Vector2;
using float3 = DirectX::SimpleMath::Vector3;
using float4 = DirectX::SimpleMath::Vector4;
using float4x4 = DirectX::SimpleMath::Matrix;
