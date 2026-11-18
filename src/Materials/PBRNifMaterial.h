#pragma once

#include "PBRFlags.h"

namespace PBRRecords
{
	struct NifSurfaceFlags
	{
		bool multiLayerParallax = false;
		bool backLighting = false;
		bool effectLighting = false;
		bool anisotropicLighting = false;
		bool rimLighting = false;
		bool softLighting = false;
		bool fitSlope = false;
	};

	struct NifMaterialInterpretation
	{
		uint32_t pbrFlags = 0;
		uint32_t thinSurfaceFlags = 0;
		bool applyThinSurfaceFlags = false;
		bool enableGlint = false;
	};

	/** @brief Interpret existing Skyrim NIF flag combinations without changing engine state. */
	[[nodiscard]] constexpr NifMaterialInterpretation InterpretNifSurface(const NifSurfaceFlags& a_flags)
	{
		NifMaterialInterpretation result;
		if (a_flags.multiLayerParallax) {
			result.pbrFlags |= static_cast<uint32_t>(PBRFlags::TwoLayer);
			if (a_flags.softLighting)
				result.pbrFlags |= static_cast<uint32_t>(PBRFlags::InterlayerParallax);
			if (a_flags.backLighting)
				result.pbrFlags |= static_cast<uint32_t>(PBRFlags::CoatNormal);
			if (a_flags.effectLighting)
				result.pbrFlags |= static_cast<uint32_t>(PBRFlags::ColoredCoat);
		} else if (a_flags.backLighting) {
			result.pbrFlags |= static_cast<uint32_t>(PBRFlags::HairMarschner);
		} else {
			result.applyThinSurfaceFlags = true;
			if (a_flags.anisotropicLighting)
				result.thinSurfaceFlags |= static_cast<uint32_t>(PBRFlags::ThinSurface);
			if (a_flags.effectLighting)
				result.thinSurfaceFlags |= static_cast<uint32_t>(PBRFlags::Foliage);
			if (a_flags.rimLighting)
				result.pbrFlags |= static_cast<uint32_t>(PBRFlags::Subsurface);
			if (a_flags.softLighting)
				result.pbrFlags |= static_cast<uint32_t>(PBRFlags::Fuzz);
			else if (a_flags.fitSlope)
				result.enableGlint = true;
		}
		return result;
	}
}
