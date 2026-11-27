#pragma once

#include "EngineFix.h"

/** @brief Keeps region-driven light colors valid throughout a sky weather transition. */
struct RegionLightEmittanceFix : EngineFix
{
	std::string GetName() override { return "Region Light Emittance Fix"; }
	void Install() override;
};
