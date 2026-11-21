#pragma once

#include "../../Utils/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// Pure helpers extracted from ShadowCasterManager so they can be unit-tested
// without the game/RE runtime.
namespace ShadowCasterManager
{
	using Util::HashCombine;
	using Util::HashCombineFloat;
	using Util::QuantizeFloat;

	/// Shadow strength of a slot: the fade-in ramp from a_fadeStart (a_fadeIn seconds) capped by the fade-out ramp from
	/// a_releaseStart (a_fadeOut seconds); a negative start means no ramp.
	inline float ShadowFadeAlpha(double a_now, double a_fadeStart, double a_releaseStart, float a_fadeIn, float a_fadeOut) noexcept
	{
		float alpha = 1.0f;
		if (a_fadeStart >= 0.0)
			alpha = std::clamp(static_cast<float>(a_now - a_fadeStart) / a_fadeIn, 0.0f, 1.0f);
		if (a_releaseStart >= 0.0)
			alpha = std::min(alpha, 1.0f - std::clamp(static_cast<float>(a_now - a_releaseStart) / a_fadeOut, 0.0f, 1.0f));
		return alpha;
	}

	/// The fade-in start that shows a_shown at a_now, so a leaving light chosen again fades back in from where it stood.
	inline double ResumedFadeStart(double a_now, float a_shown, float a_duration) noexcept
	{
		return a_now - static_cast<double>(std::clamp(a_shown, 0.0f, 1.0f)) * a_duration;
	}

	/// True when a caster switched static / moving after the light's bake rendered: the bake lacks it or holds it at an old pose.
	inline bool CasterChangedSinceBake(int a_classEpoch, int a_bakeEpoch) noexcept
	{
		return a_classEpoch > a_bakeEpoch;
	}

	/// True when the composite pass leaves a caster to the bake: one static since the bake, or one that started moving after it,
	/// which the bake still holds at its old pose until the rebake, so it is never drawn beside its baked copy.
	inline bool CompositeSkipsCaster(bool a_dynamic, bool a_bakeStale) noexcept
	{
		return a_dynamic == a_bakeStale;
	}

	/// True for a rebake that counts toward the pose-rebake latch: one queued only because a caster changed class, while the light's
	/// valid bake still matches its pose and the static hash has not diverged for three accumulates, does not.
	inline bool CountsTowardPoseLatch(bool a_bakeStale, bool a_staticValid, uint8_t a_mismatchStreak, bool a_poseMatches) noexcept
	{
		return !(a_bakeStale && a_staticValid && a_mismatchStreak < 3 && a_poseMatches);
	}

	/// Largest unscaled bound radius of a part that can hold a light's flame, and how far past its bound the flame may sit.
	inline constexpr float kFlameEnclosureMaxRadius = 20.0f;
	inline constexpr float kFlameEnclosureReachMargin = 4.0f;
	/// How far over the top of a small part standing under it a light may hang and still count that part as its holder.
	inline constexpr float kFlameAboveHolderMax = 24.0f;
	/// Largest local offset of a light from its parent still read as flicker wobble rather than a placed offset.
	inline constexpr float kFlickerWobbleMaxOffset = 16.0f;

	/// True when a part whose world bound has radius a_radius, at a_dx/a_dy/a_dz from the flame, is small enough at its world scale
	/// a_scale to be the part holding that flame: it holds the flame within its bound plus the margin, or stands under it with the
	/// flame at most kFlameAboveHolderMax over its top.
	inline bool HoldsFlame(float a_dx, float a_dy, float a_dz, float a_radius, float a_scale) noexcept
	{
		const float scale = a_scale > 0.0f ? a_scale : 1.0f;
		if (a_radius <= 0.0f || a_radius > kFlameEnclosureMaxRadius * scale)
			return false;
		const float reach = a_radius + kFlameEnclosureReachMargin * scale;
		const float horizontal = a_dx * a_dx + a_dy * a_dy;
		if (horizontal + a_dz * a_dz <= reach * reach)
			return true;
		return a_dz < 0.0f && horizontal <= reach * reach && -a_dz - a_radius <= kFlameAboveHolderMax * scale;
	}

	/// Buckets a stall length for ShadowScheduler's stallHistogram: 0, 1-2,
	/// 3-7, 8-15, 16-44, 45+ (top boundary matches kSleepRedrawIntervalFrames).
	inline uint32_t StallBucket(uint32_t v) noexcept
	{
		if (v == 0)
			return 0;
		if (v <= 2)
			return 1;
		if (v <= 7)
			return 2;
		if (v <= 15)
			return 3;
		if (v <= 44)
			return 4;
		return 5;
	}

	/// Buckets a streak length for ShadowScheduler's demand-streak histograms:
	/// 0, 1-2, 3-7, 8-15, 16-31, 32-63, 64-127, 128-255, 256+.
	inline uint32_t DemandStreakBucket(uint32_t v) noexcept
	{
		if (v == 0)
			return 0;
		if (v <= 2)
			return 1;
		if (v <= 7)
			return 2;
		if (v <= 15)
			return 3;
		if (v <= 31)
			return 4;
		if (v <= 63)
			return 5;
		if (v <= 127)
			return 6;
		if (v <= 255)
			return 7;
		return 8;
	}

	// Rejects heap garbage an accumulator slot can hold between our prepass and
	// the engine's read: below the low 64 KiB null-guard region (near-null like
	// 0x8 would AV), outside the x64 canonical range, or not 8-byte aligned
	// (BSShadowLight is pointer-aligned). Callers stop at the first implausible entry.
	inline bool IsPlausibleShadowLightPtr(std::uintptr_t raw) noexcept
	{
		return raw >= 0x10000ull && raw < 0x0000800000000000ull && (raw & 0x7) == 0;
	}

	/// How far a challenger must out-score a light already holding a shadow slot.
	inline constexpr double kSlotHoldMargin = 1.25;

	/// Fraction of one screen axis covered by a light's clip-space bounds.
	inline float ProjectedLightExtent(float center, float extent, float nearestW, float farthestW) noexcept
	{
		if (farthestW <= 0.0f)
			return 0.0f;
		const float low = center - extent;
		const float high = center + extent;
		const float left = low >= 0.0f ? low / farthestW : (nearestW > 0.0f ? low / nearestW : -1.0f);
		const float right = high <= 0.0f ? high / farthestW : (nearestW > 0.0f ? high / nearestW : 1.0f);
		return (std::clamp(right, -1.0f, 1.0f) - std::clamp(left, -1.0f, 1.0f)) * 0.5f;
	}

	/// A light's rank score once its slot is counted. A non-positive score stays
	/// as it is: scaling it would rank the holder lower.
	inline double SlotHeldScore(double a_score, bool a_holdsSlot) noexcept
	{
		return a_holdsSlot && a_score > 0.0 ? a_score * kSlotHoldMargin : a_score;
	}

	/// Whether a light the engine culled may stay a visible shadow light while its sphere is still in view: an omni light that
	/// passed the engine's test within the exit streak and is neither hidden nor faded out by its level of detail.
	inline bool EdgeKeepsLight(bool a_recentlyValid, bool a_hidden, bool a_lodFaded, bool a_frustumLight) noexcept
	{
		return a_recentlyValid && !a_hidden && !a_lodFaded && !a_frustumLight;
	}

	/// Whether a light acquiring a slot keeps its last-valid frame: one stamped this frame is its own engine pass or edge
	/// keep, an older one may belong to a recycled pointer.
	inline bool KeepsLastValidOnAcquire(int32_t a_lastValidFrame, int32_t a_nowFrame) noexcept
	{
		return a_lastValidFrame == a_nowFrame;
	}

	/// Share of the live fade a light's score fade takes each frame: a flicker averages out over about twenty frames.
	inline constexpr float kScoreFadeSmoothing = 0.05f;

	/// Fade at or below which a light counts as out, so its score follows the live fade at once going out and coming back on.
	inline constexpr float kScoreFadeOutFloor = 0.05f;

	/// The fade a light is ranked with, from its running average a_average and its live fade a_live.
	inline float SteadyScoreFade(float a_average, float a_live) noexcept
	{
		return a_live <= kScoreFadeOutFloor || a_average <= kScoreFadeOutFloor ? a_live : a_average + kScoreFadeSmoothing * (a_live - a_average);
	}

	/// Whether a light kept by the camera hold is seated only after every light still in view.
	inline bool HeldLightYields(bool a_ownInView, bool a_playerProtected, bool a_hidden) noexcept
	{
		return !a_ownInView && !a_playerProtected && !a_hidden;
	}

	/// How many lights reaching the player keep their protection: half the shadow light count, at least one.
	inline int PlayerProtectionCap(int a_shadowLightCount) noexcept
	{
		return std::max(1, a_shadowLightCount / 2);
	}

	// Casters reaching the equatorial plane keep the engine's hemisphere choice;
	// the margin floor keeps a zero-radius light from collapsing the seam.
	inline constexpr float kHemisphereSeamMarginMin = 1.0f;
	inline constexpr float kHemisphereSeamMarginRadius = 0.005f;

	/// Seam margin for SphereWhollyBehindPlane at a light of this radius.
	inline float HemisphereSeamMargin(float a_lightRadius) noexcept
	{
		return std::max(kHemisphereSeamMarginMin, kHemisphereSeamMarginRadius * a_lightRadius);
	}

	/// True when a bound sphere lies wholly behind the light's equatorial plane by
	/// more than `a_margin`; `a_planeDistance` is the sphere centre's signed
	/// distance along the front-hemisphere axis (positive in front), so a true
	/// result means no part of the sphere falls in the front paraboloid map.
	inline bool SphereWhollyBehindPlane(float a_planeDistance, float a_radius, float a_margin) noexcept
	{
		return a_planeDistance + a_radius <= -a_margin;
	}

	/// The moving-until epoch of an object none of whose parts has moved since it was seen.
	inline constexpr int kNotMoving = std::numeric_limits<int>::min();

	/// Extends an object's moving-until epoch to cover a part that moved at a_moveEpoch and stays live for a_partWindow epochs.
	inline int ObjectMovingUntil(int a_until, int a_moveEpoch, int a_partWindow) noexcept
	{
		return std::max(a_until, a_moveEpoch + a_partWindow);
	}

	/// An object's moving-until epoch. A move noted in an epoch is staged and read from the next one, so every part of the
	/// object sees the same value in an epoch whatever order the parts are visited.
	struct ObjectMotion
	{
		int until = kNotMoving;
		int staged = kNotMoving;
		int stagedEpoch = -1;
	};

	inline int MovingUntil(const ObjectMotion& a_motion, int a_epoch) noexcept
	{
		return a_motion.stagedEpoch < a_epoch ? std::max(a_motion.until, a_motion.staged) : a_motion.until;
	}

	inline void NoteObjectMove(ObjectMotion& a_motion, int a_epoch, int a_partWindow) noexcept
	{
		if (a_motion.stagedEpoch < a_epoch) {
			a_motion.until = std::max(a_motion.until, a_motion.staged);
			a_motion.staged = kNotMoving;
		}
		a_motion.staged = ObjectMovingUntil(a_motion.staged, a_epoch, a_partWindow);
		a_motion.stagedEpoch = a_epoch;
	}

	/// Farthest a flicker or pulse movement puts a light from its parent node, per unit of the Light record's movement amplitude.
	inline constexpr float kFlickerReachPerAmplitude = 1.125f;

	/// True when a light's local translate is within reach of its flicker movement, so its parent node holds its rest position.
	inline bool WithinFlickerReach(float a_x, float a_y, float a_z, float a_amplitude) noexcept
	{
		const float reach = a_amplitude * kFlickerReachPerAmplitude;
		return a_amplitude > 0.0f && a_x * a_x + a_y * a_y + a_z * a_z <= reach * reach;
	}

	/// Share of its brightest radius a flicker or pulse can take from an inverse-square light, whose radius follows the square
	/// root of a fade the flicker scales down by up to the Light record's intensity amplitude.
	inline float FlickerRadiusBand(float a_intensityAmplitude) noexcept
	{
		return 1.0f - std::sqrt(1.0f - std::clamp(a_intensityAmplitude, 0.0f, 0.99f));
	}

	/// The radius to hash for a light flickering within a_band of its brightest: a_held while every radius that flicker reaches
	/// stays within that band of it, else the live radius.
	inline float HeldFlickerRadius(float a_held, float a_live, float a_band) noexcept
	{
		const float keep = 1.0f - a_band;
		return a_live >= a_held * keep - 1.0f && a_live * keep <= a_held + 1.0f ? a_held : a_live;
	}

	/// Largest radius difference a flicker lets a cached bake serve; past it a bake made at a dim moment would miss the casters
	/// a bright moment lights.
	inline constexpr float kMaxFlickerRadiusTolerance = 0.25f;

	/// Share a cached bake's radius may differ from the live radius by, when both stay within a_band of the brightest radius.
	inline float FlickerRadiusTolerance(float a_band, float a_floor) noexcept
	{
		return std::max(a_floor, std::min(a_band / (1.0f - a_band), kMaxFlickerRadiusTolerance));
	}

	// The tile class ladder, shared by the coverage classifier below and the
	// atlas allocator's order mapping (the floor class == one allocator cell).
	inline constexpr float kTileScaleFull = 1.0f;
	inline constexpr float kTileScaleHalf = 0.5f;
	inline constexpr float kTileScaleQuarter = 0.25f;
	inline constexpr float kTileScaleEighth = 0.125f;
	inline constexpr float kTileScaleSixteenth = 0.0625f;
	inline constexpr float kTileScaleFloor = kTileScaleSixteenth;

	// Shadow texels per unit of projected light size. sizeProxy 1.0 (a light
	// dominating the view, or sitting on the viewer) maps past the full
	// class at any sane base resolution; calibration knob.
	inline constexpr float kShadowSizeToTexels = 3072.0f;

	// Demotion evaluates with the size inflated by this factor, so a light
	// hovering at a class boundary doesn't flip classes (a class change
	// invalidates the cached shadow map and forces a redraw).
	inline constexpr float kDemoteHeadroom = 1.4f;

	/// Smallest ladder class whose resolution still meets the target texel
	/// count for a light of the given projected size.
	inline float TileScaleTarget(float sizeProxy, float baseTileTexels) noexcept
	{
		const float targetTexels = sizeProxy * kShadowSizeToTexels;
		float scale = kTileScaleFull;
		while (scale > kTileScaleFloor && baseTileTexels * scale * 0.5f >= targetTexels)
			scale *= 0.5f;
		return scale;
	}

	// Promotion is immediate (quality first); demotion only when even the
	// headroom-inflated size stays below the current class.
	inline float TileScaleForCoverage(float sizeProxy, float baseTileTexels, float currentScale) noexcept
	{
		const float target = TileScaleTarget(sizeProxy, baseTileTexels);
		if (target >= currentScale)
			return target;
		return (TileScaleTarget(sizeProxy * kDemoteHeadroom, baseTileTexels) < currentScale) ? target : currentScale;
	}

	// Percentile (default 90th) of the last min(count, Window) samples in
	// `ring`. Returns the 60fps fallback (16.67 ms) when count <= 0, which
	// also guards std::copy/std::nth_element against a negative n.
	template <int Window>
	inline float FrameTimePercentile(const float (&ring)[Window], int count, float percentile = 0.9f)
	{
		if (count <= 0)
			return 16.67f;
		const int n = std::min(count, Window);
		float tmp[Window];
		std::copy(ring, ring + n, tmp);
		const int idx = std::clamp(static_cast<int>(n * percentile), 0, n - 1);
		std::nth_element(tmp, tmp + idx, tmp + n);
		return tmp[idx];
	}

	inline constexpr int32_t kVanillaShadowSliceCount = 8;

	/// Slices the engine array needs without the atlas: exteriors keep the sun at pool index 0,
	/// so point lights index slices 1..count, and the engine never allocates fewer than 8.
	inline constexpr int32_t EngineShadowSliceCount(int32_t a_shadowLightCount) noexcept
	{
		return std::max(kVanillaShadowSliceCount, a_shadowLightCount + 1);
	}

	/// Point lights an array of a_slices slices can hold beside the sun's pool index 0.
	inline constexpr int32_t SliceCappedLightCount(int32_t a_shadowLightCount, int32_t a_slices) noexcept
	{
		return std::min(a_shadowLightCount, std::max(a_slices - 1, 0));
	}

	/// Rotation whose X column, a paraboloid shadow camera's view axis, is the direction given, kept upright; false under one unit.
	inline bool FacingRotation(float a_x, float a_y, float a_z, float (&a_out)[3][3]) noexcept
	{
		const float length = std::sqrt(a_x * a_x + a_y * a_y + a_z * a_z);
		if (!(length >= 1.0f))
			return false;
		const float view[3]{ a_x / length, a_y / length, a_z / length };
		const bool vertical = std::abs(view[2]) > 0.99f;
		const float up[3]{ 0.0f, vertical ? 1.0f : 0.0f, vertical ? 0.0f : 1.0f };
		float side[3]{ view[1] * up[2] - view[2] * up[1], view[2] * up[0] - view[0] * up[2], view[0] * up[1] - view[1] * up[0] };
		const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
		for (float& v : side)
			v /= sideLength;
		const float upright[3]{ side[1] * view[2] - side[2] * view[1], side[2] * view[0] - side[0] * view[2], side[0] * view[1] - side[1] * view[0] };
		for (int i = 0; i < 3; ++i) {
			a_out[i][0] = view[i];
			a_out[i][1] = upright[i];
			a_out[i][2] = side[i];
		}
		return true;
	}

	/// Values worked out once per key per frame; the first lookup of a later frame drops every entry.
	/// A flat list: it keeps its capacity across frames, so a steady frame allocates nothing.
	template <class Key, class Value>
	struct PerFrameCache
	{
		std::uint32_t frame{ UINT32_MAX };
		std::vector<std::pair<Key, Value>> entries;

		/// The entry for a_key in a_frame, made by a_make on its first lookup that frame.
		template <class Make>
		Value& Get(std::uint32_t a_frame, const Key& a_key, Make&& a_make)
		{
			if (frame != a_frame) {
				entries.clear();
				frame = a_frame;
			}
			for (auto& [key, value] : entries)
				if (key == a_key)
					return value;
			return entries.emplace_back(a_key, a_make()).second;
		}
	};
}
