#include "FormEditSources.h"

#include "EditorWindow.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "Utils/FileSystem.h"
#include "Utils/StringUtils.h"
#include "Widget.h"

#include <fstream>
#include <optional>
#include <vector>

namespace
{
	constexpr std::uintmax_t kMaxFormEditBytes = 1024 * 1024;
	constexpr const char* kJsonExtension = ".json";
	std::optional<bool> personalEditFiles;
	std::optional<FormEditSources::FormKeySet> packKeys;
}

namespace FormEditSources
{
	ReloadHistoryGuard::ReloadHistoryGuard()
	{
		auto* editor = EditorWindow::GetSingleton();
		rollback = [editor, history = editor->undoHistory]() mutable {
			editor->undoHistory = std::move(history);
		};
	}

	ReloadHistoryGuard::~ReloadHistoryGuard()
	{
		if (rollback)
			rollback();
	}

	std::filesystem::path GetFilePath(const std::filesystem::path& root, const FormKey& key)
	{
		return root / Util::PathHelpers::FromUtf8(key.first) / Util::PathHelpers::FromUtf8(key.second + ".json");
	}

	FormKeySet ScanKeys(const std::filesystem::path& root)
	{
		FormKeySet keys;
		if (root.empty())
			return keys;
		for (const auto folder : Widget::kSaveFolderNames) {
			std::error_code error;
			for (std::filesystem::directory_iterator entry(root / folder, error), end; !error && entry != end; entry.increment(error)) {
				if (entry->is_regular_file(error) && Util::IEquals(entry->path().extension().string(), ".json"))
					keys.emplace(folder, Util::PathHelpers::ToUtf8(entry->path().stem()));
			}
		}
		return keys;
	}

	std::filesystem::path ResolveFile(const FormKey& key)
	{
		std::error_code error;
		const auto personal = GetFilePath(Util::PathHelpers::GetCommunityShaderPath(), key);
		if (std::filesystem::exists(personal, error))
			return personal;
		return ResolvePackFile(key);
	}

	std::filesystem::path ResolvePackFile(const FormKey& key)
	{
		if (!GetPackFormKeys().contains(key))
			return {};
		std::error_code error;
		for (const auto& root : UnifiedPresetCatalog::GetSingleton().GetActivePackRoots()) {
			const auto path = GetFilePath(root / kFormsSubdir, key);
			error.clear();
			if (std::filesystem::is_regular_file(path, error))
				return path;
		}
		return {};
	}

	const FormKeySet& GetPackFormKeys()
	{
		if (!packKeys) {
			packKeys.emplace();
			for (const auto& root : UnifiedPresetCatalog::GetSingleton().GetActivePackRoots()) {
				const auto keys = ScanKeys(root / kFormsSubdir);
				packKeys->insert(keys.begin(), keys.end());
			}
		}
		return *packKeys;
	}

	void InvalidatePersonalEdits()
	{
		personalEditFiles.reset();
	}

	bool HasPersonalEdits()
	{
		if (personalEditFiles)
			return *personalEditFiles;

		const auto communityShaderPath = Util::PathHelpers::GetCommunityShaderPath();
		for (const auto folderName : Widget::kSaveFolderNames) {
			const auto widgetSettingsPath = communityShaderPath / std::filesystem::path(folderName);
			std::error_code ec;
			const bool isDirectory = std::filesystem::is_directory(widgetSettingsPath, ec);
			if (ec) {
				// A missing folder is the normal case (the user simply has no saved
				// widgets for this category), so don't treat it as a warning.
				if (ec != std::errc::no_such_file_or_directory)
					logger::warn("[CSEditor] Failed to inspect widget settings path '{}': {}", widgetSettingsPath.string(), ec.message());
				continue;
			}
			if (!isDirectory)
				continue;

			for (std::filesystem::directory_iterator it(widgetSettingsPath, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code entryEc;
				const bool isRegularFile = it->is_regular_file(entryEc);
				if (entryEc) {
					logger::warn("[CSEditor] Failed to inspect widget settings file '{}': {}", it->path().string(), entryEc.message());
					continue;
				}
				if (isRegularFile && Util::IEquals(it->path().extension().string(), kJsonExtension)) {
					logger::info("[CSEditor] Detected widget settings in '{}'", widgetSettingsPath.string());
					personalEditFiles = true;
					return true;
				}
			}
			if (ec) {
				logger::warn("[CSEditor] Failed to scan widget settings path '{}': {}", widgetSettingsPath.string(), ec.message());
				continue;
			}
		}

		personalEditFiles = false;
		return false;
	}

	bool HasEffectiveFormEdits()
	{
		return HasPersonalEdits() || HasPackFormEdits();
	}

	FormKeySet Refresh()
	{
		InvalidatePersonalEdits();
		auto changed = GetPackFormKeys();
		packKeys.reset();
		const auto& current = GetPackFormKeys();
		changed.insert(current.begin(), current.end());
		return changed;
	}

	bool HasPackFormEdits()
	{
		return !GetPackFormKeys().empty();
	}

	bool ReadDocument(const std::filesystem::path& path, nlohmann::json& document)
	{
		std::error_code error;
		const auto bytes = std::filesystem::file_size(path, error);
		if (error || !bytes || bytes > kMaxFormEditBytes)
			return false;
		std::ifstream input(path);
		document = nlohmann::json::parse(input, nullptr, false);
		return document.is_object();
	}

	bool CanChangePacks()
	{
		return !EditorWindow::GetSingleton()->HasUnsavedFormEdits();
	}

	bool ReloadActivePack()
	{
		if (!CanChangePacks())
			return false;
		const auto changed = Refresh();
		if (!EditorWindow::CanBeOpen())
			return true;
		auto* editor = EditorWindow::GetSingleton();
		if (!editor->HasWidgetResources()) {
			if (!changed.empty())
				return editor->SetupResources();
			return true;
		}
		return editor->ReloadFormEdits(changed);
	}

	bool ExportTo(const std::filesystem::path& packRoot)
	{
		auto keys = ScanKeys(Util::PathHelpers::GetCommunityShaderPath());
		const auto& preset = GetPackFormKeys();
		keys.insert(preset.begin(), preset.end());
		std::vector<std::pair<FormKey, nlohmann::json>> documents;
		for (const auto& key : keys) {
			nlohmann::json document;
			if (!ReadDocument(ResolveFile(key), document))
				return false;
			documents.emplace_back(key, std::move(document));
		}
		for (const auto& [key, document] : documents) {
			const auto destination = GetFilePath(packRoot / kFormsSubdir, key);
			std::error_code error;
			std::filesystem::create_directories(destination.parent_path(), error);
			if (error || !Util::FileHelpers::WriteJsonAtomically(destination, document, 2, "preset form edit"))
				return false;
		}
		return true;
	}

	std::filesystem::path GetPackFormsRoot(const std::filesystem::path& packRoot)
	{
		return packRoot / kFormsSubdir;
	}

	std::vector<std::filesystem::path> ListPackFormFiles(const std::filesystem::path& packRoot)
	{
		std::vector<std::filesystem::path> files;
		for (const auto& key : ScanKeys(GetPackFormsRoot(packRoot)))
			files.push_back(GetFilePath(GetPackFormsRoot(packRoot), key));
		return files;
	}

	bool HasPackFormFiles(const std::filesystem::path& packRoot)
	{
		return !ScanKeys(GetPackFormsRoot(packRoot)).empty();
	}

	std::string GetActivePackName()
	{
		const auto& catalog = UnifiedPresetCatalog::GetSingleton();
		const auto* pack = catalog.FindPack(catalog.GetActivePackId());
		return pack ? pack->name : catalog.GetActivePackId();
	}

	FormKeySet GetEffectiveKeys()
	{
		auto keys = ScanKeys(Util::PathHelpers::GetCommunityShaderPath());
		const auto& pack = GetPackFormKeys();
		keys.insert(pack.begin(), pack.end());
		return keys;
	}
	bool ReadEffectiveFormEdits(std::vector<FormEdit>& out)
	{
		bool success = true;
		for (const auto& key : GetEffectiveKeys()) {
			nlohmann::json document;
			if (ReadDocument(ResolveFile(key), document))
				out.push_back({ key, std::move(document) });
			else
				success = false;
		}
		return success;
	}

	std::optional<Util::Settings::SettingsCopy> CopyPresetSettings(const std::string& packId, const PresetContent::Selection& selection, bool replaceExisting)
	try {
		if (selection.empty() || !CanChangePacks())
			return std::nullopt;
		const auto files = PresetContent::List(packId);
		struct Document
		{
			std::optional<json> original;
			json replacement;
			bool written = false;
		};
		std::map<std::filesystem::path, Document> documents;
		FormKeySet changed;
		Util::Settings::SettingsCopy result;
		for (const auto& [relative, paths] : selection) {
			if (!PresetContent::IsRecordFile(relative) || paths.empty())
				return std::nullopt;
			const auto file = std::ranges::find(files, relative, &PresetContent::File::relative);
			const FormKey key{ Util::PathToUtf8(*std::next(relative.begin())), Util::PathToUtf8(relative.stem()) };
			if (file == files.end() || !file->document || !Util::IsSinglePathComponent(key.second) ||
				std::ranges::find(Widget::kSaveFolderNames, key.first) == Widget::kSaveFolderNames.end())
				return std::nullopt;
			const auto destination = GetFilePath(Util::PathHelpers::GetCommunityShaderPath(), key);
			auto& document = documents[destination];
			std::error_code error;
			const bool exists = std::filesystem::exists(destination, error);
			if (error)
				return std::nullopt;
			if (exists) {
				json original;
				if (!ReadDocument(destination, original))
					return std::nullopt;
				document.original = std::move(original);
			}
			const auto values = Util::Settings::SelectSettingPaths(*file->document, paths);
			PresetActivation::Footprint leaves;
			PresetContent::CollectValues(values, "", leaves);
			if (leaves.size() != std::set<std::string>(paths.begin(), paths.end()).size())
				return std::nullopt;
			const auto personal = document.original.value_or(json::object());
			const auto copy = Util::Settings::SelectSettingsToCopy(values, personal, replaceExisting);
			result.copied += copy.copied;
			result.overwritten += copy.overwritten;
			result.skipped += copy.skipped;
			if (copy.values.empty()) {
				documents.erase(destination);
				continue;
			}
			document.replacement = personal;
			Util::Settings::MergeSettings(document.replacement, copy.values);
			changed.insert(key);
		}
		if (documents.empty())
			return result;
		ReloadHistoryGuard history;
		bool committed = false;
		const SKSE::stl::scope_exit rollback([&] {
			if (committed)
				return;
			for (const auto& [path, document] : documents)
				if (document.written) {
					if (document.original) {
						if (!Util::FileHelpers::WriteJsonAtomically(path, *document.original, 2, "record copy rollback"))
							logger::error("Could not restore personal record file '{}'", path.string());
					} else {
						std::error_code error;
						std::filesystem::remove(path, error);
						if (error)
							logger::error("Could not remove failed record copy '{}': {}", path.string(), error.message());
					}
				}
			InvalidatePersonalEdits();
			if (EditorWindow::CanBeOpen() && EditorWindow::GetSingleton()->HasWidgetResources() &&
				!EditorWindow::GetSingleton()->ReloadFormEdits(changed))
				logger::error("Could not reload personal records after a failed preset copy");
		});
		for (auto& [path, document] : documents) {
			if (!Util::FileHelpers::WriteJsonAtomically(path, document.replacement, 2, "copied record settings"))
				return std::nullopt;
			document.written = true;
		}
		InvalidatePersonalEdits();
		if (EditorWindow::CanBeOpen()) {
			auto* editor = EditorWindow::GetSingleton();
			if (!editor->HasWidgetResources() && !editor->SetupResources())
				return std::nullopt;
			if (!editor->ReloadFormEdits(changed))
				return std::nullopt;
		}
		committed = true;
		history.Commit();
		return result;
	} catch (const std::exception& error) {
		logger::error("Could not copy preset record settings: {}", error.what());
		return std::nullopt;
	}
}
