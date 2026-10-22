#pragma once

#include "Features/Screenshot/SequencePolicy.h"
#include <DirectXTex.h>
#include <cstring>

namespace OS::Capture
{
	struct NativePlane
	{
		uint32_t width, height;
		std::span<const uint8_t> pixels;
	};

	/** Combines one native image or a same-sized left/right pair without filtering. */
	inline DirectX::ScratchImage CombinePlanes(std::span<const NativePlane> planes)
	{
		if (planes.empty() || planes.size() > 2)
			throw std::invalid_argument("one source or two stereo planes are required");
		const auto width = planes[0].width;
		const auto height = planes[0].height;
		const uint64_t rowBytes = uint64_t(width) * 4;
		if (!width || !height || uint64_t(width) * height > MaximumFrameBytes / (4 * planes.size()))
			throw std::invalid_argument("image dimensions exceed the frame budget");
		for (const auto& plane : planes) {
			if (plane.width != width || plane.height != height || plane.pixels.size() != rowBytes * height)
				throw std::invalid_argument("native plane dimensions or pixel lengths disagree");
		}
		DirectX::ScratchImage image;
		if (FAILED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width * planes.size(), height, 1, 1)))
			throw std::runtime_error("native image allocation failed");
		const auto* target = image.GetImage(0, 0, 0);
		for (size_t eye = 0; eye < planes.size(); ++eye) {
			for (uint32_t row = 0; row < height; ++row)
				std::memcpy(target->pixels + row * target->rowPitch + eye * rowBytes, planes[eye].pixels.data() + row * rowBytes, rowBytes);
		}
		return image;
	}

	/** Encodes the native SDR pixels using the same WIC colour flags as OS still capture. */
	inline DirectX::Blob EncodeImage(const DirectX::Image& image, bool png)
	{
		DirectX::Blob blob;
		if (FAILED(DirectX::SaveToWICMemory(image, png ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_NONE,
				DirectX::GetWICCodec(png ? DirectX::WIC_CODEC_PNG : DirectX::WIC_CODEC_BMP), blob)))
			throw std::runtime_error("lossless image encoding failed");
		return blob;
	}
}
