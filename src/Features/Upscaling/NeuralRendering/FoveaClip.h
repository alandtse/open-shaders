#pragma once

#include "Utils/Region.h"

#include <array>
#include <cstdint>

/** @brief Crop a foveated render imposes on an evaluation pass, from the foveation region UVs. */
namespace NR::FoveaClip
{
	/**
	 * @brief Feather band the composite fades NR out over, in render pixels. Matches
	 *        ColorTransferCS.hlsl's default band, which is what a crop with no subject box gets.
	 */
	inline constexpr float kFeatherBandPixels = 32.0f;

	/**
	 * @brief Padding added around the foveation region to build the crop. The band a crop with no
	 *        subject fades over is this wide and sits at the crop edge, so padding by it puts the
	 *        fade in the stretched periphery instead of the part of the frame the foveation sharpens.
	 */
	inline constexpr Util::Region::Padding kPadding{ 0.0f, kFeatherBandPixels, kFeatherBandPixels, kFeatherBandPixels };

	/**
	 * @brief Builds the per-eye clip a foveated render imposes, padding and aligning each eye's UV.
	 * @param a_uv Per-eye region UV from the foveation controller's own readers.
	 * @param a_width,a_height Per-eye frame extent the region is measured against.
	 * @return Inactive unless both eyes resolve to a non-empty crop: the clip narrows each eye of a
	 *         crop separately, so a clip eye the frame does not resolve would silently leave that
	 *         eye uncropped and the two eyes would stop sharing a region.
	 */
	inline Util::Region::StereoRegion BuildClip(const std::array<Util::Subrect::UVRegion, 2>& a_uv,
		uint32_t a_width, uint32_t a_height)
	{
		Util::Region::StereoRegion clip;
		for (size_t eye = 0; eye < clip.eye.size(); ++eye) {
			clip.eye[eye] = Util::Region::PixelRegionFromBounds(Util::Region::BoundsFromUV(a_uv[eye]), a_width, a_height, kPadding);
			if (!clip.eye[eye].w || !clip.eye[eye].h)
				return {};
		}
		clip.active = true;
		return clip;
	}

	/**
	 * @brief Narrows a pass's feather subject box to the clip, dropping an eye whose box falls
	 *        outside it, so the overlay and the feather band only cover the evaluated crop.
	 *        An inactive clip changes nothing, and an eye left with no box keeps none, so the
	 *        feather band falls back to its default width instead of hugging the crop edge.
	 */
	inline void ClipSubject(Util::Region::StereoRegion& a_subject, const Util::Region::StereoRegion& a_clip)
	{
		if (!a_clip.active)
			return;
		bool any = false;
		for (size_t eye = 0; eye < a_subject.eye.size(); ++eye) {
			auto& subject = a_subject.eye[eye];
			const auto& clip = a_clip.eye[eye];
			if (clip.w && clip.h && subject.w && subject.h)
				subject = Util::Region::Intersect(subject, clip);
			any = any || (subject.w != 0 && subject.h != 0);
		}
		a_subject.active = any;
	}
}
