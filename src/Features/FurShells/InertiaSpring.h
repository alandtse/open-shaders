#pragma once

#include "Utils/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FurInertia
{
	/** @brief Fur length and droop the stylized preset is normalised to, so fur with these values behaves exactly like the preset's base values. */
	inline constexpr float ReferenceLength = 1.2f;
	inline constexpr float ReferenceDroop = 0.15f;
	/** @brief Time constant of the low-pass on the mesh velocity, in seconds; hides hand-tracking tremor. */
	inline constexpr float VelocitySmoothing = 0.04f;
	/** @brief Frame time above which the update is clamped, in seconds. */
	inline constexpr float MaxFrameSeconds = 1.0f / 20.0f;
	/** @brief Movement in one frame, in game units, treated as a teleport that resets the spring. */
	inline constexpr float TeleportDistance = 300.0f;
	/** @brief Spring frequency limits, in hertz. */
	inline constexpr float MinFrequencyHz = 0.5f;
	inline constexpr float MaxFrequencyHz = 400.0f;
	/** @brief Mesh scales outside this range are treated as a bad value. */
	inline constexpr float MinWorldScale = 0.1f;
	inline constexpr float MaxWorldScale = 10.0f;
	/** @brief Ranges the settings and their sliders accept. */
	inline constexpr float MaxStrength = 2.0f;
	inline constexpr float MaxTuningFrequencyHz = 100.0f;
	inline constexpr float MinGain = 0.1f;
	inline constexpr float MaxGain = 4.0f;
	inline constexpr float MinBendLimit = 0.05f;
	inline constexpr float MaxBendLimit = 2.0f;
	inline constexpr float MaxLengthExponent = 2.5f;
	inline constexpr float MaxDroopExponent = 1.5f;

	enum class Preset : uint32_t
	{
		Stylized,
		Realistic,
		Custom
	};

	/** @brief How the spring responds, before it is adjusted for one fur asset. */
	struct Tuning
	{
		/** @brief Natural frequency, in hertz, of fur at ReferenceLength and ReferenceDroop. */
		float frequencyHz = 3.82f;
		/** @brief Scales the mesh acceleration into the lag of the fur tips. */
		float gain = 2.0f;
		/** @brief How steeply the frequency falls as the fur gets longer: 0.5 is gentle, 2 follows a real hair. */
		float lengthExponent = 0.5f;
		/** @brief How much droop softens the fur: 0 ignores it. */
		float droopExponent = 0.5f;
		/** @brief Largest tip lag as a fraction of the fur length. */
		float maxBend = 1.0f;
	};

	/** @brief Spring constants for one fur asset. */
	struct Response
	{
		float omega = 24.0f;
		float gain = 2.0f;
	};

	/** @brief Brings a tuning read from a settings file into range, replacing values that are not finite with the Stylized ones. */
	[[nodiscard]] inline Tuning Sanitize(const Tuning& a_tuning)
	{
		const Tuning stylized{};
		return Tuning{
			Util::ClampFinite(a_tuning.frequencyHz, MinFrequencyHz, MaxTuningFrequencyHz, stylized.frequencyHz),
			Util::ClampFinite(a_tuning.gain, MinGain, MaxGain, stylized.gain),
			Util::ClampFinite(a_tuning.lengthExponent, 0.0f, MaxLengthExponent, stylized.lengthExponent),
			Util::ClampFinite(a_tuning.droopExponent, 0.0f, MaxDroopExponent, stylized.droopExponent),
			Util::ClampFinite(a_tuning.maxBend, MinBendLimit, MaxBendLimit, stylized.maxBend)
		};
	}

	/**
	 * @brief The tuning a preset stands for.
	 * @details Stylized is the look chosen by eye. Realistic uses a cantilever hair of about 75 micrometres and 4 GPa, which rings near 30 Hz at 2.5 cm, falls with the square of the length, ignores droop, and applies the acceleration unscaled.
	 * @param a_custom Used when @p a_preset is Custom.
	 */
	[[nodiscard]] inline Tuning GetTuning(Preset a_preset, const Tuning& a_custom)
	{
		switch (a_preset) {
		case Preset::Stylized:
			return Tuning{};
		case Preset::Realistic:
			return Tuning{ 63.8f, 1.0f, 2.0f, 0.0f, 1.0f };
		default:
			return a_custom;
		}
	}

	/**
	 * @brief Adjusts a tuning for the fur of one asset: longer and droopier fur rings slower and lags more.
	 * @param a_length Fur length in game units.
	 * @param a_droop Fur droop in game units.
	 */
	[[nodiscard]] inline Response GetResponse(const Tuning& a_tuning, float a_length, float a_droop)
	{
		const float lengthRatio = std::max(a_length, 0.01f) / ReferenceLength;
		const float droopRatio = (1.0f + std::max(a_droop, 0.0f)) / (1.0f + ReferenceDroop);
		const float frequency = a_tuning.frequencyHz * std::pow(lengthRatio, -a_tuning.lengthExponent) * std::pow(droopRatio, -a_tuning.droopExponent);
		constexpr float twoPi = 6.2831853f;
		return { twoPi * std::clamp(frequency, MinFrequencyHz, MaxFrequencyHz), a_tuning.gain };
	}

	/** @brief Fur length in world game units for a mesh at @p a_scale; the scale is bounded so a bad value cannot disable or explode the spring. */
	[[nodiscard]] inline float GetWorldLength(float a_length, float a_scale)
	{
		return a_length * std::clamp(a_scale, MinWorldScale, MaxWorldScale);
	}

	/** @brief Lags the fur tips behind the acceleration of one mesh with a critically damped spring. */
	struct Spring
	{
		float3 previousPosition;
		float3 baseVelocity;
		float3 offset;
		float3 velocity;

		/** @brief A resting spring for a mesh currently at @p a_position. */
		static Spring At(const float3& a_position)
		{
			Spring spring;
			spring.previousPosition = a_position;
			return spring;
		}

		/**
		 * @brief Advances the spring by one frame, solving it exactly so any frequency stays stable at any frame rate.
		 * @param a_position World position of the mesh now.
		 * @param a_deltaTime Frame time in seconds, clamped to MaxFrameSeconds; zero leaves the spring unchanged.
		 * @param a_maxOffset Longest tip lag allowed, in game units.
		 * @param a_response Spring constants for the fur being drawn.
		 */
		void Step(const float3& a_position, float a_deltaTime, float a_maxOffset, const Response& a_response)
		{
			const float3 moved = a_position - previousPosition;
			const float deltaTime = std::clamp(a_deltaTime, 0.0f, MaxFrameSeconds);
			previousPosition = a_position;
			if (moved.Length() > TeleportDistance) {
				*this = At(a_position);
				return;
			}
			if (deltaTime <= 0.0f)
				return;

			const float blend = 1.0f - std::exp(-deltaTime / VelocitySmoothing);
			const float3 smoothedVelocity = baseVelocity + (moved / deltaTime - baseVelocity) * blend;
			const float3 baseAcceleration = (smoothedVelocity - baseVelocity) / deltaTime;
			baseVelocity = smoothedVelocity;

			const float omega = a_response.omega;
			const float3 target = baseAcceleration * (-a_response.gain / (omega * omega));
			const float3 deviation = offset - target;
			const float3 decayed = velocity + deviation * omega;
			const float fade = std::exp(-omega * deltaTime);
			offset = target + (deviation + decayed * deltaTime) * fade;
			velocity = (velocity - decayed * (omega * deltaTime)) * fade;

			const float length = offset.Length();
			if (length > a_maxOffset) {
				offset *= a_maxOffset / length;
				velocity = float3{};
			}
		}
	};
}
