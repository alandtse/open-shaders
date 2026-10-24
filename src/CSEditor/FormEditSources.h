#pragma once

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace FormEditSources
{
	using FormKey = std::pair<std::string, std::string>;
	using FormKeySet = std::set<FormKey>;
	inline constexpr const char* kFormsSubdir = "Forms";
	/** @brief A portable form edit prepared for export. */
	struct FormEdit
	{
		FormKey key;
		nlohmann::json content;
	};
	/** @brief Returns a pack's form-edit payload directory. */
	std::filesystem::path GetPackFormsRoot(const std::filesystem::path& packRoot);
	/** @brief Lists the saved form-edit files provided by one pack. */
	std::vector<std::filesystem::path> ListPackFormFiles(const std::filesystem::path& packRoot);
	/** @brief Returns whether one pack supplies any saved form edit. */
	bool HasPackFormFiles(const std::filesystem::path& packRoot);
	/** @brief Returns the active pack's display name. */
	std::string GetActivePackName();
	/** @brief Whether a personal form document exists, cached until a file change or source reload. */
	bool HasPersonalEdits();
	/** @brief Invalidates personal-file availability after a successful save, delete or explicit reload. */
	void InvalidatePersonalEdits();
	/** @brief Whether cached personal files or active-preset form keys supply any form document. */
	bool HasEffectiveFormEdits();
	/** @brief Returns personal and active-preset form identities. */
	FormKeySet GetEffectiveKeys();
	/** @brief Reads effective form edits, rejecting unreadable sources. */
	bool ReadEffectiveFormEdits(std::vector<FormEdit>& out);

	/** @brief Preserves editor history when a preset source transaction rolls back. */
	class ReloadHistoryGuard
	{
	public:
		/** @brief Captures the current editor history before reloading sources. */
		ReloadHistoryGuard();
		/** @brief Restores the captured history unless the transaction committed. */
		~ReloadHistoryGuard();
		ReloadHistoryGuard(const ReloadHistoryGuard&) = delete;
		ReloadHistoryGuard& operator=(const ReloadHistoryGuard&) = delete;
		/** @brief Keeps the history invalidation from a successful source transaction. */
		void Commit() { rollback = {}; }

	private:
		std::function<void()> rollback;
	};

	/** @brief Resolves a form document under a personal or preset Forms root. */
	std::filesystem::path GetFilePath(const std::filesystem::path& root, const FormKey& key);
	/** @brief Finds portable form keys in the editor's existing save folders. */
	FormKeySet ScanKeys(const std::filesystem::path& root);
	/** @brief Uses the personal document first, then the active pack's document. */
	std::filesystem::path ResolveFile(const FormKey& key);
	/** @brief Returns the active pack's cached form keys. */
	const FormKeySet& GetPackFormKeys();
	/** @brief Rescans the active pack and returns both previous and current form keys. */
	FormKeySet Refresh();
	/** @brief Returns whether the active pack has saved form documents. */
	bool HasPackFormEdits();
	/** @brief Reads one bounded JSON object without changing live forms. */
	bool ReadDocument(const std::filesystem::path& path, nlohmann::json& document);
	/** @brief Rejects pack changes while an editor widget holds unsaved edits. */
	bool CanChangePacks();
	/** @brief Reloads changed forms transactionally, or defers until the player is available. */
	bool ReloadActivePack();
	/** @brief Copies saved personal-first form documents into an exported preset. */
	bool ExportTo(const std::filesystem::path& packRoot);
}
