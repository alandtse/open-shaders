#pragma once

#include "FramePlan.h"
#include "ProxyContract.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NR
{
	/** @brief Appearance controls for the full-resolution Feature 18 pass. */
	struct Tuning
	{
		static constexpr float kMinStrength = 0.0f, kMaxStrength = 2.0f;
		static constexpr float kDefaultStrength = 1.0f, kAutomaticSkinStructure = -1.0f;
		static constexpr uint32_t kMaxStyle = 2;
		uint32_t style = 0;
		float intensity = kDefaultStrength;
		float localToneStrength = kDefaultStrength;
		float localStructureStrength = kDefaultStrength;
		float skinStructureStrength = kAutomaticSkinStructure;
		bool useAutoMask = true;

		/**
		 * @brief Creation-flag pairing Feature 18 is driven with, as a ProxyContract value.
		 *        PR 723's tested pairing is the default; the other arm only exists until the
		 *        proxy-contract A/B picks a winner. Not persisted, so it resets on every launch.
		 */
		uint32_t proxyContract = static_cast<uint32_t>(ProxyContract::kHdr);
		/**
		 * @brief Exposure arm the Prepare pass uses, as an ExposureSource value.
		 *        kScene falls back to kLocal when no loaded feature publishes an exposure (R1).
		 */
		uint32_t exposureSource = static_cast<uint32_t>(ExposureSource::kScene);

		/** @brief The pairing this tuning names, clamped so a settings edit cannot index past the enum. */
		[[nodiscard]] ProxyContract ProxyContractValue() const { return ClampProxyContract(proxyContract); }

		[[nodiscard]] ExposureSource ExposureSourceValue() const
		{
			return exposureSource < static_cast<uint32_t>(ExposureSource::kCount) ?
			           static_cast<ExposureSource>(exposureSource) :
			           ExposureSource::kScene;
		}

		/**
		 * @brief Clamps user input to the range the tested runtime accepts, so a bad slider or
		 *        settings file cannot hand Feature 18 a value outside it.
		 */
		void Sanitize()
		{
			style = std::min(style, kMaxStyle);
			for (auto* strength : { &intensity, &localToneStrength, &localStructureStrength })
				*strength = std::isfinite(*strength) ? std::clamp(*strength, kMinStrength, kMaxStrength) : kDefaultStrength;
			// Any negative value is the Auto sentinel: a value in (-1, 0) is not a strength the
			// runtime accepts, and Feature 18 reads a negative one as "no override".
			skinStructureStrength = (std::isfinite(skinStructureStrength) && skinStructureStrength >= kMinStrength) ?
			                            std::min(skinStructureStrength, kMaxStrength) :
			                            kAutomaticSkinStructure;
			proxyContract = static_cast<uint32_t>(ClampProxyContract(proxyContract));
			if (exposureSource >= static_cast<uint32_t>(ExposureSource::kCount))
				exposureSource = static_cast<uint32_t>(ExposureSource::kScene);
		}
	};
}
