#pragma once

#include <functional>
#include <string>

namespace PBRRecords
{
	/** @brief Visit PBR JSON recipes indexed by their record editor IDs. */
	void ReadConfigs(const std::string& a_rootPath,
		std::function<bool(const std::string&, const json&)> a_recordReader,
		bool a_verbose, const char* a_logOwner = "TruePBR");
	/** @brief Persist an edited PBR JSON recipe in the existing asset format. */
	void SaveConfig(const std::string& a_rootPath, const std::string& a_editorId, const json& a_config);
}
