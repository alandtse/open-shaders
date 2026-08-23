// HLSL Unit Tests for CloudRelight/Draine.hlsli and CloudRelight/CloudRelight.hlsli
#include "/Shaders/CloudRelight/CloudRelight.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

namespace TestConstants
{
	static const float APPROX_TOLERANCE = 0.005f;
	static const float EXACT_TOLERANCE = 0.0001f;
}

/// @tags cloud-relight, phase-function
[numthreads(1, 1, 1)] void TestDraineNonNegative() {
	float cosThetas[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
	[unroll] for (int i = 0; i < 5; i++)
	{
		ASSERT(IsTrue, evalDraine(cosThetas[i], 0.6f, 10.0f) >= 0.0f);
		ASSERT(IsTrue, evalDraine(cosThetas[i], -0.3f, 0.0f) >= 0.0f);
	}
}

	/// @tags cloud-relight, phase-function
	[numthreads(1, 1, 1)] void TestDraineReducesToHenyeyGreenstein()
{
	// evalDraine(u, g, 0) must equal the closed-form HG phase function exactly.
	float g = 0.6f;
	float cosThetas[4] = { -0.5f, 0.0f, 0.5f, 0.99f };
	[unroll] for (int i = 0; i < 4; i++)
	{
		float u = cosThetas[i];
		float hg = (1.0f - g * g) / (4.0f * Math::PI * pow(1.0f + g * g - 2.0f * g * u, 1.5f));
		ASSERT(IsTrue, abs(evalDraine(u, g, 0.0f) - hg) < TestConstants::EXACT_TOLERANCE);
	}
}

/// @tags cloud-relight, phase-function
[numthreads(1, 1, 1)] void TestDraineForwardPeakMonotonic() {
	// Within the forward hemisphere (u in [0, 1]), evalDraine should increase monotonically
	// toward u = 1 for g > 0. Outside this range a large alpha can produce a secondary
	// backward bump (the glory/fogbow feature the paper's own model doesn't fully capture),
	// so this is intentionally scoped to the forward half, not the full [-1, 1] domain.
	float g = 0.6f, a = 5.0f;
	float p0 = evalDraine(0.0f, g, a);
	float p1 = evalDraine(0.5f, g, a);
	float p2 = evalDraine(0.9f, g, a);
	float p3 = evalDraine(0.99f, g, a);
	ASSERT(IsTrue, p1 > p0);
	ASSERT(IsTrue, p2 > p1);
	ASSERT(IsTrue, p3 > p2);
}

	/// @tags cloud-relight, phase-function
	[numthreads(1, 1, 1)] void TestSilverLiningNonNegative()
{
	float cosThetas[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
	[unroll] for (int i = 0; i < 5; i++)
	{
		ASSERT(IsTrue, CloudRelight::Phase::SilverLining(cosThetas[i], 0.0f) >= 0.0f);
	}
}

/// @tags cloud-relight, phase-function, regression
[numthreads(1, 1, 1)] void TestSilverLiningKnownValues() {
	ASSERT(IsTrue, abs(CloudRelight::Phase::SilverLining(-1.0f, 0.0f)) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::SilverLining(0.0f, 0.0f)) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::SilverLining(0.5f, 0.0f)) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::SilverLining(0.99f, 0.0f) - 3.682260f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::SilverLining(1.0f, 0.0f) - 44.362514f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, CloudRelight::Phase::SilverLining(1.0f, 0.0f) > CloudRelight::Phase::SilverLining(1.0f, 1.0f));
}

	/// @tags cloud-relight, phase-function, regression
	[numthreads(1, 1, 1)] void TestBroadSilverLiningKnownValues()
{
	ASSERT(IsTrue, abs(CloudRelight::Phase::BroadSilverLining(-1.0f) - 0.028385f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::BroadSilverLining(0.0f) - 0.006536f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::BroadSilverLining(0.5f) - 0.035419f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::BroadSilverLining(0.99f) - 1.165209f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::Phase::BroadSilverLining(1.0f) - 1.335249f) < TestConstants::APPROX_TOLERANCE);
}

/// @tags cloud-relight, optical-depth
[numthreads(1, 1, 1)] void TestOpticalDepthRoundTrip() {
	float cloudDensities[4] = { 0.0f, 0.1f, 0.5f, 0.9f };
	[unroll] for (int i = 0; i < 4; i++)
	{
		float opticalDepth = CloudRelight::GetOpticalDepth(cloudDensities[i]);
		ASSERT(IsTrue, abs(CloudRelight::GetBodyScatter(opticalDepth) - cloudDensities[i]) < TestConstants::EXACT_TOLERANCE);
	}
}

	/// @tags cloud-relight, optical-depth
	[numthreads(1, 1, 1)] void TestDirectSingleScatter()
{
	ASSERT(IsTrue, abs(CloudRelight::GetDirectSingleScatter(CloudRelight::GetOpticalDepth(0.1f)) - 0.087620f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetDirectSingleScatter(CloudRelight::GetOpticalDepth(0.5f)) - 0.370933f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetDirectSingleScatter(CloudRelight::GetOpticalDepth(0.9f)) - 0.368518f) < TestConstants::APPROX_TOLERANCE);
}

/// @tags cloud-relight, phase-function
[numthreads(1, 1, 1)] void TestBroadSilverDensityWeight() {
	ASSERT(IsTrue, abs(CloudRelight::GetBroadSilverDensityWeight(0.25f, 0.0f) - 0.75f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetBroadSilverDensityWeight(0.5f, 0.0f) - 0.5f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetBroadSilverDensityWeight(0.75f, 0.0f) - 0.25f) < TestConstants::EXACT_TOLERANCE);
}

	/// @tags cloud-relight, optical-depth
	[numthreads(1, 1, 1)] void TestSilverSingleScatterDensityWindow()
{
	ASSERT(IsTrue, abs(CloudRelight::GetSilverSingleScatter(CloudRelight::GetOpticalDepth(0.08f), 0.08f)) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetSilverSingleScatter(CloudRelight::GetOpticalDepth(0.2f), 0.2f) - 0.100706f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetSilverSingleScatter(CloudRelight::GetOpticalDepth(0.5f), 0.5f) - 0.456788f) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, abs(CloudRelight::GetSilverSingleScatter(CloudRelight::GetOpticalDepth(0.85f), 0.85f)) < TestConstants::EXACT_TOLERANCE);
}

/// @tags cloud-relight, inner-shadow
[numthreads(1, 1, 1)] void TestInnerShadowOpacity() {
	ASSERT(IsTrue, CloudRelight::GetInnerShadowOpacity(0.0f) == 0.0f);
	ASSERT(IsTrue, CloudRelight::GetInnerShadowOpacity(0.5f) == 0.25f);
	ASSERT(IsTrue, CloudRelight::GetInnerShadowOpacity(1.0f) == 1.0f);
}
