#pragma once

#include <nlohmann/json.hpp>

#include <string>

/** @brief A settings change a feature offers to the settings benchmark, in the shape the feature saves its settings. */
struct BenchmarkVariant
{
	std::string id;
	std::string label;
	std::string group;
	std::string note;
	nlohmann::json settings;  ///< Partial settings of this feature only.
	bool vrOnly = false;      ///< Changes a setting that only exists or only matters in VR.
};
