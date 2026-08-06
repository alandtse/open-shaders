#include "Utils/SettingsPatch.h"

#include "Feature.h"

namespace Util::Settings
{
	bool ApplyPatch(Feature& a_feature, const json& a_patch, std::vector<std::string>& o_unknownKeys)
	{
		// WITH_DEFAULT deserialization fills a blob's absent keys from a fresh
		// default, not the live value; merge onto the saved blob first.
		json current;
		a_feature.SaveSettings(current);
		// The saved blob is the canonical shape, so reject anything it doesn't
		// name rather than deserializing it away to a silent no-op. Skipped
		// when the feature has no override to compare against.
		if (current.is_object()) {
			CollectUnknownSettingKeys(a_patch, current, "", o_unknownKeys);
			if (!o_unknownKeys.empty())
				return false;
		}
		current.merge_patch(a_patch);
		a_feature.LoadSettings(current);
		return true;
	}
}
