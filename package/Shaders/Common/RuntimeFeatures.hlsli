#ifndef RUNTIME_FEATURES_HLSLI
#define RUNTIME_FEATURES_HLSLI

#include "Common/SharedData.hlsli"

namespace RuntimeFeatures
{
#define RUNTIME_FEATURE(name, index) static const uint name##Feature = index;
#include "Common/RuntimeFeatureList.hlsli"
#undef RUNTIME_FEATURE

	bool IsEnabled(uint feature)
	{
		return (SharedData::RuntimeFeatureFlags[feature >> 5] & (1u << (feature & 31u))) != 0;
	}
}

#endif
