#pragma once

#include "Utils/Subrect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace Util::Region
{
	/** @brief A crop with no extent. A pass cannot evaluate an empty subrect, so callers read it as "no crop". */
	inline constexpr Subrect::PixelRegion kEmptyRegion{ 0, 0, 0, 0 };

	/** @brief Crop edges are rounded to this pixel grid so a pass is never handed an arbitrary subrect. */
	inline constexpr uint32_t kDefaultPixelAlignment = 64;

	/** @brief Alignment that rounds an edge to whole pixels only, for a caller that shows a region exactly as it projected. */
	inline constexpr uint32_t kNoPixelAlignment = 1;

	/**
	 * @brief The crop a source tracks, one pixel-space rect per eye; inactive means the whole frame.
	 *        Both eyes default to empty, never to a one-pixel crop, so a default-constructed region
	 *        cannot be mistaken for a real one.
	 */
	struct StereoRegion
	{
		bool active = false;
		std::array<Subrect::PixelRegion, 2> eye{ kEmptyRegion, kEmptyRegion };
	};

	/** @brief Normalised [0,1] screen bounds of a projected world-space box; y runs down from the top. */
	struct ScreenBounds
	{
		float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
	};

	/** @brief What projecting a world-space box into one eye's screen rect produced. */
	enum class ProjectionResult : uint8_t
	{
		kVisible,    ///< The box overlaps the eye's [0,1] rect and the bounds are usable.
		kOffscreen,  ///< The box misses the eye entirely, or lies wholly behind the eye plane: untrackable in this eye.
		kBehindEye   ///< The box straddles the eye plane, so it fills the view but no bounds can be formed.
	};

	/** @brief Projects one world point into normalised screen space; false when it is at or behind the eye. */
	inline bool ProjectToScreen(const DirectX::SimpleMath::Matrix& a_viewProj, const float3& a_world, float2& a_out)
	{
		const auto clip = DirectX::SimpleMath::Vector4::Transform(
			DirectX::SimpleMath::Vector4(a_world.x, a_world.y, a_world.z, 1.0f), a_viewProj);
		if (!(clip.w > 0.0f) || !std::isfinite(clip.x) || !std::isfinite(clip.y))
			return false;
		const float invW = 1.0f / clip.w;
		a_out = float2{ clip.x * invW * 0.5f + 0.5f, 0.5f - clip.y * invW * 0.5f };
		return std::isfinite(a_out.x) && std::isfinite(a_out.y);
	}

	/**
	 * @brief Screen bounds of world-space points such as a box's corners, clamped to the eye's view.
	 * @return kBehindEye when some points are behind the eye plane and some in front: a clipped box would
	 *         leave part of the target outside the crop. kOffscreen when every point is behind it, or none given.
	 */
	inline ProjectionResult ProjectBounds(const DirectX::SimpleMath::Matrix& a_viewProj,
		std::span<const float3> a_corners, ScreenBounds& a_out)
	{
		ScreenBounds bounds{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
			std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		size_t cornersBehindEye = 0;
		for (const auto& corner : a_corners) {
			float2 point;
			if (!ProjectToScreen(a_viewProj, corner, point)) {
				++cornersBehindEye;
				continue;
			}
			bounds.minX = std::min(bounds.minX, point.x);
			bounds.minY = std::min(bounds.minY, point.y);
			bounds.maxX = std::max(bounds.maxX, point.x);
			bounds.maxY = std::max(bounds.maxY, point.y);
		}
		if (cornersBehindEye == a_corners.size())
			return ProjectionResult::kOffscreen;
		if (cornersBehindEye > 0)
			return ProjectionResult::kBehindEye;
		a_out.minX = std::clamp(bounds.minX, 0.0f, 1.0f);
		a_out.minY = std::clamp(bounds.minY, 0.0f, 1.0f);
		a_out.maxX = std::clamp(bounds.maxX, 0.0f, 1.0f);
		a_out.maxY = std::clamp(bounds.maxY, 0.0f, 1.0f);
		if (a_out.maxX <= a_out.minX || a_out.maxY <= a_out.minY)
			return ProjectionResult::kOffscreen;
		return ProjectionResult::kVisible;
	}

	/** @brief Share of the frame a bounds rect covers. */
	inline float AreaFraction(const ScreenBounds& a_bounds)
	{
		return (a_bounds.maxX - a_bounds.minX) * (a_bounds.maxY - a_bounds.minY);
	}

	/** @brief Distance from a bounds' centre to the frame centre, normalised so a frame corner is 1. */
	inline float NormalizedCenterDistance(const ScreenBounds& a_bounds)
	{
		const float dx = (a_bounds.minX + a_bounds.maxX) * 0.5f - 0.5f;
		const float dy = (a_bounds.minY + a_bounds.maxY) * 0.5f - 0.5f;
		return std::sqrt(dx * dx + dy * dy) / std::sqrt(0.5f);
	}

	/** @brief Padding added around a tracked source's on-screen bounds to build its crop. */
	struct Padding
	{
		float fraction;   ///< Per-axis term, as this fraction of that axis' own on-screen extent.
		float pixels;     ///< Fixed term added on top of the fractional one.
		float minPixels;  ///< Bounds on the total per-axis padding.
		float maxPixels;
	};

	/** @brief Maps a normalised UV rect to the screen bounds PixelRegionFromBounds consumes. */
	inline ScreenBounds BoundsFromUV(const Subrect::UVRegion& a_uv)
	{
		return { a_uv.x, a_uv.y, a_uv.x + a_uv.w, a_uv.y + a_uv.h };
	}

	/**
	 * @brief Pads, aligns and clamps normalised bounds into a pixel crop.
	 * @return kEmptyRegion when the crop rounds away to nothing, so the caller widens to the whole frame.
	 */
	inline Subrect::PixelRegion PixelRegionFromBounds(const ScreenBounds& a_bounds, uint32_t a_width, uint32_t a_height,
		const Padding& a_padding, uint32_t a_alignment = kDefaultPixelAlignment)
	{
		if (!a_width || !a_height)
			return kEmptyRegion;
		const auto align = [a_alignment](float a_value, bool a_up) {
			const float grid = static_cast<float>(a_alignment);
			return (a_up ? std::ceil(a_value / grid) : std::floor(a_value / grid)) * grid;
		};
		const float minX = a_bounds.minX * static_cast<float>(a_width);
		const float maxX = a_bounds.maxX * static_cast<float>(a_width);
		const float minY = a_bounds.minY * static_cast<float>(a_height);
		const float maxY = a_bounds.maxY * static_cast<float>(a_height);
		const float padX = std::clamp((maxX - minX) * a_padding.fraction + a_padding.pixels,
			a_padding.minPixels, a_padding.maxPixels);
		const float padY = std::clamp((maxY - minY) * a_padding.fraction + a_padding.pixels,
			a_padding.minPixels, a_padding.maxPixels);
		const float left = std::clamp(align(minX - padX, false), 0.0f, static_cast<float>(a_width));
		const float top = std::clamp(align(minY - padY, false), 0.0f, static_cast<float>(a_height));
		const float right = std::clamp(align(maxX + padX, true), 0.0f, static_cast<float>(a_width));
		const float bottom = std::clamp(align(maxY + padY, true), 0.0f, static_cast<float>(a_height));
		if (right <= left || bottom <= top)
			return kEmptyRegion;
		return { static_cast<uint32_t>(left), static_cast<uint32_t>(top),
			static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top) };
	}

	/**
	 * @brief Truncates a UV rect to a pixel region for the given extent, the sizing the foveated
	 *        route's Streamline and D3D copies use. No padding, alignment or frame clamping, and
	 *        w and h floor at 1 so a crop never has a zero extent.
	 */
	inline Subrect::PixelRegion SubrectFromUV(const Subrect::UVRegion& a_uv, uint32_t a_width, uint32_t a_height)
	{
		return { static_cast<uint32_t>(a_uv.x * a_width),
			static_cast<uint32_t>(a_uv.y * a_height),
			std::max<uint32_t>(1, static_cast<uint32_t>(a_width * a_uv.w)),
			std::max<uint32_t>(1, static_cast<uint32_t>(a_height * a_uv.h)) };
	}

	/**
	 * @brief Clamps a crop to a frame's pixel extent; empty when no part of it fits.
	 *        A crop measured for a different frame can land outside it, and an empty subrect would
	 *        latch the pass off.
	 */
	inline Subrect::PixelRegion ClampToFrame(const Subrect::PixelRegion& a_region, uint32_t a_width, uint32_t a_height)
	{
		if (!a_region.w || !a_region.h || !a_width || !a_height ||
			a_region.x >= a_width || a_region.y >= a_height)
			return kEmptyRegion;
		return { a_region.x, a_region.y,
			std::min(a_region.w, a_width - a_region.x),
			std::min(a_region.h, a_height - a_region.y) };
	}

	/**
	 * @brief Gives both eyes the larger crop size, sliding a crop inward when it would leave the frame.
	 *        The eyes then share one seam geometry; an eye with no crop, or one that covers the whole
	 *        frame, leaves the region untouched so a fallback eye does not enlarge the other.
	 */
	inline void MatchEyeSizes(StereoRegion& a_region, uint32_t a_width, uint32_t a_height)
	{
		if (!a_region.active)
			return;
		const auto& left = a_region.eye[0];
		const auto& right = a_region.eye[1];
		if (!left.w || !left.h || !right.w || !right.h)
			return;
		const auto coversFrame = [&](const Subrect::PixelRegion& a_eye) { return a_eye.w >= a_width && a_eye.h >= a_height; };
		if (coversFrame(left) || coversFrame(right))
			return;
		const uint32_t width = std::min(std::max(left.w, right.w), a_width);
		const uint32_t height = std::min(std::max(left.h, right.h), a_height);
		for (auto& eye : a_region.eye)
			eye = { std::min(eye.x, a_width - width), std::min(eye.y, a_height - height), width, height };
	}

	/**
	 * @brief Gives every eye the subject missed the crop of an eye that projected it, so both eyes
	 *        enhance the same part of the scene. A donor that fell back to the whole frame is not copied.
	 */
	inline void CopyCropToUnprojectedEyes(StereoRegion& a_region, const std::array<bool, 2>& a_projected, uint32_t a_width, uint32_t a_height)
	{
		for (size_t eye = 0; eye < a_region.eye.size(); ++eye) {
			if (a_projected[eye])
				continue;
			for (size_t donor = 0; donor < a_region.eye.size(); ++donor) {
				const auto& crop = a_region.eye[donor];
				if (a_projected[donor] && (crop.w < a_width || crop.h < a_height)) {
					a_region.eye[eye] = crop;
					break;
				}
			}
		}
	}

	/**
	 * @brief Geometric overlap of two crops.
	 * @return kEmptyRegion when either input is empty or they share no pixel; edges that only touch
	 *         do not overlap. Edge sums widen to 64 bits so crops near UINT32_MAX cannot wrap.
	 */
	inline Subrect::PixelRegion Intersect(const Subrect::PixelRegion& a_left, const Subrect::PixelRegion& a_right)
	{
		if (!a_left.w || !a_left.h || !a_right.w || !a_right.h)
			return kEmptyRegion;
		const uint64_t left = std::max(a_left.x, a_right.x);
		const uint64_t top = std::max(a_left.y, a_right.y);
		const uint64_t right = std::min(static_cast<uint64_t>(a_left.x) + a_left.w, static_cast<uint64_t>(a_right.x) + a_right.w);
		const uint64_t bottom = std::min(static_cast<uint64_t>(a_left.y) + a_left.h, static_cast<uint64_t>(a_right.y) + a_right.h);
		if (right <= left || bottom <= top)
			return kEmptyRegion;
		return { static_cast<uint32_t>(left), static_cast<uint32_t>(top),
			static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top) };
	}

	/**
	 * @brief Narrows each eye of a_focus to its overlap with the matching a_clip eye, so a pass crops
	 *        no wider than the clip rect it must stay inside. An inactive clip changes nothing; an
	 *        empty clip eye leaves that focus eye as it was, and a focus eye with no crop, or one the
	 *        clip does not overlap, takes the clip eye.
	 */
	inline void ClipRegion(StereoRegion& a_focus, const StereoRegion& a_clip)
	{
		if (!a_clip.active)
			return;
		a_focus.active = true;
		for (size_t eye = 0; eye < a_focus.eye.size(); ++eye) {
			const auto& clip = a_clip.eye[eye];
			if (!clip.w || !clip.h)
				continue;
			const auto& focus = a_focus.eye[eye];
			if (!focus.w || !focus.h) {
				a_focus.eye[eye] = clip;
				continue;
			}
			const auto overlap = Intersect(focus, clip);
			a_focus.eye[eye] = overlap.w && overlap.h ? overlap : clip;
		}
	}

	/** @brief True when a_inner lies entirely inside a_outer. */
	inline bool Contains(const Subrect::PixelRegion& a_outer, const Subrect::PixelRegion& a_inner)
	{
		return a_inner.x >= a_outer.x && a_inner.y >= a_outer.y &&
		       a_inner.x + a_inner.w <= a_outer.x + a_outer.w &&
		       a_inner.y + a_inner.h <= a_outer.y + a_outer.h;
	}

	/** @brief Smallest rect covering both inputs. */
	inline Subrect::PixelRegion UnionRegion(const Subrect::PixelRegion& a_left, const Subrect::PixelRegion& a_right)
	{
		const uint32_t left = std::min(a_left.x, a_right.x);
		const uint32_t top = std::min(a_left.y, a_right.y);
		const uint32_t right = std::max(a_left.x + a_left.w, a_right.x + a_right.w);
		const uint32_t bottom = std::max(a_left.y + a_left.h, a_right.y + a_right.h);
		return { left, top, right - left, bottom - top };
	}

	/** @brief Pixel area of a crop. */
	inline float RectArea(const Subrect::PixelRegion& a_region)
	{
		return static_cast<float>(a_region.w) * static_cast<float>(a_region.h);
	}

	/** @brief Smallest rect covering both inputs, where an empty input contributes nothing. */
	inline Subrect::PixelRegion UnionNonEmpty(const Subrect::PixelRegion& a_left, const Subrect::PixelRegion& a_right)
	{
		if (!a_left.w || !a_left.h)
			return a_right;
		if (!a_right.w || !a_right.h)
			return a_left;
		return UnionRegion(a_left, a_right);
	}

	/**
	 * @brief Grows a_base to also cover a_addition, per eye, unless a cropped eye would then exceed
	 *        a_maxAreaFraction of the frame; a_base is untouched on failure. Eyes a_base leaves
	 *        uncropped (whole frame) are already as large as they can be and are not checked.
	 */
	inline bool TryMergeRegions(StereoRegion& a_base, const StereoRegion& a_addition, uint32_t a_width, uint32_t a_height,
		uint32_t a_eyes, float a_maxAreaFraction)
	{
		const float frameArea = static_cast<float>(a_width) * static_cast<float>(a_height);
		StereoRegion merged = a_base;
		for (uint32_t eye = 0; eye < a_eyes && eye < merged.eye.size(); ++eye) {
			const auto& base = a_base.eye[eye];
			if (base.w >= a_width && base.h >= a_height)
				continue;
			merged.eye[eye] = UnionNonEmpty(base, a_addition.eye[eye]);
			if (RectArea(merged.eye[eye]) > a_maxAreaFraction * frameArea)
				return false;
		}
		a_base = merged;
		return true;
	}

	/** @brief True when the crop moved further than a_tolerancePixels, or appeared or disappeared. */
	inline bool RegionChanged(const StereoRegion& a_current, const StereoRegion& a_previous, float a_tolerancePixels)
	{
		if (a_current.active != a_previous.active)
			return true;
		if (!a_current.active)
			return false;
		const auto drifted = [a_tolerancePixels](uint32_t a_value, uint32_t a_reference) {
			return std::abs(static_cast<float>(a_value) - static_cast<float>(a_reference)) > a_tolerancePixels;
		};
		for (size_t eye = 0; eye < a_current.eye.size(); ++eye) {
			const auto& current = a_current.eye[eye];
			const auto& previous = a_previous.eye[eye];
			if (drifted(current.x, previous.x) || drifted(current.y, previous.y) ||
				drifted(current.w, previous.w) || drifted(current.h, previous.h))
				return true;
		}
		return false;
	}

	/** @brief When a change to the published crop resets the pass's temporal history. */
	enum class ResetPolicy : uint8_t
	{
		kOnChange,  ///< Reset once the crop moved past the tolerance, or appeared or disappeared.
		kNever      ///< Keep the history across any crop change.
	};

	/** @brief Applies a_policy to this frame's crop change. */
	inline bool ShouldResetForRegion(ResetPolicy a_policy, const StereoRegion& a_current,
		const StereoRegion& a_previous, float a_tolerancePixels)
	{
		if (a_policy == ResetPolicy::kNever)
			return false;
		return RegionChanged(a_current, a_previous, a_tolerancePixels);
	}

	/** @brief Hold, shrink-window and shrink-threshold values a RegionStabilizer runs with. */
	struct StabilizerPolicy
	{
		uint32_t holdFrames;          ///< Frames a crop is kept alive after the candidate goes inactive.
		uint32_t shrinkWindowFrames;  ///< Frames the shrink envelope is accumulated over.
		float shrinkAreaFraction;     ///< Envelope area, as a fraction of the held crop, at or below which it shrinks.
	};

	/**
	 * @brief Holds a crop steady across the per-frame noise in the tracked candidate.
	 *        Anchors while the candidate stays inside the held crop, grows only by union, and shrinks
	 *        only after a quiet window: a crop that jumped by a whole alignment step would reset the
	 *        pass's temporal history. Stateful; the caller advances it once per frame.
	 */
	struct RegionStabilizer
	{
		/** @brief Creates a stabiliser running with a_policy's thresholds. */
		explicit RegionStabilizer(const StabilizerPolicy& a_policy) :
			policy(a_policy) {}

		/** @brief Crop currently held for publication; inactive until the first candidate is adopted. */
		StereoRegion held;

		/**
		 * @brief Advances one frame and returns the crop to publish.
		 * @param a_candidate This frame's raw crop, already padded and aligned.
		 * @param a_width,a_height Frame extent the crop is clamped to; a change starts the hold over.
		 */
		StereoRegion Update(const StereoRegion& a_candidate, uint32_t a_width, uint32_t a_height)
		{
			if (a_width != frameWidth || a_height != frameHeight) {
				held = {};
				inactiveFrames = 0;
				windowFrames = 0;
				envelopeSeeded = {};
				frameWidth = a_width;
				frameHeight = a_height;
			}
			if (!a_candidate.active) {
				if (held.active && ++inactiveFrames <= policy.holdFrames)
					return held;
				held = {};
				inactiveFrames = 0;
				windowFrames = 0;
				envelopeSeeded = {};
				return {};
			}
			inactiveFrames = 0;
			if (!held.active)
				return Adopt(a_candidate);
			for (size_t eye = 0; eye < held.eye.size(); ++eye) {
				if (!Contains(held.eye[eye], a_candidate.eye[eye]))
					held.eye[eye] = UnionRegion(held.eye[eye], a_candidate.eye[eye]);
			}
			Clamp();
			MergeEnvelope(a_candidate);
			if (++windowFrames >= policy.shrinkWindowFrames) {
				for (size_t eye = 0; eye < held.eye.size(); ++eye) {
					if (envelopeSeeded[eye] && RectArea(envelope[eye]) <= policy.shrinkAreaFraction * RectArea(held.eye[eye]))
						held.eye[eye] = envelope[eye];
				}
				Clamp();
				windowFrames = 0;
				envelopeSeeded = {};
			}
			return held;
		}

		/** @brief Drops the held crop and every counter, keeping the policy. */
		void Reset()
		{
			held = {};
			frameWidth = 0;
			frameHeight = 0;
			inactiveFrames = 0;
			windowFrames = 0;
			envelope = { kEmptyRegion, kEmptyRegion };
			envelopeSeeded = {};
		}

	private:
		StabilizerPolicy policy;
		uint32_t frameWidth = 0, frameHeight = 0;
		uint32_t inactiveFrames = 0, windowFrames = 0;
		std::array<Subrect::PixelRegion, 2> envelope{ kEmptyRegion, kEmptyRegion };
		std::array<bool, 2> envelopeSeeded{};

		StereoRegion Adopt(const StereoRegion& a_candidate)
		{
			held = a_candidate;
			Clamp();
			windowFrames = 0;
			envelope = { kEmptyRegion, kEmptyRegion };
			envelopeSeeded = {};
			return held;
		}

		void MergeEnvelope(const StereoRegion& a_candidate)
		{
			for (size_t eye = 0; eye < envelope.size(); ++eye) {
				envelope[eye] = envelopeSeeded[eye] ? UnionRegion(envelope[eye], a_candidate.eye[eye]) : a_candidate.eye[eye];
				envelopeSeeded[eye] = true;
			}
		}

		void Clamp()
		{
			for (auto& region : held.eye)
				region = ClampToFrame(region, frameWidth, frameHeight);
		}
	};
}
