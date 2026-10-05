#pragma once

#include "Utils/Region.h"

#include <array>
#include <cstdint>

/** @brief Crop a foveated render imposes on an evaluation pass, from the foveation region UVs. */
namespace NR::FoveaClip
{
	/** @brief Feather band the composite fades NR out over, in render pixels; ColorTransferCS.hlsl's default. */
	inline constexpr float kFeatherBandPixels = 32.0f;

	/** @brief Padding around the foveation region, so the feather fade lands in the stretched periphery. */
	inline constexpr Util::Region::Padding kPadding{ 0.0f, kFeatherBandPixels, kFeatherBandPixels, kFeatherBandPixels };

	/**
	 * @brief Builds the per-eye clip a foveated render imposes, padding and aligning each eye's UV.
	 * @param a_uv Per-eye region UV from the foveation controller.
	 * @param a_width,a_height Per-eye frame extent the region is measured against.
	 * @return Inactive unless both eyes resolve to a non-empty crop, so the eyes keep sharing a region.
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
	 * @brief Narrows a feather subject box to the clip, dropping an eye whose box falls outside it.
	 *        An inactive clip changes nothing; an eye left with no box keeps the default feather band.
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
