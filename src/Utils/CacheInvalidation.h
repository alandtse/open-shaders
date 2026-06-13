#pragma once

// Disk shader-cache invalidation logic, kept free of game/SKSE dependencies so
// tests/cpp can exercise it directly (see test_cacheinvalidation.cpp). Policy:
// every unknown or failed path degrades to "invalidate more", never less --
// serving a blob compiled under a different feature set is silent corruption
// (feature defines change every shader's bytecode; cache paths don't encode them).

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace Util::CacheInvalidation
{
	/// One disk-cache/runtime state divergence.
	struct CacheMismatch
	{
		enum class Kind
		{
			PluginVersion,   ///< plugin updated (or no Info.ini) — expected, rebuild silently
			FeatureVersion,  ///< feature updated — expected, rebuild (partially when possible)
			EnabledFlip,     ///< feature set changed — likely unintentional, hold + prompt
		};
		Kind kind;
		std::string shortName;    ///< stable key ("Plugin" for plugin-version entries)
		std::string feature;      ///< display name for UI
		std::string detail;       ///< English direction of the mismatch (logs + fallback)
		bool nowPresent = false;  ///< EnabledFlip only: feature is loaded now (added) vs not (removed); lets the UI localize the direction
	};

	/// Runtime-side view of a feature, decoupled from the Feature class.
	struct FeatureState
	{
		std::string shortName;
		std::string name;
		bool loaded = false;
		std::string version;
		std::string define;  ///< global shader define; empty => unknown reach (full wipe)
	};

	/// Parsed Info.ini feature section.
	struct CacheIniEntry
	{
		bool enabled = false;
		std::optional<std::string> version;
	};

	/// Compare the cache manifest against current state. Pure function of its inputs.
	inline std::vector<CacheMismatch> ClassifyMismatches(
		const std::string& currentPluginVersion,
		const std::optional<std::string>& cachedPluginVersion,
		const std::vector<FeatureState>& features,
		const std::map<std::string, CacheIniEntry>& cacheEntries)
	{
		std::vector<CacheMismatch> mismatches;
		if (!cachedPluginVersion) {
			mismatches.push_back({ CacheMismatch::Kind::PluginVersion, "Plugin", "Plugin", "no plugin version found in cache" });
		} else if (*cachedPluginVersion != currentPluginVersion) {
			mismatches.push_back({ CacheMismatch::Kind::PluginVersion, "Plugin", "Plugin",
				std::format("version changed (current: {}, cached: {})", currentPluginVersion, *cachedPluginVersion) });
		}
		for (const auto& feature : features) {
			const auto it = cacheEntries.find(feature.shortName);
			const bool enabledInCache = it != cacheEntries.end() && it->second.enabled;
			if (enabledInCache != feature.loaded) {
				mismatches.push_back({ CacheMismatch::Kind::EnabledFlip, feature.shortName, feature.name,
					feature.loaded ?
						"installed/enabled now, but the cache was built without it" :
						"the cache was built with it, but it is now uninstalled or disabled at boot",
					feature.loaded });
				continue;
			}
			if (feature.loaded) {
				const auto& cachedVersion = it->second.version;
				if (!cachedVersion || *cachedVersion != feature.version) {
					mismatches.push_back({ CacheMismatch::Kind::FeatureVersion, feature.shortName, feature.name,
						std::format("version changed (installed: {}, cached: {})", feature.version,
							cachedVersion ? *cachedVersion : "<none>") });
				}
			}
		}
		return mismatches;
	}

	/// Scan a root shader's include closure for a token. nullopt on any IO failure
	/// so callers fall back to the conservative full wipe.
	inline std::optional<bool> RootShaderReferencesToken(
		const std::filesystem::path& root, const std::string& token, const std::filesystem::path& shadersRoot)
	{
		try {
			static const std::regex includeRe(R"#(^\s*#\s*include\s+"([^"]+)")#");
			std::set<std::filesystem::path> visited;
			std::vector<std::filesystem::path> queue{ root };
			while (!queue.empty()) {
				// Normalize so relative spellings (Sub/../A.hlsli) can't defeat the
				// visited set and spin on a cycle.
				auto file = queue.back().lexically_normal();
				queue.pop_back();
				if (!visited.insert(file).second)
					continue;
				std::ifstream stream(file);
				if (!stream)
					return std::nullopt;
				std::string line;
				while (std::getline(stream, line)) {
					// Identifier-boundary match: UNIFIED_WATER must not hit UNIFIED_WATERX.
					for (size_t pos = line.find(token); pos != std::string::npos; pos = line.find(token, pos + 1)) {
						const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
						const bool beforeOk = pos == 0 || !isIdent(line[pos - 1]);
						const bool afterOk = pos + token.size() >= line.size() || !isIdent(line[pos + token.size()]);
						if (beforeOk && afterOk)
							return true;
					}
					std::smatch m;
					if (std::regex_search(line, m, includeRe)) {
						// Includes resolve against the merged root, then the includer's dir.
						auto byRoot = shadersRoot / m[1].str();
						auto byLocal = file.parent_path() / m[1].str();
						if (std::filesystem::exists(byRoot))
							queue.push_back(byRoot);
						else if (std::filesystem::exists(byLocal))
							queue.push_back(byLocal);
						// Unresolvable includes are permutation-gated feature paths; the
						// token test above already covers the gating define on this line.
					}
				}
			}
			return false;
		} catch (...) {
			return std::nullopt;
		}
	}

	/// Delete only the cache dirs whose root shader references any of the defines.
	/// Returns false (caller must full-wipe) on any empty define, missing root
	/// source, or scan failure -- conservative by construction.
	inline bool TryPartialInvalidation(
		const std::filesystem::path& cacheRoot, const std::filesystem::path& shadersRoot,
		const std::vector<std::string>& defines, size_t* outDeleted = nullptr, size_t* outKept = nullptr)
	{
		try {
			for (const auto& define : defines)
				if (define.empty())
					return false;
			size_t deleted = 0, kept = 0;
			for (const auto& entry : std::filesystem::directory_iterator(cacheRoot)) {
				if (!entry.is_directory())
					continue;
				const auto root = shadersRoot / (entry.path().filename().wstring() + L".hlsl");
				if (!std::filesystem::exists(root))
					return false;
				bool affected = false;
				for (const auto& define : defines) {
					auto refs = RootShaderReferencesToken(root, define, shadersRoot);
					if (!refs.has_value())
						return false;
					if (*refs) {
						affected = true;
						break;
					}
				}
				if (affected) {
					std::filesystem::remove_all(entry.path());
					++deleted;
				} else {
					++kept;
				}
			}
			if (outDeleted)
				*outDeleted = deleted;
			if (outKept)
				*outKept = kept;
			return true;
		} catch (...) {
			return false;
		}
	}
}
