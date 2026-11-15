#include "/Shaders/Common/Math.hlsli"
#include "/Shaders/Common/RainCurtain.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

static const float kCurtainScale = 4096.0f;

/// @tags rain, curtain
[numthreads(1, 1, 1)] void TestRainCurtainStrengthZeroKeepsEveryDrop() {
	for (uint index = 0u; index < 64u; ++index) {
		float3 position = float3(index * 731.0f, index * -419.0f, 200.0f + index * 37.0f);
		float hash = RainCurtain::DropHash(float3(index, index * 3u, index * 7u));
		ASSERT(AreEqual, RainCurtain::Weight(position, float2(0.0f, 0.0f), hash, kCurtainScale, 0.0f), 1.0f);
	}
}

	/// @tags rain, curtain
	[numthreads(1, 1, 1)] void TestRainCurtainWeightStaysInUnitRange()
{
	for (uint index = 0u; index < 256u; ++index) {
		float3 position = float3(index * 311.0f, index * -157.0f, index * 53.0f);
		float hash = RainCurtain::DropHash(float3(index * 5u, index, index * 11u));
		float weight = RainCurtain::Weight(position, float2(index * 13.0f, 0.0f), hash, kCurtainScale, 1.0f);
		ASSERT(IsTrue, weight >= 0.0f && weight <= 1.0f);
	}
}

/// @tags rain, curtain
[numthreads(1, 1, 1)] void TestRainCurtainThinsRainAtFullStrength() {
	uint kept = 0u;
	static const uint sampleCount = 512u;
	for (uint index = 0u; index < sampleCount; ++index) {
		float3 position = float3(index * 977.0f, index * -613.0f, index * 71.0f);
		float hash = RainCurtain::DropHash(float3(index * 17u, index * 3u, index));
		kept += RainCurtain::Weight(position, float2(0.0f, 0.0f), hash, kCurtainScale, 1.0f) >= 0.5f ? 1u : 0u;
	}
	ASSERT(IsTrue, kept > sampleCount / 5u);
	ASSERT(IsTrue, kept < sampleCount * 9u / 10u);
}

	/// @tags rain, curtain
	[numthreads(1, 1, 1)] void TestRainCurtainDriftMovesThePattern()
{
	uint changed = 0u;
	for (uint index = 0u; index < 128u; ++index) {
		float3 position = float3(index * 523.0f, index * -271.0f, 150.0f);
		float hash = RainCurtain::DropHash(float3(index, index * 13u, index * 29u));
		float still = RainCurtain::Weight(position, float2(0.0f, 0.0f), hash, kCurtainScale, 1.0f);
		float moved = RainCurtain::Weight(position, float2(2500.0f, 1700.0f), hash, kCurtainScale, 1.0f);
		changed += abs(still - moved) > 0.01f ? 1u : 0u;
	}
	ASSERT(IsTrue, changed > 0u);
}

/// @tags rain, curtain
[numthreads(1, 1, 1)] void TestRainCurtainDropHashIsStableAndInUnitRange() {
	float3 base = float3(12.0f, -34.0f, 56.0f);
	float first = RainCurtain::DropHash(base);
	ASSERT(AreEqual, first, RainCurtain::DropHash(base));
	ASSERT(IsTrue, first >= 0.0f && first < 1.0f);
}
