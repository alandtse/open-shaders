#pragma once

#include <SimpleMath.h>

namespace UpscalingCamera
{
	/** @brief Row-vector transforms for temporal reconstruction from one engine frame. */
	struct Reprojection
	{
		DirectX::SimpleMath::Matrix cameraViewToClip;
		DirectX::SimpleMath::Matrix clipToCameraView;
		DirectX::SimpleMath::Matrix clipToPrevClip;
		DirectX::SimpleMath::Matrix prevClipToClip;
	};

	/** @brief Builds unjittered row-vector transforms, accounting for the change in camera-relative origin. */
	inline Reprojection BuildReprojection(
		const DirectX::SimpleMath::Matrix& a_cameraViewInverse,
		const DirectX::SimpleMath::Matrix& a_viewProjection,
		const DirectX::SimpleMath::Matrix& a_previousViewProjection,
		const DirectX::SimpleMath::Vector3& a_cameraOriginDelta)
	{
		Reprojection result;
		result.cameraViewToClip = a_cameraViewInverse * a_viewProjection;
		result.clipToCameraView = result.cameraViewToClip.Invert();
		result.clipToPrevClip = a_viewProjection.Invert() *
		                        DirectX::SimpleMath::Matrix::CreateTranslation(a_cameraOriginDelta) * a_previousViewProjection;
		result.prevClipToClip = result.clipToPrevClip.Invert();
		return result;
	}
}
