#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for inspect extension featureissues. {brand} is expanded at registration. */
	inline constexpr const char* kInspectFeatureIssuesExt =
		R"({"description":")"
		R"({brand} feature-issue tracking -> {featureIssues:[{shortName,displayName,version,issueType,rejectionReason,replacementFeature,replacementFeatureDisplayName,replacementFeatureInstalled,)"
		R"(replacementFeatureModLink,userMessage,minimumVersionRequired,modifiedShaderDirectory,iniPath,removedInVersion}],hasIssues,hasObsoleteShaderModifyingFeatures,hasPotentialShaderModifyingFeatures}. )"
		R"(Surfaces the same detection FeatureIssues.h already does at boot for the in-menu warning banner -- obsolete features with a known replacement, INI version mismatches, )"
		R"(override files that failed to apply, )"
		R"(and unrecognized/unknown feature INIs left in Data/Shaders/Features. issueType: OBSOLETE|VERSION_MISMATCH|OVERRIDE_FAILED|UNKNOWN. modifiedShaderDirectory/hasObsoleteShaderModifyingFeatures flag features whose obsolete files could still be holding stale shader source -- the more likely of the two shader-modifying flags to explain an otherwise-unexplained compile problem elsewhere.")"
		R"(,"readOnly":true)"
		R"(,"inputSchema":{"type":"object"})"
		R"(})";
}
