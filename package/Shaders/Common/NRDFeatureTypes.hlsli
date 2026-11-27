#ifndef NRD_FEATURE_TYPES_HLSLI
#define NRD_FEATURE_TYPES_HLSLI

// Included inside the SharedData namespace by Common/SharedData.hlsli.
// NRD settings follow the snow slot in the CPU FeatureData buffer.
// Keep the layout independent of feature and shader-stage defines.
struct SSGISettings
{
	uint Enabled;
	uint EnableIL;
	uint ReplaceAmbient;
	float AOPower;
	float ILSaturation;
	float3 pad0;
};

struct SSRSettings
{
	uint Enabled;
	float SpecularMult;
	float SpecCubemapMult;
	uint pad0;
};
#endif
