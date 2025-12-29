// HLSL Unit Tests for Common/FrameBuffer.hlsli
// Tests coordinate transformations, dynamic resolution, and VR/non-VR variants
#include "/Test/STF/ShaderTestFramework.hlsli"

// ============================================================================
// NON-VR VARIANT TESTS
// ============================================================================

/// @tags framebuffer, non-vr, coordinates
[numthreads(1, 1, 1)]
void TestIsOutsideFrameBasic()
{
	// Test UV boundary detection (no dynamic resolution)

	// Inside frame
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(0.0, 0.0)));
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(0.5, 0.5)));
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(1.0, 1.0)));

	// Outside frame - negative
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(-0.1, 0.5)));
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(0.5, -0.1)));
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(-0.1, -0.1)));

	// Outside frame - beyond 1.0
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(1.1, 0.5)));
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(0.5, 1.1)));
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(1.1, 1.1)));
}

/// @tags framebuffer, non-vr, coordinates, edge-cases
[numthreads(1, 1, 1)]
void TestIsOutsideFrameEdgeCases()
{
	// Test exact boundaries (should be inside)
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(0.0, 0.0)));
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(1.0, 1.0)));
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(0.0, 1.0)));
	ASSERT(IsFalse, FrameBuffer::IsOutsideFrame(float2(1.0, 0.0)));

	// Test just barely outside
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(-0.0001, 0.5)));
	ASSERT(IsTrue, FrameBuffer::IsOutsideFrame(float2(1.0001, 0.5)));
}

/// @tags framebuffer, non-vr, srgb, gamma
[numthreads(1, 1, 1)]
void TestToSRGBColorWithMockGamma()
{
	// Mock gamma value (inverse gamma = 1/2.2 ≈ 0.4545)
	FrameBuffer::FrameParams = float4(0.4545, 0, 0, 0);

	// Test linear to sRGB conversion
	// Linear white should stay white
	float3 linearWhite = float3(1.0, 1.0, 1.0);
	float3 srgbWhite = FrameBuffer::ToSRGBColor(linearWhite);
	ASSERT(IsTrue, abs(srgbWhite.x - 1.0) < 0.01);
	ASSERT(IsTrue, abs(srgbWhite.y - 1.0) < 0.01);
	ASSERT(IsTrue, abs(srgbWhite.z - 1.0) < 0.01);

	// Linear black should stay black
	float3 linearBlack = float3(0.0, 0.0, 0.0);
	float3 srgbBlack = FrameBuffer::ToSRGBColor(linearBlack);
	ASSERT(AreEqual, srgbBlack.x, 0.0);
	ASSERT(AreEqual, srgbBlack.y, 0.0);
	ASSERT(AreEqual, srgbBlack.z, 0.0);

	// Mid-gray should be brighter in sRGB (gamma < 1 brightens)
	float3 linearMidGray = float3(0.5, 0.5, 0.5);
	float3 srgbMidGray = FrameBuffer::ToSRGBColor(linearMidGray);
	// pow(0.5, 0.4545) ≈ 0.73
	ASSERT(IsTrue, srgbMidGray.x > 0.7 && srgbMidGray.x < 0.75);
}

/// @tags framebuffer, non-vr, dynamic-resolution
[numthreads(1, 1, 1)]
void TestDynamicResolutionAdjustment()
{
	// Mock dynamic resolution params
	// Simulate 0.75x resolution (75% of native)
	float drScale = 0.75;
	FrameBuffer::DynamicResolutionParams1 = float4(drScale, drScale, drScale, drScale);  // xy = current, zw = previous
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / drScale, 1.0 / drScale, drScale, drScale);

	// Test adjusted screen position
	float2 nativeCenter = float2(0.5, 0.5);
	float2 drCenter = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(nativeCenter, 0);

	// Should be scaled by 0.75
	ASSERT(IsTrue, abs(drCenter.x - 0.375) < 0.01);  // 0.5 * 0.75 = 0.375
	ASSERT(IsTrue, abs(drCenter.y - 0.375) < 0.01);

	// Test unadjustment (should reverse the scaling)
	float2 unadjusted = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(drCenter);
	ASSERT(IsTrue, abs(unadjusted.x - nativeCenter.x) < 0.01);
	ASSERT(IsTrue, abs(unadjusted.y - nativeCenter.y) < 0.01);
}

/// @tags framebuffer, non-vr, dynamic-resolution, clamping
[numthreads(1, 1, 1)]
void TestDynamicResolutionClamping()
{
	// Mock 0.5x dynamic resolution
	float drScale = 0.5;
	FrameBuffer::DynamicResolutionParams1 = float4(drScale, drScale, drScale, drScale);
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / drScale, 1.0 / drScale, drScale, drScale);

	// Test that positions are clamped properly
	float2 farEdge = float2(1.0, 1.0);
	float2 drFarEdge = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(farEdge, 0);

	// Should be clamped to max value (0.5 in this case)
	ASSERT(IsTrue, drFarEdge.x <= 0.5);
	ASSERT(IsTrue, drFarEdge.y <= 0.5);

	// Test that negative values clamp to 0
	float2 negative = float2(-0.1, -0.1);
	float2 drNegative = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(negative, 0);
	ASSERT(AreEqual, drNegative.x, 0.0);
	ASSERT(AreEqual, drNegative.y, 0.0);
}

/// @tags framebuffer, non-vr, transforms, identity
[numthreads(1, 1, 1)]
void TestWorldViewTransformIdentity()
{
	// Set up identity view matrix
	FrameBuffer::CameraView[0] = float4x4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	);
	FrameBuffer::CameraViewInverse[0] = FrameBuffer::CameraView[0];  // Identity inverse is identity

	// Test that transforming with identity doesn't change position
	float3 worldPos = float3(10, 20, 30);
	float3 viewPos = FrameBuffer::WorldToView(worldPos, true, 0);

	ASSERT(AreEqual, viewPos.x, worldPos.x);
	ASSERT(AreEqual, viewPos.y, worldPos.y);
	ASSERT(AreEqual, viewPos.z, worldPos.z);

	// Test round-trip
	float3 worldPosReconstructed = FrameBuffer::ViewToWorld(viewPos, true, 0);
	ASSERT(AreEqual, worldPosReconstructed.x, worldPos.x);
	ASSERT(AreEqual, worldPosReconstructed.y, worldPos.y);
	ASSERT(AreEqual, worldPosReconstructed.z, worldPos.z);
}

/// @tags framebuffer, non-vr, transforms, direction-vector
[numthreads(1, 1, 1)]
void TestWorldViewTransformDirectionVector()
{
	// Set up identity view matrix
	FrameBuffer::CameraView[0] = float4x4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	);
	FrameBuffer::CameraViewInverse[0] = FrameBuffer::CameraView[0];

	// Test transforming a direction vector (is_position = false)
	// Direction vectors shouldn't be affected by translation
	float3 direction = float3(1, 0, 0);  // Unit X direction
	float3 viewDir = FrameBuffer::WorldToView(direction, false, 0);

	// With identity matrix, should be unchanged
	ASSERT(AreEqual, viewDir.x, direction.x);
	ASSERT(AreEqual, viewDir.y, direction.y);
	ASSERT(AreEqual, viewDir.z, direction.z);
}

// ============================================================================
// VR VARIANT TESTS
// ============================================================================

/// @tags framebuffer, vr, dynamic-resolution, stereo
/// @define VR
[numthreads(1, 1, 1)]
void TestVRDynamicResolutionLeftEye()
{
	// Mock dynamic resolution for VR
	float drScale = 0.75;
	FrameBuffer::DynamicResolutionParams1 = float4(drScale, drScale, drScale, drScale);
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / drScale, 1.0 / drScale, drScale, drScale);

	// Test left eye (x < 0.5)
	float2 leftEyeCenter = float2(0.25, 0.5);  // Center of left viewport
	float2 drLeftEye = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(leftEyeCenter, 1);

	// Should be scaled and within left half
	ASSERT(IsTrue, drLeftEye.x < 0.5);
	ASSERT(IsTrue, drLeftEye.x >= 0.0);
}

/// @tags framebuffer, vr, dynamic-resolution, stereo
/// @define VR
[numthreads(1, 1, 1)]
void TestVRDynamicResolutionRightEye()
{
	// Mock dynamic resolution for VR
	float drScale = 0.75;
	FrameBuffer::DynamicResolutionParams1 = float4(drScale, drScale, drScale, drScale);
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / drScale, 1.0 / drScale, drScale, drScale);

	// Test right eye (x >= 0.5)
	float2 rightEyeCenter = float2(0.75, 0.5);  // Center of right viewport
	float2 drRightEye = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(rightEyeCenter, 1);

	// Should be scaled and within right half
	ASSERT(IsTrue, drRightEye.x >= 0.5);
	ASSERT(IsTrue, drRightEye.x <= 1.0);
}

/// @tags framebuffer, vr, dynamic-resolution, no-stereo
/// @define VR
[numthreads(1, 1, 1)]
void TestVRDynamicResolutionNoStereo()
{
	// Mock dynamic resolution for VR
	float drScale = 0.75;
	FrameBuffer::DynamicResolutionParams1 = float4(drScale, drScale, drScale, drScale);
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / drScale, 1.0 / drScale, drScale, drScale);

	// Test with stereo = 0 (no stereo clamping)
	float2 testPos = float2(0.75, 0.5);
	float2 drNoStereo = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(testPos, 0);

	// Should be scaled but not clamped to eye halves
	ASSERT(IsTrue, abs(drNoStereo.x - (0.75 * drScale)) < 0.01);
}

/// @tags framebuffer, vr, transforms, dual-eye
/// @define VR
[numthreads(1, 1, 1)]
void TestVRDualEyeTransforms()
{
	// Set up different view matrices for left and right eyes
	// Left eye identity
	FrameBuffer::CameraView[0] = float4x4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	);
	FrameBuffer::CameraViewInverse[0] = FrameBuffer::CameraView[0];

	// Right eye with small offset (simulating IPD)
	FrameBuffer::CameraView[1] = float4x4(
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0.065, 0, 0, 1  // ~65mm IPD offset
	);
	FrameBuffer::CameraViewInverse[1] = FrameBuffer::CameraView[1];

	float3 worldPos = float3(10, 20, 30);

	// Transform for left eye
	float3 viewPosLeft = FrameBuffer::WorldToView(worldPos, true, 0);

	// Transform for right eye (should be different due to IPD)
	float3 viewPosRight = FrameBuffer::WorldToView(worldPos, true, 1);

	// Should be different (at least X coordinate due to IPD offset)
	ASSERT(IsTrue, abs(viewPosLeft.x - viewPosRight.x) > 0.01);
}

/// @tags framebuffer, vr, previous-frame, temporal
/// @define VR
[numthreads(1, 1, 1)]
void TestVRPreviousFrameDynamicResolution()
{
	// Mock different current and previous DR scales
	float currentScale = 0.75;
	float previousScale = 0.5;

	FrameBuffer::DynamicResolutionParams1 = float4(currentScale, currentScale, previousScale, previousScale);
	FrameBuffer::DynamicResolutionParams2 = float4(1.0 / currentScale, 1.0 / currentScale, currentScale, previousScale);

	float2 screenPos = float2(0.75, 0.5);  // Right eye

	// Test previous frame adjustment
	float2 drPrevious = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(screenPos);

	// Should use previous scale (0.5)
	ASSERT(IsTrue, abs(drPrevious.x - (0.75 * previousScale)) < 0.1);

	// Should be clamped to right eye half
	ASSERT(IsTrue, drPrevious.x >= 0.5);
}

// ============================================================================
// CROSS-VARIANT TESTS (ensure VR and non-VR produce compatible results)
// ============================================================================

/// @tags framebuffer, non-vr, cross-variant
[numthreads(1, 1, 1)]
void TestNonVRSRGBConsistency()
{
	// Test that sRGB conversion is consistent regardless of variant
	FrameBuffer::FrameParams = float4(1.0 / 2.2, 0, 0, 0);

	float3 linearColor = float3(0.5, 0.3, 0.8);
	float3 srgbColor = FrameBuffer::ToSRGBColor(linearColor);

	// Verify result is finite and in valid range
	ASSERT(IsTrue, all(srgbColor >= 0.0));
	ASSERT(IsTrue, all(srgbColor <= 1.0));
	ASSERT(IsTrue, isfinite(srgbColor.x));
	ASSERT(IsTrue, isfinite(srgbColor.y));
	ASSERT(IsTrue, isfinite(srgbColor.z));
}

/// @tags framebuffer, vr, cross-variant
/// @define VR
[numthreads(1, 1, 1)]
void TestVRSRGBConsistency()
{
	// Same test as non-VR to ensure consistent behavior
	FrameBuffer::FrameParams = float4(1.0 / 2.2, 0, 0, 0);

	float3 linearColor = float3(0.5, 0.3, 0.8);
	float3 srgbColor = FrameBuffer::ToSRGBColor(linearColor);

	// Verify result is finite and in valid range
	ASSERT(IsTrue, all(srgbColor >= 0.0));
	ASSERT(IsTrue, all(srgbColor <= 1.0));
	ASSERT(IsTrue, isfinite(srgbColor.x));
	ASSERT(IsTrue, isfinite(srgbColor.y));
	ASSERT(IsTrue, isfinite(srgbColor.z));
}
