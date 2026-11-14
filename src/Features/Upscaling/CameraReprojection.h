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

	/** @brief One camera's unjittered row-vector matrices and camera-relative origins for a frame. */
	struct CameraFrame
	{
		DirectX::SimpleMath::Matrix viewInverse;
		DirectX::SimpleMath::Matrix viewProjection;
		DirectX::SimpleMath::Matrix previousViewProjection;
		DirectX::SimpleMath::Vector3 origin;
		DirectX::SimpleMath::Vector3 previousOrigin;
	};

	/** @brief Reads a camera from the per-frame constant buffer, which stores column-vector matrices. */
	inline CameraFrame FromConstantBuffer(
		const DirectX::SimpleMath::Matrix& a_viewInverse,
		const DirectX::SimpleMath::Matrix& a_viewProjection,
		const DirectX::SimpleMath::Matrix& a_previousViewProjection,
		const DirectX::SimpleMath::Vector3& a_origin,
		const DirectX::SimpleMath::Vector3& a_previousOrigin)
	{
		return { a_viewInverse.Transpose(), a_viewProjection.Transpose(), a_previousViewProjection.Transpose(), a_origin, a_previousOrigin };
	}

	/** @brief Reads a camera from its engine camera-table entry, which stores row-vector matrices. */
	inline CameraFrame FromCameraTable(
		const DirectX::SimpleMath::Matrix& a_view,
		const DirectX::SimpleMath::Matrix& a_viewProjection,
		const DirectX::SimpleMath::Matrix& a_previousViewProjection,
		const DirectX::SimpleMath::Vector3& a_origin,
		const DirectX::SimpleMath::Vector3& a_previousOrigin)
	{
		return { a_view.Invert(), a_viewProjection, a_previousViewProjection, a_origin, a_previousOrigin };
	}

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
