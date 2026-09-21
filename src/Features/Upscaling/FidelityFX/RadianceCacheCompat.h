#pragma once

#include <FidelityFX/api/include/ffx_api.hpp>

#ifndef FFX_API_EFFECT_ID_RADIANCECACHE
#	define FFX_API_EFFECT_ID_RADIANCECACHE 0x00060000u
#endif

#ifndef FFX_API_CREATE_CONTEXT_DESC_TYPE_RADIANCECACHE
#	define FFX_API_CREATE_CONTEXT_DESC_TYPE_RADIANCECACHE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_RADIANCECACHE, 0x02)
#endif

#ifndef FFX_RADIANCECACHE_VERSION_MAJOR
#	define FFX_RADIANCECACHE_VERSION_MAJOR 0
#	define FFX_RADIANCECACHE_VERSION_MINOR 9
#	define FFX_RADIANCECACHE_VERSION_PATCH 0
#	define FFX_RADIANCECACHE_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#	define FFX_RADIANCECACHE_VERSION FFX_RADIANCECACHE_MAKE_VERSION(FFX_RADIANCECACHE_VERSION_MAJOR, FFX_RADIANCECACHE_VERSION_MINOR, FFX_RADIANCECACHE_VERSION_PATCH)
#endif

typedef struct ffxCreateContextDescRadianceCache
{
	ffxCreateContextDescHeader header;
	uint32_t flags;
	uint32_t version;
	uint32_t maxInferenceSampleCount;
	uint32_t maxTrainingSampleCount;
} ffxCreateContextDescRadianceCache;

#ifndef FFX_API_DISPATCH_DESC_TYPE_RADIANCECACHE
#	define FFX_API_DISPATCH_DESC_TYPE_RADIANCECACHE \
		FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_RADIANCECACHE, 0x04)
#endif

#ifndef FFX_RADIANCE_CACHE_DISPATCH_INFERENCE
#	define FFX_RADIANCE_CACHE_DISPATCH_INFERENCE (1u << 0)
#	define FFX_RADIANCE_CACHE_DISPATCH_TRAINING (1u << 1)
#	define FFX_RADIANCE_CACHE_CLEAR_INFERENCE_COUNTER (1u << 2)
#	define FFX_RADIANCE_CACHE_CLEAR_TRAINING_COUNTER (1u << 3)
#	define FFX_RADIANCE_CACHE_CLEAR_ALL_COUNTERS \
		(FFX_RADIANCE_CACHE_CLEAR_INFERENCE_COUNTER | FFX_RADIANCE_CACHE_CLEAR_TRAINING_COUNTER)
#	define FFX_RADIANCE_CACHE_RESET (1u << 4)
#	define FFX_RADIANCE_CACHE_OVERRIDE_LEARNING_RATE (1u << 5)
#	define FFX_RADIANCE_CACHE_OVERRIDE_WEIGHT_SMOOTHING (1u << 6)
#endif

typedef struct ffxDispatchDescRadianceCache
{
	ffxDispatchDescHeader header;
	void* commandList;
	FfxApiResource predictionInputs;
	FfxApiResource predictionOutputs;
	FfxApiResource trainInputs;
	FfxApiResource trainTargets;
	FfxApiResource sampleCounters;
	uint32_t flags;
	struct
	{
		float learningRate;
		float weightSmoothing;
	} overrides;
} ffxDispatchDescRadianceCache;
