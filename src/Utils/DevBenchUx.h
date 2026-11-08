#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

struct Feature;

namespace Util::DevBenchUx
{
	using CommandFn = void (*)(Feature*, const json&);
	using QueryFn = json (*)(const Feature*, const json&);

	/** @brief The thread that owns a registered action's live state. */
	enum class ExecutionThread
	{
		Main,
		Render
	};

	/** @brief A registered command: its devbench-facing description and callback. */
	struct CommandEntry
	{
		std::string description;
		CommandFn fn;
		ExecutionThread executionThread = ExecutionThread::Main;
	};

	/** @brief A registered query: its devbench-facing description and callback. */
	struct QueryEntry
	{
		std::string description;
		QueryFn fn;
		ExecutionThread executionThread = ExecutionThread::Main;
	};

	/**
	 * @brief Per-feature tables of devbench-invocable commands (one-shot, non-settings
	 * actions) and queries (read-only computed values), the imperative/derived-state
	 * counterpart to how Feature::SaveSettings/LoadSettings already make settings fields
	 * devbench-drivable with no bridge changes. Populated once at boot by
	 * DevBenchBridge::Install() from each Feature::RegisterUxActions() override -- see
	 * FEATURE_COMMAND/FEATURE_QUERY below. Immutable after boot: safe for the devbench
	 * listener thread to read without locking.
	 */
	class Registry
	{
	public:
		/** @brief Process-wide instance; there is exactly one registry. */
		static Registry& GetSingleton();

		/** @brief Registers a command under a feature's shortName. Use FEATURE_COMMAND, not this directly. */
		void RegisterCommand(std::string_view a_featureShortName, std::string_view a_name, std::string_view a_description, CommandFn a_fn, ExecutionThread a_thread = ExecutionThread::Main);
		/** @brief Registers a query under a feature's shortName. Use FEATURE_QUERY, not this directly. */
		void RegisterQuery(std::string_view a_featureShortName, std::string_view a_name, std::string_view a_description, QueryFn a_fn, ExecutionThread a_thread = ExecutionThread::Main);

		/** @return The matching command, or nullptr if no feature/name combination is registered. */
		const CommandEntry* FindCommand(std::string_view a_featureShortName, std::string_view a_name) const;
		/** @return The matching query, or nullptr if no feature/name combination is registered. */
		const QueryEntry* FindQuery(std::string_view a_featureShortName, std::string_view a_name) const;

		/** @return `[{name, description, executionThread}, ...]` for this feature's commands. */
		json ListCommands(std::string_view a_featureShortName) const;
		/** @return `[{name, description, executionThread}, ...]` for this feature's queries. */
		json ListQueries(std::string_view a_featureShortName) const;

	private:
		std::unordered_map<std::string, std::unordered_map<std::string, CommandEntry>> commands;
		std::unordered_map<std::string, std::unordered_map<std::string, QueryEntry>> queries;
	};
}

/**
 * @brief Registers a devbench command from a Feature::RegisterUxActions override.
 * @param Fn Capture-less lambda `[](Feature* self, const json& args) { ... }` (the unary
 * `+` casts it to a plain function pointer -- a capturing lambda won't compile here).
 */
#define FEATURE_COMMAND(Name, Desc, Fn) \
	::Util::DevBenchUx::Registry::GetSingleton().RegisterCommand(GetShortName(), Name, Desc, +Fn)

/** @brief Same contract as FEATURE_COMMAND, for a read-only computed value.
 * @param Fn Capture-less lambda `[](const Feature* self, const json& args) -> json { ... }`. */
#define FEATURE_QUERY(Name, Desc, Fn) \
	::Util::DevBenchUx::Registry::GetSingleton().RegisterQuery(GetShortName(), Name, Desc, +Fn)

/** @brief Registers a command whose live state belongs to the render thread. */
#define FEATURE_RENDER_COMMAND(Name, Desc, Fn) \
	::Util::DevBenchUx::Registry::GetSingleton().RegisterCommand(GetShortName(), Name, Desc, +Fn, ::Util::DevBenchUx::ExecutionThread::Render)

/** @brief Registers a query whose live state belongs to the render thread. */
#define FEATURE_RENDER_QUERY(Name, Desc, Fn) \
	::Util::DevBenchUx::Registry::GetSingleton().RegisterQuery(GetShortName(), Name, Desc, +Fn, ::Util::DevBenchUx::ExecutionThread::Render)
