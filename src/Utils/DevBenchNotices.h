#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <vector>

/**
 * @brief Decides which conditions are worth a devbench notice and keeps the posted set in sync.
 *
 * devbench attaches every posted notice to every tool reply until it is cleared, so a notice must
 * either stop an agent from trusting a result or tell it what to fix. Counters, progress and
 * anything an agent can read on demand through `inspect` stay out: text that changes every poll
 * would also re-send the whole board on each reply. Pure logic with no game or devbench
 * dependency so it is unit tested.
 */
namespace Util::DevBenchNotices
{
	/** @brief Mirrors devbench's notice severities. */
	enum class Severity
	{
		Warning,
		Critical
	};

	/** @brief The severity string devbench's PostNotice expects. */
	constexpr const char* SeverityName(Severity a_severity)
	{
		return a_severity == Severity::Critical ? "critical" : "warning";
	}

	/** @brief One notice: a stable key, so a repeat replaces rather than stacks. */
	struct Notice
	{
		std::string key;
		Severity severity = Severity::Warning;
		std::string text;

		bool operator==(const Notice&) const = default;
	};

	/** @brief Snapshot of the renderer state the notices are derived from. */
	struct Conditions
	{
		bool shadersCompiling = false;
		std::uint64_t failedShaders = 0;
		/** @brief The disk cache is held, or a feature-set change awaits accept, revert or commit. */
		bool cacheAwaitingDecision = false;
		/** @brief Display names of features rejected at load (obsolete, version mismatch, conflict, override failed). */
		std::vector<std::string> rejectedFeatures;
		/** @brief Display names of loaded features whose restart-gated settings differ from the values the game booted with. */
		std::vector<std::string> restartPendingFeatures;
	};

	inline constexpr const char* kKeyCompiling = "openshaders.shaders.compiling";
	inline constexpr const char* kKeyFailed = "openshaders.shaders.failed";
	inline constexpr const char* kKeyCache = "openshaders.shaders.cache";
	inline constexpr const char* kKeyFeatures = "openshaders.features.rejected";
	inline constexpr const char* kKeyRestart = "openshaders.settings.restart";
	inline constexpr std::size_t kMaxListedFeatures = 5;

	/** @brief a_names joined by commas, truncated to kMaxListedFeatures with a "+N more" tail. */
	inline std::string JoinNames(const std::vector<std::string>& a_names)
	{
		std::string joined;
		const std::size_t listed = (std::min)(a_names.size(), kMaxListedFeatures);
		for (std::size_t i = 0; i < listed; ++i) {
			joined += (i ? ", " : "") + a_names[i];
		}
		if (a_names.size() > listed) {
			joined += std::format(", +{} more", a_names.size() - listed);
		}
		return joined;
	}

	/** @brief The notices that should be active for a_conditions, most urgent first. */
	inline std::vector<Notice> Evaluate(const Conditions& a_conditions)
	{
		std::vector<Notice> notices;

		if (a_conditions.failedShaders > 0) {
			notices.push_back({ kKeyFailed, Severity::Critical,
				"Shader compile failures: the affected effects draw with a fallback or not at all, so visual and "
				"timing results are not valid. Read inspect kind=shadercache recentFailures for the shader and the error." });
		}

		if (a_conditions.cacheAwaitingDecision) {
			notices.push_back({ kKeyCache, Severity::Warning,
				"The shader cache is waiting on a decision after a feature-set change and is compiling in memory only. "
				"Settle it before relying on startup time or frame pacing: inspect kind=shadercache shows the pending "
				"state, and openshaders.shadercache offers acceptRebuild, restorePrevious and discardPrevious." });
		}

		if (a_conditions.shadersCompiling) {
			notices.push_back({ kKeyCompiling, Severity::Warning,
				"Shaders are compiling: frames can hitch and some draws are missing until it finishes. Do not capture, "
				"screenshot or time anything until inspect kind=shadercache reports compiling=false." });
		}

		if (!a_conditions.rejectedFeatures.empty()) {
			notices.push_back({ kKeyFeatures, Severity::Warning,
				std::format("Features rejected at load and not running: {}. A test that expects them will not see them; "
							"inspect kind=featureissues has the reason and the fix for each.",
					JoinNames(a_conditions.rejectedFeatures)) });
		}

		if (!a_conditions.restartPendingFeatures.empty()) {
			notices.push_back({ kKeyRestart, Severity::Warning,
				std::format("Settings changed since boot need a restart to take effect ({}). Until then a live toggle "
							"does nothing and an A/B of it measures noise; restart the game before testing them.",
					JoinNames(a_conditions.restartPendingFeatures)) });
		}

		return notices;
	}

	/** @brief What to send to devbench to make its board match a desired set. */
	struct Delta
	{
		std::vector<Notice> post;
		std::vector<std::string> clear;
	};

	/** @brief Remembers what is posted so an unchanged notice is not re-sent and a resolved one is cleared. */
	class Reconciler
	{
	public:
		/** @brief Diffs a_desired against what is already posted and records the result as posted. */
		Delta Update(const std::vector<Notice>& a_desired)
		{
			Delta delta;
			std::map<std::string, Notice> next;
			for (const auto& notice : a_desired) {
				next[notice.key] = notice;
				const auto it = posted.find(notice.key);
				if (it == posted.end() || !(it->second == notice)) {
					delta.post.push_back(notice);
				}
			}
			for (const auto& [key, notice] : posted) {
				if (!next.contains(key)) {
					delta.clear.push_back(key);
				}
			}
			posted = std::move(next);
			return delta;
		}

	private:
		std::map<std::string, Notice> posted;
	};
}
