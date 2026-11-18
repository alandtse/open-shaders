#include "PBRRecordConfig.h"

#include "Util.h"

void PBRRecords::ReadConfigs(const std::string& a_rootPath,
	std::function<bool(const std::string&, const json&)> a_recordReader,
	bool a_verbose, const char* a_logOwner)
{
	if (std::filesystem::exists(a_rootPath)) {
		auto configs = clib_util::distribution::get_configs(a_rootPath, "", ".json");

		if (configs.empty()) {
			logger::warn("[{}] no .json files were found within the {} folder, aborting...", a_logOwner, a_rootPath);
			return;
		}

		if (a_verbose) {
			logger::info("[{}] {} matching jsons found", a_logOwner, configs.size());
		}

		bool anyFailed = false;
		for (auto& path : configs) {
			if (a_verbose) {
				logger::info("[{}] loading json : {}", a_logOwner, path);
			}

			std::string error;
			const auto config = Util::FileHelpers::ReadJsonFile(path, "PBR record config", 0, &error);
			if (!config) {
				if (error.empty())
					logger::error("[{}] failed to read {}", a_logOwner, path);
				anyFailed = true;
				continue;
			}

			const auto editorId = std::filesystem::path(path).stem().string();
			if (!a_recordReader(editorId, *config)) {
				anyFailed = true;
			}
		}

		if (!a_verbose && !anyFailed) {
			logger::info("[{}] All .jsons loaded successfully.", a_logOwner);
		}
	}
}

void PBRRecords::SaveConfig(const std::string& a_rootPath, const std::string& a_editorId, const json& a_config)
{
	Util::FileHelpers::WriteJsonAtomically(std::filesystem::path(a_rootPath) / (a_editorId + ".json"), a_config, 4, "PBR record config");
}
