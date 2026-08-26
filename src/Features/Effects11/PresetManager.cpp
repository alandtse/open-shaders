#include "PresetManager.h"

#include <Windows.h>
#include <fstream>

#include "Utils/FileSystem.h"

namespace
{
	struct KnownEffect
	{
		const char* flagName;
		const char* fileName;
	};

	// The optional ENB effects Effects11 actually loads (see EffectManager.h); enbeffect.fx
	// is unconditional and has no matching ini flag, so it's excluded here.
	constexpr KnownEffect kKnownEffects[] = {
		{ "EnableBloom", "enbbloom.fx" },
		{ "EnableLens", "enblens.fx" },
		{ "EnableAdaptation", "enbadaptation.fx" },
		{ "EnablePostPassShader", "enbeffectpostpass.fx" },
	};

	bool ReadIniBool(const std::string& a_iniPath, const char* a_key)
	{
		char buffer[16];
		GetPrivateProfileStringA("EFFECT", a_key, "false", buffer, sizeof(buffer), a_iniPath.c_str());
		return _stricmp(buffer, "true") == 0;
	}

	// Reproduces enbeffect.fx's leading comment block verbatim (minus pure border
	// decoration), handling both "// line" and "/* ... */" block style. Empty if the
	// file has no such block.
	std::string ScrapeHeaderComment(const std::filesystem::path& a_enbeffectPath)
	{
		std::ifstream file(a_enbeffectPath);
		if (!file)
			return {};

		std::string result;
		std::string line;
		bool insideBlockComment = false;
		int contentLines = 0;
		for (int i = 0; i < 40 && contentLines < 10 && std::getline(file, line); ++i) {
			const auto firstNonSpace = line.find_first_not_of(" \t\r");
			const bool blank = firstNonSpace == std::string::npos;

			if (!insideBlockComment) {
				if (blank)
					continue;  // gap between comment paragraphs -- keep scanning
				const bool isLineComment = line.compare(firstNonSpace, 2, "//") == 0;
				const bool blockOpens = line.compare(firstNonSpace, 2, "/*") == 0;
				if (!isLineComment && !blockOpens)
					break;  // a real (non-comment) code line ends the header block
				if (blockOpens)
					insideBlockComment = true;
			} else if (line.find("*/") != std::string::npos) {
				insideBlockComment = false;
			}

			std::string content = blank ? std::string{} : line.substr(firstNonSpace);

			// Strip only a leading/trailing comment marker -- never scan-and-remove "//"
			// mid-line, or a "http://" inside the text gets mangled.
			if (content.rfind("//", 0) == 0 || content.rfind("/*", 0) == 0)
				content.erase(0, 2);
			else if (!content.empty() && content.front() == '*')
				content.erase(0, 1);

			auto trailEnd = content.find_last_not_of(" \t\r");
			while (trailEnd != std::string::npos && trailEnd >= 1 &&
				   ((content[trailEnd - 1] == '*' && content[trailEnd] == '/') ||
					   (content[trailEnd - 1] == '/' && content[trailEnd] == '/'))) {
				content.erase(trailEnd - 1);
				trailEnd = content.find_last_not_of(" \t\r");
			}

			auto start = content.find_first_not_of(" \t");
			if (start == std::string::npos)
				continue;
			auto end = content.find_last_not_of(" \t\r");
			content = content.substr(start, end - start + 1);

			if (content.find_first_not_of("+-=*/ \t") == std::string::npos)
				continue;  // pure border decoration, not content

			result += (result.empty() ? "" : "\n") + content;
			++contentLines;
		}
		return result;
	}

	void PopulateSummary(PresetLocation& a_location)
	{
		const auto enbseriesDir = a_location.root / "enbseries";
		const auto iniPathStr = (a_location.root / "enbseries.ini").string();

		for (const auto& known : kKnownEffects) {
			a_location.effectStatus.push_back({ known.flagName,
				ReadIniBool(iniPathStr, known.flagName),
				Util::PathHelpers::SafeExists(enbseriesDir / known.fileName) });
		}

		a_location.headerComment = ScrapeHeaderComment(enbseriesDir / "enbeffect.fx");
	}
}

PresetManager& PresetManager::GetSingleton()
{
	static PresetManager instance;
	return instance;
}

void PresetManager::Rescan()
{
	discoveredLocations.clear();

	// GetDataPath() resolves against the game exe's own path (falling back to CWD only
	// if that fails), unlike a bare absolute("Data") which trusts CWD outright -- those
	// can differ depending on how the game was launched.
	const auto dataRoot = Util::PathHelpers::GetDataPath();
	if (Util::PathHelpers::SafeExists(dataRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(dataRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::DataRoot, dataRoot, "Data", true });
	}

	const auto gameRoot = dataRoot.parent_path();
	if (Util::PathHelpers::SafeExists(gameRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(gameRoot / "enbseries")) {
		discoveredLocations.push_back({ PresetLocationKind::GameRoot, gameRoot, "Game root", true });
	}

	// Manual increment(ec) rather than a range-for: the implicit operator++() a range-for
	// calls is still throwing even when the iterator was constructed with an ec, so a file
	// deleted mid-scan could otherwise crash the whole game.
	std::error_code ec;
	for (auto it = std::filesystem::directory_iterator(dataRoot, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
		if (!it->is_directory(ec))
			continue;

		const auto childRoot = Util::PathHelpers::SafeAbsolute(it->path());
		if (Util::PathHelpers::SafeExists(childRoot / "enbseries.ini") && Util::PathHelpers::SafeIsDirectory(childRoot / "enbseries")) {
			discoveredLocations.push_back({ PresetLocationKind::DataSubfolder,
				childRoot,
				"Data\\" + childRoot.filename().string(),
				true });
		}
	}

	// Cheap (one ini read + a handful of stat() calls per location): safe to do inline
	// here since Rescan() only runs at Initialize() or an explicit UI/devbench action,
	// never per-frame.
	for (auto& location : discoveredLocations)
		PopulateSummary(location);

	// A rescan can drop the active preset (deleted from disk); GetENBSeriesPath() must
	// stop pointing at it once it's no longer in discoveredLocations.
	if (!activeRoot.empty() && !GetActiveLocation())
		activeRoot.clear();
}

const std::vector<PresetLocation>& PresetManager::GetDiscoveredLocations() const
{
	return discoveredLocations;
}

void PresetManager::SetActiveLocation(const std::filesystem::path& root)
{
	activeRoot = root;
}

const PresetLocation* PresetManager::GetActiveLocation() const
{
	for (const auto& loc : discoveredLocations) {
		if (loc.root == activeRoot)
			return &loc;
	}
	return nullptr;
}

std::string PresetManager::ToRelativeKey(const std::filesystem::path& a_root) const
{
	// SafeRelative falls back to returning a_root unchanged on failure; a_root is always
	// absolute here, so an absolute result means resolution failed -- treat that as no key
	// rather than persisting/matching against a path that was never actually relative.
	auto relative = Util::PathHelpers::SafeRelative(a_root, Util::PathHelpers::GetDataPath().parent_path());
	if (relative.is_absolute())
		return {};
	return relative.string();
}

const PresetLocation* PresetManager::FindByRelativeKey(const std::string& a_relativeKey) const
{
	for (const auto& loc : discoveredLocations) {
		if (ToRelativeKey(loc.root) == a_relativeKey)
			return &loc;
	}
	return nullptr;
}

std::filesystem::path PresetManager::GetENBSeriesPath() const
{
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries";
}

std::filesystem::path PresetManager::GetENBSeriesIniPath() const
{
	if (activeRoot.empty())
		return {};
	return activeRoot / "enbseries.ini";
}
