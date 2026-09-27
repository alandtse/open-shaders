#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Features/Upscaling/CameraReprojection.h"

namespace
{
	void RequireVectorNear(const float4& actual, const float4& expected)
	{
		REQUIRE(actual.x == Catch::Approx(expected.x).margin(0.001f));
		REQUIRE(actual.y == Catch::Approx(expected.y).margin(0.001f));
		REQUIRE(actual.z == Catch::Approx(expected.z).margin(0.001f));
		REQUIRE(actual.w == Catch::Approx(expected.w).margin(0.001f));
	}

	void RequireMatrixNear(const float4x4& actual, const float4x4& expected)
	{
		for (size_t row = 0; row < 4; ++row)
			for (size_t column = 0; column < 4; ++column)
				REQUIRE(actual.m[row][column] == Catch::Approx(expected.m[row][column]).margin(0.00001f));
	}
}

TEST_CASE("Streamline projection matches the captured AE camera", "[upscaling][camera]")
{
	const float4x4 capturedViewInverse(
		-0.1959844083f, -0.1209190413f, -0.9731232524f, 0.0f,
		0.9806070924f, -0.0241669156f, -0.1944886744f, 0.0f,
		0.0f, 0.9923682213f, -0.1233103946f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
	const float4x4 capturedViewProjection(
		-0.1795865744f, 0.8985607624f, 0.0f, 0.0f,
		-0.1969811171f, -0.0393687002f, 1.6166006327f, 0.0f,
		-0.9731644392f, -0.1944968998f, -0.1233156174f, -15.0006361008f,
		-0.9731231332f, -0.1944886446f, -0.1233103871f, 0.0f);
	const auto matrices = UpscalingCamera::BuildReprojection(
		capturedViewInverse.Transpose(), capturedViewProjection.Transpose(), capturedViewProjection.Transpose(), float3::Zero);
	const float4 viewPoint(30.0f, 20.0f, 100.0f, 1.0f);
	const auto clip = float4::Transform(viewPoint, matrices.cameraViewToClip);
	RequireVectorNear(clip, float4(27.4899352f, 32.5806642f, 85.00360775f, 100.0f));
	RequireVectorNear(float4::Transform(clip, matrices.clipToCameraView), viewPoint);
	RequireMatrixNear(matrices.clipToPrevClip, float4x4::Identity);
}

TEST_CASE("Streamline reprojection follows camera movement and projection changes", "[upscaling][camera]")
{
	const float4x4 projection(DirectX::XMMatrixPerspectiveOffCenterLH(-10.0f, 13.0f, -8.0f, 9.0f, 15.0f, 370000.0f));
	const float4x4 previousProjection(DirectX::XMMatrixPerspectiveOffCenterLH(-11.0f, 12.0f, -9.0f, 8.0f, 15.0f, 370000.0f));
	const auto view = float4x4::CreateRotationY(0.1f) * float4x4::CreateRotationX(-0.2f);
	const auto previousView = float4x4::CreateRotationY(0.08f) * float4x4::CreateRotationX(-0.18f);
	const float3 originDelta(4.0f, -3.0f, 2.0f);
	const auto matrices = UpscalingCamera::BuildReprojection(view.Invert(), view * projection, previousView * previousProjection, originDelta);
	RequireMatrixNear(matrices.cameraViewToClip, projection);
	for (const float4 point : { float4(10.0f, 5.0f, 100.0f, 1.0f), float4(-30.0f, 20.0f, 500.0f, 1.0f) }) {
		const auto currentClip = float4::Transform(float4::Transform(point, view), projection);
		const auto previousPoint = point + float4(originDelta.x, originDelta.y, originDelta.z, 0.0f);
		const auto previousClip = float4::Transform(float4::Transform(previousPoint, previousView), previousProjection);
		RequireVectorNear(float4::Transform(currentClip, matrices.clipToPrevClip), previousClip);
		RequireVectorNear(float4::Transform(previousClip, matrices.prevClipToClip), currentClip);
	}
}

TEST_CASE("DLSS and frame generation share repeatable camera transforms", "[upscaling][camera]")
{
	const float4x4 projection(DirectX::XMMatrixPerspectiveFovLH(1.2f, 16.0f / 9.0f, 15.0f, 370000.0f));
	const auto view = float4x4::CreateRotationY(0.2f);
	const auto previousView = float4x4::CreateRotationY(0.15f);
	const float3 originDelta(5.0f, 0.0f, 0.0f);
	const auto dlss = UpscalingCamera::BuildReprojection(view.Invert(), view * projection, previousView * projection, originDelta);
	const auto otherEyeView = float4x4::CreateTranslation(-3.2f, 0.0f, 0.0f) * view;
	const auto otherEye = UpscalingCamera::BuildReprojection(otherEyeView.Invert(), otherEyeView * projection, previousView * projection, originDelta);
	RequireMatrixNear(otherEye.cameraViewToClip, projection);
	const auto frameGeneration = UpscalingCamera::BuildReprojection(view.Invert(), view * projection, previousView * projection, originDelta);
	RequireMatrixNear(frameGeneration.clipToPrevClip, dlss.clipToPrevClip);
	RequireMatrixNear(frameGeneration.prevClipToClip, dlss.prevClipToClip);
	const auto still = UpscalingCamera::BuildReprojection(view.Invert(), view * projection, view * projection, float3::Zero);
	RequireMatrixNear(still.clipToPrevClip, float4x4::Identity);
}
