#pragma once

#include <cstdint>

enum class PBRFlags : uint32_t
{
	Subsurface = 1 << 0,
	TwoLayer = 1 << 1,
	ColoredCoat = 1 << 2,
	InterlayerParallax = 1 << 3,
	CoatNormal = 1 << 4,
	Fuzz = 1 << 5,
	HairMarschner = 1 << 6,
	Foliage = 1 << 7,
	ThinSurface = 1 << 8,
};

enum class PBRShaderFlags : uint32_t
{
	HasEmissive = 1 << 0,
	HasDisplacement = 1 << 1,
	HasFeaturesTexture0 = 1 << 2,
	HasFeaturesTexture1 = 1 << 3,
	Subsurface = 1 << 4,
	TwoLayer = 1 << 5,
	ColoredCoat = 1 << 6,
	InterlayerParallax = 1 << 7,
	CoatNormal = 1 << 8,
	Fuzz = 1 << 9,
	HairMarschner = 1 << 10,
	Glint = 1 << 11,
	ProjectedGlint = 1 << 12,
	Foliage = 1 << 13,
	ThinSurface = 1 << 14,
	HasFuzzRoughnessTexture = 1 << 15,
};
