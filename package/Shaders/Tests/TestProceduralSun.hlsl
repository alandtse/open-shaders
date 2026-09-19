// HLSL Unit Tests for ProceduralSun/ProceduralSun.hlsli
#include "/Shaders/ProceduralSun/ProceduralSun.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

namespace TestConstants
{
	static const float APPROX_TOLERANCE = 0.005f;
	static const float EXACT_TOLERANCE = 0.0001f;
	static const float SUN_DISK_COS = 0.9999572f;
	static const float SUN_HALO_COS = 0.9968761f;
}

/// @tags procedural-sun, limb-darkening
[numthreads(1, 1, 1)] void TestHestrofferCenterAndLimb() {
	float3 center = ProceduralSun::GetHestrofferLimbDarkening(0.0f);
	float3 limb = ProceduralSun::GetHestrofferLimbDarkening(1.0f);
	ASSERT(IsTrue, all(abs(center - 1.0f) < TestConstants::APPROX_TOLERANCE));
	ASSERT(IsTrue, all(abs(limb - float3(0.34685f, 0.26073f, 0.15248f)) < TestConstants::APPROX_TOLERANCE));
	ASSERT(IsTrue, all(center > limb));
}

	/// @tags procedural-sun, coverage
	[numthreads(1, 1, 1)] void TestDiscCenterAndBoundary()
{
	float3 limbDarkening;
	float coverage;
	ProceduralSun::EvaluateDisc(1.0f, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);
	ASSERT(IsTrue, coverage == 1.0f);
	ASSERT(IsTrue, all(limbDarkening > 0.0f));

	ProceduralSun::EvaluateDisc(TestConstants::SUN_DISK_COS, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);
	ASSERT(IsTrue, coverage == 0.0f);
	ASSERT(IsTrue, all(limbDarkening == 0.0f));
}

/// @tags procedural-sun, radial-mapping
[numthreads(1, 1, 1)] void TestInteriorRadialMapping() {
	float sunDiskSin = sqrt(1.0f - TestConstants::SUN_DISK_COS * TestConstants::SUN_DISK_COS);
	float halfRadiusTan = 0.5f * sunDiskSin / TestConstants::SUN_DISK_COS;
	float halfRadiusCos = rsqrt(1.0f + halfRadiusTan * halfRadiusTan);
	float3 limbDarkening;
	float coverage;
	ProceduralSun::EvaluateDisc(halfRadiusCos, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);

	float3 expected = ProceduralSun::GetHestrofferLimbDarkening(0.5f);
	ASSERT(IsTrue, all(abs(limbDarkening - expected) < TestConstants::APPROX_TOLERANCE));
	ASSERT(IsTrue, coverage == 1.0f);
}

	/// @tags procedural-sun, coverage
	[numthreads(1, 1, 1)] void TestSoftEdgeCoverage()
{
	float edgeWidth = (1.0f - TestConstants::SUN_DISK_COS) * 0.125f;
	float3 limbDarkening;
	float coverage;
	ProceduralSun::EvaluateDisc(TestConstants::SUN_DISK_COS + edgeWidth * 0.5f, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);
	ASSERT(IsTrue, abs(coverage - 0.5f) < TestConstants::APPROX_TOLERANCE);

	ProceduralSun::EvaluateDisc(TestConstants::SUN_DISK_COS + edgeWidth, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);
	ASSERT(IsTrue, abs(coverage - 1.0f) < TestConstants::EXACT_TOLERANCE);
}

/// @tags procedural-sun, robustness
[numthreads(1, 1, 1)] void TestMinimumRadiusIsFinite() {
	float minimumRadiusCos = cos(0.05f * 0.01745329252f);
	float3 limbDarkening;
	float coverage;
	ProceduralSun::EvaluateDisc(1.0f, minimumRadiusCos, 0.01f, limbDarkening, coverage);
	ASSERT(IsTrue, all(!isnan(limbDarkening)) && all(!isinf(limbDarkening)));
	ASSERT(IsTrue, !isnan(coverage) && !isinf(coverage));
	ASSERT(IsTrue, coverage >= 0.0f && coverage <= 1.0f);
}

	/// @tags procedural-sun, coverage
	[numthreads(1, 1, 1)] void TestOutsideDiscIsEmpty()
{
	float3 limbDarkening;
	float coverage;
	ProceduralSun::EvaluateDisc(0.99f, TestConstants::SUN_DISK_COS, 0.125f, limbDarkening, coverage);
	ASSERT(IsTrue, all(limbDarkening == 0.0f));
	ASSERT(IsTrue, coverage == 0.0f);
}

/// @tags procedural-sun, halo
[numthreads(1, 1, 1)] void TestHaloProfile() {
	float center = ProceduralSun::EvaluateHalo(1.0f, TestConstants::SUN_DISK_COS, TestConstants::SUN_HALO_COS, 10.0f);
	float discEdge = ProceduralSun::EvaluateHalo(TestConstants::SUN_DISK_COS, TestConstants::SUN_DISK_COS, TestConstants::SUN_HALO_COS, 10.0f);
	float midpointCos = 0.5f * (TestConstants::SUN_DISK_COS + TestConstants::SUN_HALO_COS);
	float midpoint = ProceduralSun::EvaluateHalo(midpointCos, TestConstants::SUN_DISK_COS, TestConstants::SUN_HALO_COS, 10.0f);
	float outerEdge = ProceduralSun::EvaluateHalo(TestConstants::SUN_HALO_COS, TestConstants::SUN_DISK_COS, TestConstants::SUN_HALO_COS, 10.0f);

	ASSERT(IsTrue, abs(center - 1.0f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(discEdge - 1.0f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(midpoint - (1.0f / 12.0f)) < TestConstants::APPROX_TOLERANCE);
	ASSERT(IsTrue, outerEdge == 0.0f);
	ASSERT(IsTrue, center > midpoint && midpoint > outerEdge);
}

	/// @tags procedural-sun, halo, composition
	[numthreads(1, 1, 1)] void TestDiscCompositionWithoutHalo()
{
	float3 limbDarkening = float3(0.8f, 0.6f, 0.4f);
	float3 sunColor;
	float sunCoverage;
	ProceduralSun::ComposeDiscAndHalo(limbDarkening, 0.5f, 6.0f, 0.0f, 0.4f, sunColor, sunCoverage);

	ASSERT(IsTrue, abs(sunCoverage - 0.5f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, all(abs(sunColor - limbDarkening * 6.0f) < TestConstants::APPROX_TOLERANCE));
}

/// @tags procedural-sun, halo, composition
[numthreads(1, 1, 1)] void TestHaloStraightAlphaComposition() {
	float3 sunColor;
	float sunCoverage;
	ProceduralSun::ComposeDiscAndHalo(0.0f, 0.0f, 6.0f, 0.25f, 0.4f, sunColor, sunCoverage);

	ASSERT(IsTrue, abs(sunCoverage - 0.25f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, all(abs(sunColor - 0.4f) < TestConstants::EXACT_TOLERANCE));
	ASSERT(IsTrue, all(abs(sunColor * sunCoverage - 0.1f) < TestConstants::EXACT_TOLERANCE));
}

	/// @tags procedural-sun, halo, robustness
	[numthreads(1, 1, 1)] void TestInvalidHaloRangeIsEmpty()
{
	float halo = ProceduralSun::EvaluateHalo(1.0f, TestConstants::SUN_DISK_COS, TestConstants::SUN_DISK_COS, 10.0f);
	ASSERT(IsTrue, halo == 0.0f);
}

/// @tags procedural-sun, cloud-occlusion
[numthreads(1, 1, 1)] void TestCloudTransmissionBoundaries() {
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(0.0f, 4.0f) == 1.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(1.0f, 0.0f) == 1.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(0.8f, 0.0f) == 1.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(1.0f, 1.0f) == 0.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(-1.0f, 1.0f) == 1.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(2.0f, 1.0f) == 0.0f);
	ASSERT(IsTrue, ProceduralSun::GetCloudTransmission(0.8f, -1.0f) == 1.0f);
}

	/// @tags procedural-sun, cloud-occlusion
	[numthreads(1, 1, 1)] void TestCloudTransmissionStrengthAndDensity()
{
	float thinCloud = ProceduralSun::GetCloudTransmission(0.2f, 1.0f);
	float thickCloud = ProceduralSun::GetCloudTransmission(0.8f, 1.0f);
	float strongerOcclusion = ProceduralSun::GetCloudTransmission(0.8f, 2.0f);
	float maximumOcclusion = ProceduralSun::GetCloudTransmission(0.8f, 4.0f);
	ASSERT(IsTrue, abs(thinCloud - 0.8f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(thickCloud - 0.2f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(strongerOcclusion - 0.04f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, abs(maximumOcclusion - 0.0016f) < TestConstants::EXACT_TOLERANCE);
	ASSERT(IsTrue, thinCloud > thickCloud && thickCloud > strongerOcclusion && strongerOcclusion > maximumOcclusion);
}
