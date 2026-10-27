#pragma once

#include "EngineFix.h"

/**
 * @brief Searches a geometry's light list with AVX2 before BSShaderPropertyLightData::AddLight runs.
 *
 * Light attachment calls AddLight once per light and geometry pair, and nearly every call finds the light already listed,
 * so the engine's scalar search over a list that can hold hundreds of lights dominates scenes with many lights.
 * A light that is not listed, and every call made while the engine rebuilds the list in order, still runs the engine routine.
 */
struct LightListSearchFix : EngineFix
{
	std::string GetName() override { return "Light List Search Fix"; }

	void Install() override;
};
