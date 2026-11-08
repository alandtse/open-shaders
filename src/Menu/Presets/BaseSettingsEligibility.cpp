#include "Menu/Presets/BaseSettingsEditor.h"

#include "Feature.h"

#include <cassert>
#include <unordered_map>

bool BaseSettingsEditor::HasExportableSettings(Feature* feature)
{
	assert(feature);
	// The settings schema is fixed per feature, so one SaveSettings probe is enough.
	static std::unordered_map<Feature*, bool> cache;
	auto [it, inserted] = cache.try_emplace(feature, false);
	if (inserted && feature->UsesMainSettings()) {
		json settings;
		feature->SaveSettings(settings);
		it->second = settings.is_object() && !settings.empty();
	}
	return it->second;
}
