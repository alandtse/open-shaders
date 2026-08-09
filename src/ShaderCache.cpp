#include "ShaderCache.h"
#include "Globals.h"
#include "ShaderCompileStatus.h"
#include "ShaderFileWatcher.h"
#include "Util.h"

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <d3dcompiler.h>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "Deferred.h"
#include "Feature.h"
#include "State.h"
#include "Utils/ContentHash.h"
#include "Utils/ShaderCacheManifest.h"

#include "Features/DynamicCubemaps.h"

#include "Plugin.h"

namespace SIE
{
	// Chrono-free snapshot of compile counters for consumers that can't include
	// ShaderCache.h (see ShaderCompileStatus.h). Thread-safe: atomics + the
	// shader-map lock inside GetCurrentFailedCount.
	ShaderCompileStatus GetShaderCompileStatus()
	{
		auto* cache = globals::shaderCache;
		if (!cache)
			return {};
		// Read the task counters once and derive `compiling` from the same
		// snapshot, so callers never observe compiling=false with work still
		// outstanding. Named-field init avoids depending on member order.
		const uint64_t completed = cache->GetCompletedTasks();
		const uint64_t total = cache->GetTotalTasks();
		ShaderCompileStatus status{};
		status.valid = true;
		status.compiling = completed < total;
		status.completedTasks = completed;
		status.totalTasks = total;
		status.failedTasks = cache->GetFailedTasks();
		status.currentFailedCount = cache->GetCurrentFailedCount();
		return status;
	}

	struct IncludeParseEntry
	{
		std::chrono::system_clock::time_point selfMTime;
		std::vector<std::filesystem::path> includes;
		std::optional<Util::ContentHash::Hash128> selfContentHash;
	};

	static std::string NormalizedPathKey(const std::filesystem::path& path)
	{
		// lexically_normal collapses ".."/"." spellings so aliases of one file share a key
		// (and can't slip past the per-call cycle guard).
		std::string key = path.lexically_normal().string();
#ifdef _WIN32
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
		return key;
	}

	// Folds a_path's key + mtime into *a_fingerprint once per node's first visit,
	// so it also changes on a closure shape change, unlike max mtime alone.
	static void FoldClosureFingerprint(Util::ContentHash::Hash128* a_fingerprint, const std::string& a_key,
		std::chrono::system_clock::time_point a_mtime)
	{
		if (!a_fingerprint)
			return;
		const int64_t ticks = a_mtime.time_since_epoch().count();
		*a_fingerprint = Util::ContentHash::CombineHashes(*a_fingerprint,
			Util::ContentHash::CombineHashes(Util::ContentHash::HashString(a_key), Util::ContentHash::HashBytes(&ticks, sizeof(ticks))));
	}

	/// Newest mtime across a shader source and its recursive quoted includes. Only the parsed
	/// include lists are cached across calls (keyed by each file's own mtime); every query re-stats
	/// the graph, so a changed descendant is always seen. The scan is textual (preprocessor-blind),
	/// which over-approximates: an ifdef'd-out include still contributes its mtime. That errs
	/// toward recompiling, never toward serving stale.
	static std::chrono::system_clock::time_point GetMaxShaderMTimeInternal(
		const std::filesystem::path& path,
		const std::filesystem::path& shadersRoot,
		std::unordered_map<std::string, IncludeParseEntry>& parseCache,
		std::mutex& parseCacheMutex,
		std::unordered_map<std::string, std::chrono::system_clock::time_point>& callResults,
		Util::ContentHash::Hash128* fingerprint = nullptr)
	{
		const std::string key = NormalizedPathKey(path);
		if (auto it = callResults.find(key); it != callResults.end())
			return it->second;
		// In-progress marker: an include cycle resolves to min() and drops out of the max.
		callResults[key] = std::chrono::system_clock::time_point::min();

		std::error_code ec;
		const auto selfMTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(path, ec));
		if (ec) {
			// Unreadable source: report "just changed" so the cache recompiles rather than serving stale.
			const auto now = std::chrono::system_clock::now();
			callResults[key] = now;
			// now() never repeats, so a closure containing this node never re-hits
			// the digest memo below -- conservatively forces a recompile.
			FoldClosureFingerprint(fingerprint, key, now);
			return now;
		}
		FoldClosureFingerprint(fingerprint, key, selfMTime);

		// Hold parseCacheMutex only around the shared-map lookup/insert; the stat above and the
		// file read below run unlocked so parallel validity checks don't serialize on cold boot.
		// The include list is copied out (never a pointer into the map) so a concurrent insert
		// can't rehash it out from under the recursion.
		std::vector<std::filesystem::path> includes;
		bool cached = false;
		{
			std::lock_guard lock(parseCacheMutex);
			if (auto it = parseCache.find(key); it != parseCache.end() && it->second.selfMTime == selfMTime) {
				includes = it->second.includes;
				cached = true;
			}
		}

		if (!cached) {
			std::ifstream ifs(path);
			if (!ifs.is_open()) {
				// Metadata readable but contents not (transient lock/AV): force a recompile
				// rather than caching an empty include list and serving stale. Already
				// folded into the fingerprint above; no second fold needed here.
				const auto now = std::chrono::system_clock::now();
				callResults[key] = now;
				return now;
			}
			std::string line;
			while (std::getline(ifs, line)) {
				// Accept whitespace between '#' and "include" ("#	include" is the norm in
				// this repo's nested-#if style).
				size_t pos = line.find_first_not_of(" 	");
				if (pos == std::string::npos || line[pos] != '#')
					continue;
				pos = line.find_first_not_of(" 	", pos + 1);
				if (pos == std::string::npos || line.compare(pos, 7, "include") != 0)
					continue;
				// Accept both quoted and angle-bracket includes; under-tracking either
				// form risks serving a stale cache, which the textual scan must never do.
				const size_t afterInclude = pos + 7;
				const size_t firstDelim = line.find_first_of("\"<", afterInclude);
				if (firstDelim == std::string::npos)
					continue;
				const char closeDelim = line[firstDelim] == '"' ? '"' : '>';
				const size_t secondDelim = line.find(closeDelim, firstDelim + 1);
				if (secondDelim == std::string::npos || secondDelim == firstDelim + 1)
					continue;
				const std::string includeName = line.substr(firstDelim + 1, secondDelim - firstDelim - 1);

				std::error_code rootEc, parentEc;
				std::filesystem::path includePath = shadersRoot / includeName;
				if (!std::filesystem::exists(includePath, rootEc)) {
					includePath = path.parent_path() / includeName;
					// Clean "not found" at both probes: an ifdef'd-out include for an uninstalled
					// feature. Skipping matches the compiler (which never opens it). A probe IO error
					// instead falls through so the stat below forces a recompile rather than serving stale.
					if (!std::filesystem::exists(includePath, parentEc) && !rootEc && !parentEc)
						continue;
				}
				includes.push_back(std::move(includePath));
			}
			std::lock_guard lock(parseCacheMutex);
			parseCache[key] = IncludeParseEntry{ selfMTime, includes };
		}

		auto maxTime = selfMTime;
		for (const auto& includePath : includes)
			maxTime = std::max(maxTime, GetMaxShaderMTimeInternal(includePath, shadersRoot, parseCache, parseCacheMutex, callResults, fingerprint));

		callResults[key] = maxTime;
		return maxTime;
	}

	// Shared by GetMaxShaderMTime and GetShaderContentDigest so a closure is
	// parsed for #includes at most once per session no matter which caller
	// (or both) queries it first.
	std::unordered_map<std::string, IncludeParseEntry> g_shaderIncludeParseCache;
	std::mutex g_shaderIncludeParseCacheMutex;

	static std::chrono::system_clock::time_point GetMaxShaderMTime(
		const std::filesystem::path& path,
		const std::filesystem::path& shadersRoot,
		Util::ContentHash::Hash128* fingerprint = nullptr)
	{
		std::unordered_map<std::string, std::chrono::system_clock::time_point> callResults;
		return GetMaxShaderMTimeInternal(path, shadersRoot, g_shaderIncludeParseCache, g_shaderIncludeParseCacheMutex, callResults, fingerprint);
	}

	// Root-only memo of GetShaderContentDigest's closure digest; interior nodes
	// must never be memoized here, since callResults is only valid per-traversal.
	struct ClosureDigestEntry
	{
		Util::ContentHash::Hash128 closureFingerprint;
		Util::ContentHash::Hash128 digest;
	};
	std::unordered_map<std::string, ClosureDigestEntry> g_shaderClosureDigestCache;
	std::mutex g_shaderClosureDigestCacheMutex;

	/// Merkle-style content digest over a shader source and its recursive quoted
	/// includes (path-sorted, so ordering doesn't affect the result). Reads the
	/// same parse cache GetMaxShaderMTimeInternal populates rather than
	/// re-scanning for #includes itself. nullopt only if the root is unreadable;
	/// an unreadable descendant is skipped, not fatal to the whole digest.
	static std::optional<Util::ContentHash::Hash128> GetShaderContentDigestInternal(
		const std::filesystem::path& path,
		std::unordered_map<std::string, IncludeParseEntry>& parseCache,
		std::mutex& parseCacheMutex,
		std::unordered_map<std::string, std::optional<Util::ContentHash::Hash128>>& callResults)
	{
		const std::string key = NormalizedPathKey(path);
		if (auto it = callResults.find(key); it != callResults.end())
			return it->second;
		callResults[key] = std::nullopt;  // cycle guard: a cycle contributes nothing extra to its own combine

		std::error_code ec;
		const auto selfMTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(path, ec));

		std::optional<Util::ContentHash::Hash128> selfHash;
		std::vector<std::filesystem::path> includes;
		{
			std::lock_guard lock(parseCacheMutex);
			if (auto it = parseCache.find(key); it != parseCache.end()) {
				includes = it->second.includes;
				if (!ec && it->second.selfMTime == selfMTime && it->second.selfContentHash.has_value())
					selfHash = it->second.selfContentHash;
			}
		}

		if (!selfHash) {
			selfHash = Util::ContentHash::HashFile(path);
			if (selfHash && !ec) {
				std::lock_guard lock(parseCacheMutex);
				auto& entry = parseCache[key];
				entry.selfMTime = selfMTime;
				entry.selfContentHash = selfHash;
			}
		}
		if (!selfHash) {
			callResults[key] = std::nullopt;
			return std::nullopt;
		}

		std::vector<std::pair<std::string, std::filesystem::path>> keyedIncludes;
		keyedIncludes.reserve(includes.size());
		for (auto& inc : includes)
			keyedIncludes.emplace_back(NormalizedPathKey(inc), inc);
		std::sort(keyedIncludes.begin(), keyedIncludes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

		auto combined = *selfHash;
		for (const auto& [includeKey, includePath] : keyedIncludes) {
			if (auto childHash = GetShaderContentDigestInternal(includePath, parseCache, parseCacheMutex, callResults))
				combined = Util::ContentHash::CombineHashes(combined, *childHash);
		}

		callResults[key] = combined;
		return combined;
	}

	static std::optional<Util::ContentHash::Hash128> GetShaderContentDigest(
		const std::filesystem::path& path,
		const std::filesystem::path& shadersRoot)
	{
		// Builds this call's closure fingerprint, checked against the digest memo
		// below so a repeat call for the same source skips the recursive walk.
		Util::ContentHash::Hash128 fingerprint{};
		GetMaxShaderMTime(path, shadersRoot, &fingerprint);

		const std::string rootKey = NormalizedPathKey(path);
		{
			std::lock_guard lock(g_shaderClosureDigestCacheMutex);
			if (auto it = g_shaderClosureDigestCache.find(rootKey);
				it != g_shaderClosureDigestCache.end() && it->second.closureFingerprint == fingerprint)
				return it->second.digest;
		}

		std::unordered_map<std::string, std::optional<Util::ContentHash::Hash128>> callResults;
		const auto result = GetShaderContentDigestInternal(path, g_shaderIncludeParseCache, g_shaderIncludeParseCacheMutex, callResults);
		if (result) {
			std::lock_guard lock(g_shaderClosureDigestCacheMutex);
			g_shaderClosureDigestCache[rootKey] = ClosureDigestEntry{ fingerprint, *result };
		}
		return result;
	}

	// Times a digest computation and folds it into the cache's running stats, so
	// the two call sites (the disk-cache validity check and the post-compile
	// manifest write) share one measurement instead of duplicating the
	// QueryPerformanceCounter dance ProcessCompilationSet already uses for task timing.
	static std::optional<Util::ContentHash::Hash128> GetShaderContentDigestTimed(
		const std::filesystem::path& path,
		const std::filesystem::path& shadersRoot,
		ShaderCache& cache)
	{
		LARGE_INTEGER start, end, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		auto result = GetShaderContentDigest(path, shadersRoot);
		QueryPerformanceCounter(&end);
		cache.RecordDigestComputeTime((end.QuadPart - start.QuadPart) * 1000000LL / freq.QuadPart);
		return result;
	}

	/// Manifest key for a GetDiskPath() result: the path relative to
	/// Data/ShaderCache/, narrow-encoded, matching what SyncShaderDeploy.cmake
	/// and hlslkit will use for the same blob.
	static std::string GetManifestKey(const std::wstring& diskPath)
	{
		static constexpr std::wstring_view prefix = L"Data/ShaderCache/";
		const std::wstring_view rel = diskPath.starts_with(prefix) ? std::wstring_view(diskPath).substr(prefix.size()) : std::wstring_view(diskPath);
		return Util::WStringToString(std::wstring(rel));
	}

	static Util::ShaderCacheManifest::Manifest& GetShaderCacheManifest()
	{
		static Util::ShaderCacheManifest::Manifest manifest;
		static std::once_flag loaded;
		std::call_once(loaded, [] { manifest.Load(L"Data/ShaderCache/Manifest.json"); });
		return manifest;
	}

	// Developer Mode and the custom Shader Defines setting affect every compile
	// without changing a shader's own source; fold both into the digest so
	// flipping either forces a recompile instead of reusing a mismatched blob.
	static Util::ContentHash::Hash128 GetGlobalDefinesDigest()
	{
		std::string state;
		if (globals::state->IsDeveloperMode())
			state += "D3DCOMPILE_SKIP_OPTIMIZATION;D3DCOMPILE_DEBUG;";
		if (globals::game::isVR)
			state += "VR;";
		state += globals::state->shaderDefinesString;
		return Util::ContentHash::HashString(state);
	}

	// Batches manifest writes instead of re-serializing the whole file per
	// shader; CompilationSet::Complete() guarantees a final flush per batch.
	constexpr uint64_t kManifestFlushBatchSize = 25;
	std::atomic<uint64_t> g_manifestPendingWrites = 0;

	static void RecordDigestAndMaybeFlush(Util::ShaderCacheManifest::Manifest& manifest, const std::string& key, const std::string& digestHex)
	{
		manifest.Set(key, digestHex);
		if (g_manifestPendingWrites.fetch_add(1, std::memory_order_relaxed) + 1 >= kManifestFlushBatchSize) {
			g_manifestPendingWrites.store(0, std::memory_order_relaxed);
			manifest.Save();
		}
	}

	// Custom include handler to track all includes during shader compilation
	class TrackingIncludeHandler : public ID3DInclude
	{
	public:
		// Captured include paths (normalized)
		std::vector<std::string> includes;
		// Owned buffers for include contents; kept alive for the lifetime of this handler
		std::vector<std::vector<char>> buffers;
		std::filesystem::path baseDir;

		TrackingIncludeHandler(const std::filesystem::path& base) :
			baseDir(base) {}

		HRESULT Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID /*pParentData*/, LPCVOID* ppData, UINT* pBytes) override
		{
			(void)IncludeType;
			try {
				std::filesystem::path includePath = baseDir / pFileName;
				// Normalize path to reduce duplicates (weakly_canonical may throw)
				std::error_code ec;
				auto canonical = std::filesystem::weakly_canonical(includePath, ec);
				std::string pathStr = (ec ? includePath.string() : canonical.string());
				// On Windows, normalize to lowercase for comparison
#ifdef _WIN32
				std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
				includes.push_back(pathStr);

				// Read file into owned buffer
				std::ifstream ifs(pathStr, std::ios::binary | std::ios::ate);
				if (!ifs)
					return E_FAIL;
				std::streamsize size = ifs.tellg();
				if (size < 0)
					return E_FAIL;
				ifs.seekg(0, std::ios::beg);
				std::vector<char> buf(static_cast<size_t>(size));
				if (size > 0) {
					if (!ifs.read(buf.data(), size))
						return E_FAIL;
				}
				buffers.push_back(std::move(buf));
				const auto& storage = buffers.back();
				*ppData = storage.empty() ? nullptr : storage.data();
				*pBytes = static_cast<UINT>(storage.size());
				return S_OK;
			} catch (...) {
				return E_FAIL;
			}
		}

		HRESULT Close(LPCVOID /*pData*/) override
		{
			// Buffers are owned by this handler; no action required on Close.
			return S_OK;
		}
	};

	namespace SShaderCache
	{
		static void GetShaderDefines(const RE::BSShader&, uint32_t, D3D_SHADER_MACRO*);
		static std::string GetShaderString(ShaderClass, const RE::BSShader&, uint32_t, bool = false);
		/**
		 * @brief Resolve image-space shader descriptor when applicable.
		 *
		 * If @p shader is an image-space shader, attempts to map it to a
		 * runtime image-space descriptor via GetImagespaceShaderDescriptor and
		 * returns true on success. If the shader is not image-space the
		 * function returns true and leaves @p descriptor unchanged. Returns
		 * false only when the shader is image-space and no valid descriptor
		 * could be resolved.
		 *
		 * This helper is used by the shader loading and caching code paths to
		 * determine whether an image-space shader can be loaded or cached. If
		 * this function returns false the caller should skip loading/compiling
		 * and caching that shader.
		 *
		 * @param shader The shader to resolve (may be an image-space shader).
		 * @param[out] descriptor Resolved descriptor for image-space shaders.
		 * @return True if descriptor is valid or not applicable, false on failure.
		 */
		static bool ResolveImageSpaceDescriptor(const RE::BSShader& shader, uint32_t& descriptor);
		/**
		@brief Get the BSShader::Type from the ShaderString
		@param a_key The key generated from GetShaderString
		@return A string with a valid BSShader::Type
		*/
		static std::string GetTypeFromShaderString(const std::string&);
		constexpr const char* VertexShaderProfile = "vs_5_0";
		constexpr const char* PixelShaderProfile = "ps_5_0";
		constexpr const char* ComputeShaderProfile = "cs_5_0";

		static std::wstring GetShaderPath(const std::string_view& name)
		{
			return std::format(L"Data/Shaders/{}.hlsl", std::wstring(name.begin(), name.end()));
		}

		static const char* GetShaderProfile(ShaderClass shaderClass)
		{
			switch (shaderClass) {
			case ShaderClass::Vertex:
				return VertexShaderProfile;
			case ShaderClass::Pixel:
				return PixelShaderProfile;
			case ShaderClass::Compute:
				return ComputeShaderProfile;
			}
			return nullptr;
		}

		uint32_t GetTechnique(uint32_t descriptor)
		{
			return 0x3F & (descriptor >> 24);
		}

		static void GetLightingShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(RELOCATION_ID(101631, 108698));
			VanillaGetLightingShaderDefines(descriptor, defines.data());

			size_t lastIndex = std::ranges::find_if(defines, [](const D3D_SHADER_MACRO& macro) { return macro.Name == nullptr; }) - defines.begin();

			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}
			if ((descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr)) != 0) {
				defines[lastIndex++] = { "TRUE_PBR", nullptr };
				if ((descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::AnisoLighting)) != 0) {
					defines[lastIndex++] = { "GLINT", nullptr };
				}
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetBloodSplaterShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;
			if (descriptor == static_cast<uint32_t>(ShaderCache::BloodSplatterShaderTechniques::Splatter)) {
				defines[lastIndex++] = { "SPLATTER", nullptr };
			} else if (descriptor == static_cast<uint32_t>(ShaderCache::BloodSplatterShaderTechniques::Flare)) {
				defines[lastIndex++] = { "FLARE", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::BloodSplatter)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetDistantTreeShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			const auto technique = descriptor & 1;
			size_t lastIndex = 0;
			if (technique == static_cast<uint32_t>(ShaderCache::DistantTreeShaderTechniques::Depth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::DistantTreeShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(ShaderCache::DistantTreeShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::DistantTree)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetSkyShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::SkyShaderTechniques;

			const auto technique = static_cast<ShaderCache::SkyShaderTechniques>(descriptor & 255);
			size_t lastIndex = 0;
			switch (technique) {
			case SunOcclude:
				{
					defines[lastIndex++] = { "OCCLUSION", nullptr };
					break;
				}
			case SunGlare:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			case MoonAndStarsMask:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "MOONMASK", nullptr };
					break;
				}
			case Stars:
				{
					defines[lastIndex++] = { "HORIZFADE", nullptr };
					break;
				}
			case Clouds:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					break;
				}
			case CloudsLerp:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXLERP", nullptr };
					break;
				}
			case CloudsFade:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXFADE", nullptr };
					break;
				}
			case Texture:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					break;
				}
			case Sky:
				{
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			}

			uint32_t flags = descriptor >> 8;

			if (flags) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Sky)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetGrassShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			const auto technique = descriptor & 0b1111;
			size_t lastIndex = 0;
			if (technique == static_cast<uint32_t>(ShaderCache::GrassShaderTechniques::RenderDepth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::GrassShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Grass)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetParticleShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::ParticleShaderTechniques;

			const auto technique = static_cast<ShaderCache::ParticleShaderTechniques>(descriptor);
			size_t lastIndex = 0;
			switch (technique) {
			case ParticlesGryColor:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					break;
				}
			case ParticlesGryAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case ParticlesGryColorAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case EnvCubeSnow:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "SNOW", nullptr };
					break;
				}
			case EnvCubeRain:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "RAIN", nullptr };
					break;
				}
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Particle)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetEffectShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;

			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoord)) {
				defines[lastIndex++] = { "TEXCOORD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoordIndex)) {
				defines[lastIndex++] = { "TEXCOORD_INDEX", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Skinned)) {
				defines[lastIndex++] = { "SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Normals)) {
				defines[lastIndex++] = { "NORMALS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::BinormalTangent)) {
				defines[lastIndex++] = { "BINORMAL_TANGENT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Texture)) {
				defines[lastIndex++] = { "TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IndexedTexture)) {
				defines[lastIndex++] = { "INDEXED_TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Falloff)) {
				defines[lastIndex++] = { "FALLOFF", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AddBlend)) {
				defines[lastIndex++] = { "ADDBLEND", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlend)) {
				defines[lastIndex++] = { "MULTBLEND", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Particles)) {
				defines[lastIndex++] = { "PARTICLES", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::StripParticles)) {
				defines[lastIndex++] = { "STRIP_PARTICLES", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Blood)) {
				defines[lastIndex++] = { "BLOOD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Membrane)) {
				defines[lastIndex++] = { "MEMBRANE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Lighting)) {
				defines[lastIndex++] = { "LIGHTING", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::ProjectedUv)) {
				defines[lastIndex++] = { "PROJECTED_UV", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Soft)) {
				defines[lastIndex++] = { "SOFT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToColor)) {
				defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToAlpha)) {
				defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IgnoreTexAlpha)) {
				defines[lastIndex++] = { "IGNORE_TEX_ALPHA", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlendDecal)) {
				defines[lastIndex++] = { "MULTBLEND_DECAL", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "ALPHA_TEST", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::SkyObject)) {
				defines[lastIndex++] = { "SKY_OBJECT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MsnSpuSkinned)) {
				defines[lastIndex++] = { "MSN_SPU_SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MotionVectorsNormals)) {
				defines[lastIndex++] = { "MOTIONVECTORS_NORMALS", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Effect)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetWaterShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;
			defines[lastIndex++] = { "WATER", nullptr };
			defines[lastIndex++] = { "FOG", nullptr };

			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::NormalTexCoord)) {
				defines[lastIndex++] = { "NORMAL_TEXCOORD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Reflections)) {
				defines[lastIndex++] = { "REFLECTIONS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Refractions)) {
				defines[lastIndex++] = { "REFRACTIONS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Depth)) {
				defines[lastIndex++] = { "DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Interior)) {
				defines[lastIndex++] = { "INTERIOR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Wading)) {
				defines[lastIndex++] = { "WADING", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::VertexAlphaDepth)) {
				defines[lastIndex++] = { "VERTEX_ALPHA_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Cubemap)) {
				defines[lastIndex++] = { "CUBEMAP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Flowmap)) {
				defines[lastIndex++] = { "FLOWMAP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::BlendNormals)) {
				defines[lastIndex++] = { "BLEND_NORMALS", nullptr };
			}

			const auto technique = (descriptor >> 11) & 0xF;
			if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Underwater)) {
				defines[lastIndex++] = { "UNDERWATER", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Lod)) {
				defines[lastIndex++] = { "LOD", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Stencil)) {
				defines[lastIndex++] = { "STENCIL", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Simple)) {
				defines[lastIndex++] = { "SIMPLE", nullptr };
			} else if (technique < 8) {
				static constexpr std::array<const char*, 8> numLightDefines = { { "0", "1", "2", "3", "4",
					"5", "6", "7" } };
				defines[lastIndex++] = { "SPECULAR", nullptr };
				defines[lastIndex++] = { "NUM_SPECULAR_LIGHTS", numLightDefines[technique] };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Water)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetUtilityShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::UtilityShaderFlags;

			size_t lastIndex = 0;

			if (descriptor & static_cast<uint32_t>(Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Texture)) {
				defines[lastIndex++] = { "TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Skinned)) {
				defines[lastIndex++] = { "SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Normals)) {
				defines[lastIndex++] = { "NORMALS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(AlphaTest)) {
				defines[lastIndex++] = { "ALPHA_TEST", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(LodLandscape)) {
				if (descriptor &
					(static_cast<uint32_t>(RenderShadowmask) |
						static_cast<uint32_t>(RenderShadowmaskSpot))) {
					defines[lastIndex++] = { "FOCUS_SHADOW", nullptr };
				} else {
					defines[lastIndex++] = { "LOD_LANDSCAPE", nullptr };
				}
			}

			if ((descriptor & static_cast<uint32_t>(RenderNormal)) &&
				!(descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "RENDER_NORMAL", nullptr };

			} else if (!(descriptor & static_cast<uint32_t>(RenderNormal)) &&
					   (descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "RENDER_NORMAL_CLEAR", nullptr };

			} else if ((descriptor & static_cast<uint32_t>(RenderNormal)) &&
					   (descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "STENCIL_ABOVE_WATER", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(RenderNormalFalloff)) {
				defines[lastIndex++] = { "RENDER_NORMAL_FALLOFF", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderNormalClamp)) {
				defines[lastIndex++] = { "RENDER_NORMAL_CLAMP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderDepth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(OpaqueEffect)) {
				defines[lastIndex++] = { "OPAQUE_EFFECT", nullptr };

				if (!(descriptor & static_cast<uint32_t>(RenderShadowmap)) &&
					(descriptor & static_cast<uint32_t>(AdditionalAlphaMask))) {
					defines[lastIndex++] = { "ADDITIONAL_ALPHA_MASK", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(GrayscaleToAlpha)) {
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
				}
			} else {
				if (descriptor & static_cast<uint32_t>(RenderShadowmap)) {
					defines[lastIndex++] = { "RENDER_SHADOWMAP", nullptr };

					if (descriptor & static_cast<uint32_t>(RenderShadowmapPb)) {
						defines[lastIndex++] = { "RENDER_SHADOWMAP_PB", nullptr };
					}
				} else if (descriptor &
						   static_cast<uint32_t>(AdditionalAlphaMask)) {
					defines[lastIndex++] = { "ADDITIONAL_ALPHA_MASK", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(RenderShadowmapClamped)) {
					defines[lastIndex++] = { "RENDER_SHADOWMAP_CLAMPED", nullptr };
				}
			}

			if (descriptor & static_cast<uint32_t>(GrayscaleMask)) {
				defines[lastIndex++] = { "GRAYSCALE_MASK", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmask)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASK", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskSpot)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKSPOT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskPb)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKPB", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskDpb)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKDPB", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderBaseTexture)) {
				defines[lastIndex++] = { "RENDER_BASE_TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(TreeAnim)) {
				defines[lastIndex++] = { "TREE_ANIM", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(LodObject)) {
				defines[lastIndex++] = { "LOD_OBJECT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(LocalMapFogOfWar)) {
				defines[lastIndex++] = { "LOCALMAP_FOGOFWAR", nullptr };
			}

			if (descriptor & (static_cast<uint32_t>(RenderShadowmask) |
								 static_cast<uint32_t>(RenderShadowmaskDpb) |
								 static_cast<uint32_t>(RenderShadowmaskPb) |
								 static_cast<uint32_t>(RenderShadowmaskSpot))) {
				static constexpr std::array<const char*, 5> shadowFilters = { { "0", "1", "2",
					"3", "4" } };
				const size_t shadowFilterIndex = std::clamp((descriptor >> 17) & 0b111, 0u, 4u);
				defines[lastIndex++] = { "SHADOWFILTER", shadowFilters[shadowFilterIndex] };
			} else if ((!(descriptor & static_cast<uint32_t>(OpaqueEffect)) &&
						   (descriptor &
							   static_cast<uint32_t>(RenderShadowmap))) ||
					   (descriptor & static_cast<uint32_t>(RenderDepth))) {
				if (descriptor & static_cast<uint32_t>(DepthWriteDecals)) {
					defines[lastIndex++] = { "DEPTH_WRITE_DECALS", nullptr };
				}
			} else {
				if (descriptor & (static_cast<uint32_t>(DepthWriteDecals) |
									 static_cast<uint32_t>(DebugColor))) {
					defines[lastIndex++] = { "DEBUG_COLOR", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(DebugShadowSplit)) {
					defines[lastIndex++] = { "DEBUG_SHADOWSPLIT", nullptr };
				}
			}

			defines[lastIndex++] = { "SHADOWSPLITCOUNT", "3" };

			if ((descriptor & 0x14000) != 0x14000 &&
				((descriptor & 0x20004000) == 0x4000 || (descriptor & 0x1E02000) == 0x2000) &&
				!(descriptor & 0x80) && (descriptor & 0x14000) != 0x10000) {
				defines[lastIndex++] = { "NO_PIXEL_SHADER", nullptr };
			}

			defines[lastIndex++] = { nullptr, nullptr };
		}

		static void GetImagespaceShaderDefines(const RE::BSShader& shader, std::span<D3D_SHADER_MACRO> defines)
		{
			auto& isShader = const_cast<RE::BSImagespaceShader&>(static_cast<const RE::BSImagespaceShader&>(shader));
			auto* macros = reinterpret_cast<RE::BSImagespaceShader::ShaderMacro*>(defines.data());
			isShader.GetShaderMacros(macros);
			size_t lastIndex = std::ranges::find_if(defines, [](const D3D_SHADER_MACRO& macro) { return macro.Name == nullptr; }) - defines.begin();
			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::ImageSpace)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}
			defines[lastIndex] = { nullptr, nullptr };
			return;
		}

		static void GetShaderDefines(const RE::BSShader& shader, uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			switch (shader.shaderType.get()) {
			case RE::BSShader::Type::Grass:
				GetGrassShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Sky:
				GetSkyShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Water:
				GetWaterShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::BloodSplatter:
				GetBloodSplaterShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::ImageSpace:
				GetImagespaceShaderDefines(shader, defines);
				break;
			case RE::BSShader::Type::Lighting:
				GetLightingShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::DistantTree:
				GetDistantTreeShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Particle:
				GetParticleShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Effect:
				GetEffectShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Utility:
				GetUtilityShaderDefines(descriptor, defines);
				break;
			}
		}

		static std::array<std::array<std::unordered_map<std::string, int32_t>,
							  static_cast<size_t>(ShaderClass::Total)>,
			static_cast<size_t>(RE::BSShader::Type::Total)>
		GetVariableIndices()
		{
			std::array<std::array<std::unordered_map<std::string, int32_t>,
						   static_cast<size_t>(ShaderClass::Total)>,
				static_cast<size_t>(RE::BSShader::Type::Total)>
				result;

			auto& lightingVS =
				result[static_cast<size_t>(RE::BSShader::Type::Lighting)][static_cast<size_t>(ShaderClass::Vertex)];
			lightingVS = {
				{ "World", 0 },
				{ "PreviousWorld", 1 },
				{ "EyePosition", 2 },
				{ "LandBlendParams", 3 },
				{ "TreeParams", 4 },
				{ "WindTimers", 5 },
				{ "TextureProj", 6 },
				{ "IndexScale", 7 },
				{ "WorldMapOverlayParameters", 8 },
				{ "LeftEyeCenter", 9 },
				{ "RightEyeCenter", 10 },
				{ "TexcoordOffset", 11 },
				{ "HighDetailRange", 12 },
				{ "FogParam", 13 },
				{ "FogNearColor", 14 },
				{ "FogFarColor", 15 },
				{ "Bones", 16 },
			};

			const auto& lightingPSConstants = ShaderConstants::LightingPS::Get();

			auto& lightingPS = result[static_cast<size_t>(RE::BSShader::Type::Lighting)]
									 [static_cast<size_t>(ShaderClass::Pixel)];

			lightingPS = {
				{ "NumLightNumShadowLight", lightingPSConstants.NumLightNumShadowLight },
				{ "PointLightPosition", lightingPSConstants.PointLightPosition },
				{ "PointLightColor", lightingPSConstants.PointLightColor },
				{ "DirLightDirection", lightingPSConstants.DirLightDirection },
				{ "DirLightColor", lightingPSConstants.DirLightColor },
				{ "DirectionalAmbient", lightingPSConstants.DirectionalAmbient },
				{ "AmbientSpecularTintAndFresnelPower", lightingPSConstants.AmbientSpecularTintAndFresnelPower },
				{ "MaterialData", lightingPSConstants.MaterialData },
				{ "EmitColor", lightingPSConstants.EmitColor },
				{ "AlphaTestRef", lightingPSConstants.AlphaTestRef },
				{ "ShadowLightMaskSelect", lightingPSConstants.ShadowLightMaskSelect },
				{ "VPOSOffset", lightingPSConstants.VPOSOffset },
				{ "ProjectedUVParams", lightingPSConstants.ProjectedUVParams },
				{ "ProjectedUVParams2", lightingPSConstants.ProjectedUVParams2 },
				{ "ProjectedUVParams3", lightingPSConstants.ProjectedUVParams3 },
				{ "SplitDistance", lightingPSConstants.SplitDistance },
				{ "SSRParams", lightingPSConstants.SSRParams },
				{ "WorldMapOverlayParametersPS", lightingPSConstants.WorldMapOverlayParametersPS },
				{ "ShadowSampleParam", lightingPSConstants.ShadowSampleParam },      // VR only
				{ "EndSplitDistances", lightingPSConstants.EndSplitDistances },      // VR only
				{ "StartSplitDistances", lightingPSConstants.StartSplitDistances },  // VR only
				{ "DephBiasParam", lightingPSConstants.DephBiasParam },              // VR only
				{ "ShadowLightParam", lightingPSConstants.ShadowLightParam },        // VR only
				{ "ShadowMapProj", lightingPSConstants.ShadowMapProj },              // VR only
				{ "AmbientColor", lightingPSConstants.AmbientColor },
				{ "FogColor", lightingPSConstants.FogColor },
				{ "ColourOutputClamp", lightingPSConstants.ColourOutputClamp },
				{ "EnvmapData", lightingPSConstants.EnvmapData },
				{ "ParallaxOccData", lightingPSConstants.ParallaxOccData },
				{ "TintColor", lightingPSConstants.TintColor },
				{ "LODTexParams", lightingPSConstants.LODTexParams },
				{ "SpecularColor", lightingPSConstants.SpecularColor },
				{ "SparkleParams", lightingPSConstants.SparkleParams },
				{ "MultiLayerParallaxData", lightingPSConstants.MultiLayerParallaxData },
				{ "LightingEffectParams", lightingPSConstants.LightingEffectParams },
				{ "IBLParams", lightingPSConstants.IBLParams },
				{ "LandscapeTexture1to4IsSnow", lightingPSConstants.LandscapeTexture1to4IsSnow },
				{ "LandscapeTexture5to6IsSnow", lightingPSConstants.LandscapeTexture5to6IsSnow },
				{ "LandscapeTexture1to4IsSpecPower", lightingPSConstants.LandscapeTexture1to4IsSpecPower },
				{ "LandscapeTexture5to6IsSpecPower", lightingPSConstants.LandscapeTexture5to6IsSpecPower },
				{ "SnowRimLightParameters", lightingPSConstants.SnowRimLightParameters },
				{ "CharacterLightParams", lightingPSConstants.CharacterLightParams },
				{ "InvWorldMat", lightingPSConstants.InvWorldMat },            // VR only
				{ "PreviousWorldMat", lightingPSConstants.PreviousWorldMat },  // VR only

				{ "PBRFlags", lightingPSConstants.PBRFlags },
				{ "PBRParams1", lightingPSConstants.PBRParams1 },
				{ "LandscapeTexture2PBRParams", lightingPSConstants.LandscapeTexture2PBRParams },
				{ "LandscapeTexture3PBRParams", lightingPSConstants.LandscapeTexture3PBRParams },
				{ "LandscapeTexture4PBRParams", lightingPSConstants.LandscapeTexture4PBRParams },
				{ "LandscapeTexture5PBRParams", lightingPSConstants.LandscapeTexture5PBRParams },
				{ "LandscapeTexture6PBRParams", lightingPSConstants.LandscapeTexture6PBRParams },
				{ "PBRParams2", lightingPSConstants.PBRParams2 },
				{ "LandscapeTexture1GlintParameters", lightingPSConstants.LandscapeTexture1GlintParameters },
				{ "LandscapeTexture2GlintParameters", lightingPSConstants.LandscapeTexture2GlintParameters },
				{ "LandscapeTexture3GlintParameters", lightingPSConstants.LandscapeTexture3GlintParameters },
				{ "LandscapeTexture4GlintParameters", lightingPSConstants.LandscapeTexture4GlintParameters },
				{ "LandscapeTexture5GlintParameters", lightingPSConstants.LandscapeTexture5GlintParameters },
				{ "LandscapeTexture6GlintParameters", lightingPSConstants.LandscapeTexture6GlintParameters },
				{ "MaterialObjectRGBScale", lightingPSConstants.MaterialObjectRGBScale },
			};

			auto& bloodSplatterVS = result[static_cast<size_t>(RE::BSShader::Type::BloodSplatter)]
										  [static_cast<size_t>(ShaderClass::Vertex)];
			bloodSplatterVS = {
				{ "WorldViewProj", 0 },
				{ "LightLoc", 1 },
				{ "Ctrl", 2 },
			};

			auto& bloodSplatterPS = result[static_cast<size_t>(RE::BSShader::Type::BloodSplatter)]
										  [static_cast<size_t>(ShaderClass::Pixel)];
			bloodSplatterPS = {
				{ "Alpha", 0 },
			};

			auto& distantTreeVS = result[static_cast<size_t>(RE::BSShader::Type::DistantTree)]
										[static_cast<size_t>(ShaderClass::Vertex)];

			distantTreeVS = {
				{ "InstanceData", 0 },
				{ "WorldViewProj", 1 },
				{ "World", 2 },
				{ "PreviousWorld", 3 },
				{ "FogParam", 4 },
				{ "FogNearColor", 5 },
				{ "FogFarColor", 6 },
				{ "DiffuseDir", 7 },
				{ "IndexScale", 8 },
			};

			auto& distantTreePS = result[static_cast<size_t>(RE::BSShader::Type::DistantTree)]
										[static_cast<size_t>(ShaderClass::Pixel)];
			distantTreePS = {
				{ "DiffuseColor", 0 },
				{ "AmbientColor", 1 },
			};

			auto& skyVS = result[static_cast<size_t>(RE::BSShader::Type::Sky)]
								[static_cast<size_t>(ShaderClass::Vertex)];
			skyVS = {
				{ "WorldViewProj", 0 },
				{ "World", 1 },
				{ "PreviousWorld", 2 },
				{ "BlendColor", 3 },
				{ "EyePosition", 4 },
				{ "TexCoordOff", 5 },
				{ "VParams", 6 },
			};

			auto& skyPS = result[static_cast<size_t>(RE::BSShader::Type::Sky)]
								[static_cast<size_t>(ShaderClass::Pixel)];
			skyPS = {
				{ "PParams", 0 },
			};

			auto& grassVS = result[static_cast<size_t>(RE::BSShader::Type::Grass)]
								  [static_cast<size_t>(ShaderClass::Vertex)];
			grassVS = {
				{ "WorldViewProj", 0 },
				{ "WorldView", 1 },
				{ "World", 2 },
				{ "PreviousWorld", 3 },
				{ "FogNearColor", 4 },
				{ "WindVector", 5 },
				{ "WindTimer", 6 },
				{ "DirLightDirection", 7 },
				{ "PreviousWindTimer", 8 },
				{ "DirLightColor", 9 },
				{ "AlphaParam1", 10 },
				{ "AmbientColor", 11 },
				{ "AlphaParam2", 12 },
				{ "ScaleMask", 13 },
			};

			if (globals::game::isVR) {
				grassVS.insert({ "Padding", 14 });
			} else {
				grassVS.insert({ "ShadowClampValue", 14 });
			}

			const auto& grassPSConstants = ShaderConstants::GrassPS::Get();

			auto& grassPS = result[static_cast<size_t>(RE::BSShader::Type::Grass)]
								  [static_cast<size_t>(ShaderClass::Pixel)];
			grassPS = {
				{ "PBRFlags", grassPSConstants.PBRFlags },
				{ "PBRParams1", grassPSConstants.PBRParams1 },
				{ "PBRParams2", grassPSConstants.PBRParams2 },
			};

			auto& particleVS = result[static_cast<size_t>(RE::BSShader::Type::Particle)]
									 [static_cast<size_t>(ShaderClass::Vertex)];
			particleVS = {
				{ "WorldViewProj", 0 },
				{ "PrevWorldViewProj", 1 },
				{ "PrecipitationOcclusionWorldViewProj", 2 },
				{ "fVars0", 3 },
				{ "fVars1", 4 },
				{ "fVars2", 5 },
				{ "fVars3", 6 },
				{ "fVars4", 7 },
				{ "Color1", 8 },
				{ "Color2", 9 },
				{ "Color3", 10 },
				{ "Velocity", 11 },
				{ "Acceleration", 12 },
				{ "ScaleAdjust", 13 },
				{ "Wind", 14 },
			};

			auto& particlePS = result[static_cast<size_t>(RE::BSShader::Type::Particle)]
									 [static_cast<size_t>(ShaderClass::Pixel)];
			particlePS = {
				{ "ColorScale", 0 },
				{ "TextureSize", 1 },
			};

			auto& effectVS = result[static_cast<size_t>(RE::BSShader::Type::Effect)]
								   [static_cast<size_t>(ShaderClass::Vertex)];
			effectVS = {
				{ "World", 0 },
				{ "PreviousWorld", 1 },
				{ "Bones", 2 },
				{ "EyePosition", 3 },
				{ "FogParam", 4 },
				{ "FogNearColor", 5 },
				{ "FogFarColor", 6 },
				{ "FalloffData", 7 },
				{ "SoftMateralVSParams", 8 },
				{ "TexcoordOffset", 9 },
				{ "TexcoordOffsetMembrane", 10 },
				{ "SubTexOffset", 11 },
				{ "PosAdjust", 12 },
				{ "MatProj", 13 },
			};

			const auto& effectPSConstants = ShaderConstants::EffectPS::Get();

			auto& effectPS = result[static_cast<size_t>(RE::BSShader::Type::Effect)]
								   [static_cast<size_t>(ShaderClass::Pixel)];
			effectPS = {
				{ "PropertyColor", effectPSConstants.PropertyColor },
				{ "AlphaTestRef", effectPSConstants.AlphaTestRef },
				{ "MembraneRimColor", effectPSConstants.MembraneRimColor },
				{ "MembraneVars", effectPSConstants.MembraneVars },
				{ "PLightPositionX", effectPSConstants.PLightPositionX },
				{ "PLightPositionY", effectPSConstants.PLightPositionY },
				{ "PLightPositionZ", effectPSConstants.PLightPositionZ },
				{ "PLightingRadiusInverseSquared", effectPSConstants.PLightingRadiusInverseSquared },
				{ "PLightColorR", effectPSConstants.PLightColorR },
				{ "PLightColorG", effectPSConstants.PLightColorG },
				{ "PLightColorB", effectPSConstants.PLightColorB },
				{ "DLightColor", effectPSConstants.DLightColor },
				{ "VPOSOffset", effectPSConstants.VPOSOffset },
				{ "CameraDataEffect", effectPSConstants.CameraData },
				{ "FilteringParam", effectPSConstants.FilteringParam },
				{ "BaseColor", effectPSConstants.BaseColor },
				{ "BaseColorScale", effectPSConstants.BaseColorScale },
				{ "LightingInfluence", effectPSConstants.LightingInfluence },

				{ "ExtendedFlags", effectPSConstants.ExtendedFlags },
			};

			auto& waterVS = result[static_cast<size_t>(RE::BSShader::Type::Water)]
								  [static_cast<size_t>(ShaderClass::Vertex)];
			waterVS = {
				{ "WorldViewProj", 0 },
				{ "World", 1 },
				{ "PreviousWorld", 2 },
				{ "QPosAdjust", 3 },
				{ "ObjectUV", 4 },
				{ "NormalsScroll0", 5 },
				{ "NormalsScroll1", 6 },
				{ "NormalsScale", 7 },
				{ "VSFogParam", 8 },
				{ "VSFogNearColor", 9 },
				{ "VSFogFarColor", 10 },
				{ "CellTexCoordOffset", 11 },
			};

			if (!globals::game::isVR) {
				waterVS.insert(
					{
						{ "SubTexOffset", 12 },
						{ "PosAdjust", 13 },
						{ "MatProj", 14 },
					});
			}

			auto& waterPS = result[static_cast<size_t>(RE::BSShader::Type::Water)]
								  [static_cast<size_t>(ShaderClass::Pixel)];
			waterPS = {
				{ "TextureProj", 0 },
				{ "ShallowColor", 1 },
				{ "DeepColor", 2 },
				{ "ReflectionColor", 3 },
				{ "FresnelRI", 4 },
				{ "BlendRadius", 5 },
				{ "PosAdjust", 6 },
				{ "ReflectPlane", 7 },
				{ "CameraDataWater", 8 },
				{ "ProjData", 9 },
				{ "VarAmounts", 10 },
				{ "FogParam", 11 },
				{ "FogNearColor", 12 },
				{ "FogFarColor", 13 },
				{ "SunDir", 14 },
				{ "SunColor", 15 },
				{ "NumLights", 16 },
				{ "LightPos", 17 },
				{ "LightColor", 18 },
				{ "WaterParams", 19 },
				{ "DepthControl", 20 },
				{ "SSRParams", 21 },
				{ "SSRParams2", 22 },
				{ "NormalsAmplitude", 23 },
				{ "VPOSOffset", 24 },
			};

			auto& utilityVS = result[static_cast<size_t>(RE::BSShader::Type::Utility)]
									[static_cast<size_t>(ShaderClass::Vertex)];
			utilityVS = {
				{ "World", 0 },
				{ "TexcoordOffset", 1 },
				{ "EyePos", 2 },
				{ "HighDetailRange", 3 },
				{ "ParabolaParam", 4 },
				{ "ShadowFadeParam", 5 },
				{ "TreeParams", 6 },
				{ "WaterParams", 7 },
				{ "Bones", 8 },
			};

			auto& utilityPS = result[static_cast<size_t>(RE::BSShader::Type::Utility)]
									[static_cast<size_t>(ShaderClass::Pixel)];
			utilityPS = {
				{ "AlphaTestRef", 0 },
				{ "RefractionPower", 1 },
				{ "DebugColor", 2 },
				{ "BaseColor", 3 },
				{ "PropertyColor", 4 },
				{ "FocusShadowMapProj", 5 },
				{ "ShadowMapProj", 6 },
				{ "ShadowSampleParam", 7 },
				{ "ShadowLightParam", 8 },
			};

			if (!globals::game::isVR) {
				utilityPS.insert(
					{
						{ "ShadowFadeParam", 9 },
						{ "VPOSOffset", 10 },
						{ "EndSplitDistances", 11 },
						{ "StartSplitDistances", 12 },
						{ "FocusShadowFadeParam", 13 },
					});
			} else {
				utilityPS.insert(
					{
						{ "StereoClipRects", 9 },  // VR only
						{ "ShadowFadeParam", 10 },
						{ "VPOSOffset", 11 },
						{ "EndSplitDistances", 12 },
						{ "StartSplitDistances", 13 },
						{ "FocusShadowFadeParam", 14 },
					});
			}

			return result;
		}

		static int32_t GetVariableIndex(ShaderClass shaderClass, const RE::BSShader& shader, const char* name)
		{
			if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
				const auto& imagespaceShader = static_cast<const RE::BSImagespaceShader&>(shader);

				if (shaderClass == ShaderClass::Vertex) {
					for (size_t nameIndex = 0; nameIndex < imagespaceShader.vsConstantNames.size();
						++nameIndex) {
						if (std::string_view(imagespaceShader.vsConstantNames[static_cast<uint32_t>(nameIndex)].c_str()) ==
							name) {
							return static_cast<int32_t>(nameIndex);
						}
					}
				} else if (shaderClass == ShaderClass::Pixel || shaderClass == ShaderClass::Compute) {
					for (size_t nameIndex = 0; nameIndex < imagespaceShader.psConstantNames.size(); ++nameIndex) {
						if (std::string_view(imagespaceShader.psConstantNames[static_cast<uint32_t>(nameIndex)].c_str()) == name) {
							return static_cast<int32_t>(nameIndex);
						}
					}
				}
			} else {
				static auto variableNames = GetVariableIndices();

				const auto& names = variableNames[static_cast<size_t>(shader.shaderType.get())]
												 [static_cast<size_t>(shaderClass)];
				auto it = names.find(name);
				if (it != names.cend()) {
					return it->second;
				}
			}
			return -1;
		}

		static std::string MergeDefinesString(std::array<D3D_SHADER_MACRO, 64>& defines, bool a_sort = false)
		{
			std::string result;
			// a.Name/b.Name are char*, so comparing them directly sorts by
			// address, not content -- non-deterministic across runs. Null
			// names (unused slots) always sort last.
			if (a_sort)
				std::sort(std::begin(defines), std::end(defines), [](const D3D_SHADER_MACRO& a, const D3D_SHADER_MACRO& b) {
					if (a.Name == nullptr || b.Name == nullptr)
						return a.Name != nullptr;
					return std::strcmp(a.Name, b.Name) < 0;
				});
			for (const auto& def : defines) {
				if (def.Name != nullptr) {
					result += def.Name;
					if (def.Definition != nullptr && !std::string_view(def.Definition).empty()) {
						result += "=";
						result += def.Definition;
					}
					result += ' ';
				} else {
					break;
				}
			}
			return result;
		}

		static void AddAttribute(uint64_t& desc, RE::BSGraphics::Vertex::Attribute attribute)
		{
			desc |= ((1ull << (44 + attribute)) | (1ull << (54 + attribute)) |
					 (0b1111ull << (4 * attribute + 4)));
		}

		template <size_t MaxOffsetsSize>
		static void ReflectConstantBuffers(ID3D11ShaderReflection& reflector,
			std::array<size_t, 3>& bufferSizes,
			std::array<int8_t, MaxOffsetsSize>& constantOffsets,
			uint64_t& vertexDesc,
			ShaderClass shaderClass, uint32_t descriptor, const RE::BSShader& shader)
		{
			D3D11_SHADER_DESC desc;
			if (FAILED(reflector.GetDesc(&desc))) {
				logger::error("Failed to get shader descriptor for {} shader {}::{:X}",
					magic_enum::enum_name(shaderClass), magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				return;
			}

			if (shaderClass == ShaderClass::Vertex) {
				vertexDesc = 0b1111;
				bool hasTexcoord2 = false;
				bool hasTexcoord3 = false;
				for (uint32_t inputIndex = 0; inputIndex < desc.InputParameters; ++inputIndex) {
					D3D11_SIGNATURE_PARAMETER_DESC inputDesc;
					if (FAILED(reflector.GetInputParameterDesc(inputIndex, &inputDesc))) {
						logger::error(
							"Failed to get input parameter {} descriptor for {} shader {}::{:X}",
							inputIndex, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shader.shaderType.get()),
							descriptor);
					} else {
						std::string_view semanticName = inputDesc.SemanticName;
						if (semanticName == "POSITION" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_POSITION);
						} else if (semanticName == "TEXCOORD" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD0);
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex == 1) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD1);
						} else if (semanticName == "NORMAL" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_NORMAL);
						} else if (semanticName == "BINORMAL" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_BINORMAL);
						} else if (semanticName == "COLOR" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_COLOR);
						} else if (semanticName == "BLENDWEIGHT" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_SKINNING);
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex >= 4 &&
								   inputDesc.SemanticIndex <= 7) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_INSTANCEDATA);
						} else if (semanticName == "TEXCOORD" &&
								   inputDesc.SemanticIndex == 2) {
							hasTexcoord2 = true;
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex == 3) {
							hasTexcoord3 = true;
						}
					}
				}
				if (hasTexcoord2) {
					if (hasTexcoord3) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_LANDDATA);
					} else {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_EYEDATA);
					}
				}
			}

			if (desc.ConstantBuffers <= 0) {
				return;
			}

			auto mapBufferConsts =
				[&](const char* bufferName, size_t& bufferSize) {
					auto bufferReflector = reflector.GetConstantBufferByName(bufferName);
					if (bufferReflector == nullptr) {
						logger::trace("Buffer {} not found for {} shader {}::{:X}",
							bufferName, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shader.shaderType.get()),
							descriptor);
						return;
					}

					D3D11_SHADER_BUFFER_DESC bufferDesc;
					if (FAILED(bufferReflector->GetDesc(&bufferDesc))) {
						logger::trace("Failed to get buffer {} descriptor for {} shader {}::{:X}",
							bufferName, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shader.shaderType.get()),
							descriptor);
						return;
					}

					for (uint32_t i = 0; i < bufferDesc.Variables; i++) {
						ID3D11ShaderReflectionVariable* var = bufferReflector->GetVariableByIndex(i);

						D3D11_SHADER_VARIABLE_DESC varDesc;
						if (FAILED(var->GetDesc(&varDesc))) {
							logger::trace("Failed to get variable descriptor for {} shader {}::{:X}",
								magic_enum::enum_name(shaderClass), magic_enum::enum_name(shader.shaderType.get()),
								descriptor);
							continue;
						}

						const auto variableIndex =
							GetVariableIndex(shaderClass, shader, varDesc.Name);
						const bool variableFound = variableIndex != -1;
						if (variableFound) {
							constantOffsets[variableIndex] = (int8_t)(varDesc.StartOffset / 4);
						} else {
							logger::trace("Unknown variable name {} in {} shader {}::{:X}",
								varDesc.Name, magic_enum::enum_name(shaderClass),
								magic_enum::enum_name(shader.shaderType.get()),
								descriptor);
						}

						if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
							D3D11_SHADER_TYPE_DESC varTypeDesc;
							var->GetType()->GetDesc(&varTypeDesc);
							if (varTypeDesc.Elements > 0) {
								if (!variableFound) {
									const std::string arrayName =
										std::format("{}[{}]", varDesc.Name, varTypeDesc.Elements);
									const auto variableArrayIndex =
										GetVariableIndex(shaderClass, shader, arrayName.c_str());
									if (variableArrayIndex != -1) {
										constantOffsets[variableArrayIndex] = static_cast<int8_t>(varDesc.StartOffset / 4);
									} else {
										logger::debug("Unknown variable name {} in {} shader {}::{:X}",
											arrayName, magic_enum::enum_name(shaderClass),
											magic_enum::enum_name(shader.shaderType.get()), descriptor);
									}
								} else {
									const auto elementSize = varDesc.Size / varTypeDesc.Elements;
									for (uint32_t arrayIndex = 1; arrayIndex < varTypeDesc.Elements;
										++arrayIndex) {
										const std::string varName =
											std::format("{}[{}]", varDesc.Name, arrayIndex);
										const auto variableArrayElementIndex =
											GetVariableIndex(shaderClass, shader, varName.c_str());
										if (variableArrayElementIndex != -1) {
											constantOffsets[variableArrayElementIndex] =
												static_cast<int8_t>((varDesc.StartOffset + elementSize * arrayIndex) / 4);
										} else {
											logger::debug(
												"Unknown variable name {} in {} shader {}::{:X}", varName,
												magic_enum::enum_name(shaderClass),
												magic_enum::enum_name(shader.shaderType.get()),
												descriptor);
										}
									}
								}
							}
						}
					}

					bufferSize = ((bufferDesc.Size + 15) & ~15) / 16;
				};

			mapBufferConsts("PerTechnique", bufferSizes[0]);
			mapBufferConsts("PerMaterial", bufferSizes[1]);
			mapBufferConsts("PerGeometry", bufferSizes[2]);
		}

		std::wstring GetDiskPath(const std::string_view& name, uint32_t descriptor, ShaderClass shaderClass)
		{
			const auto suffixNarrow = Util::GetShaderDefinesSuffix(globals::state->shaderDefinesString);
			const std::wstring suffix(suffixNarrow.begin(), suffixNarrow.end());

			const auto wname = std::wstring(name.begin(), name.end());
			switch (shaderClass) {
			case ShaderClass::Pixel:
				return std::format(L"Data/ShaderCache/{}/{:X}{}.pso", wname, descriptor, suffix);
			case ShaderClass::Vertex:
				return std::format(L"Data/ShaderCache/{}/{:X}{}.vso", wname, descriptor, suffix);
			case ShaderClass::Compute:
				return std::format(L"Data/ShaderCache/{}/{:X}{}.cso", wname, descriptor, suffix);
			}
			return {};
		}

		static std::string GetShaderString(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, bool hashkey)
		{
			auto sourceShaderFile = shader.fxpFilename;
			std::array<D3D_SHADER_MACRO, 64> defines{};
			SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });
			std::string result;
			if (hashkey)  // generate hashkey so don't include descriptor
				result = fmt::format("{}:{}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), SIE::SShaderCache::MergeDefinesString(defines, true));
			else
				result = fmt::format("{}:{}:{:X}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), descriptor, SIE::SShaderCache::MergeDefinesString(defines, true));
			return result;
		}

		std::string GetTypeFromShaderString(const std::string& a_key)
		{
			std::string type = "";
			std::string::size_type pos = a_key.find(':');
			if (pos != std::string::npos)
				type = a_key.substr(0, pos);
			if (type.starts_with("IS") || type == "ReflectionsRayTracing")
				type = "ImageSpace";  // fix type for image space shaders
			return type;
		}

		/**
		 * @brief Compiles or retrieves a cached shader.
		 *
		 * Checks the in-memory shader cache, then the disk cache (if enabled and valid),
		 * and compiles from HLSL source if necessary. Records include dependencies for
		 * hot-reload invalidation.
		 *
		 * @param useDiskCache Whether to use disk cache for reading and writing compiled shaders.
		 * @param dependencyTracker Optional tracker for include dependency registration;
		 *                          enables cache invalidation when dependencies change.
		 *
		 * @return Compiled shader blob, or nullptr if compilation failed or source file not found.
		 */
		static ID3DBlob* CompileShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, bool useDiskCache, ShaderFileDependencyTracker* dependencyTracker)
		{
			if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
				return nullptr;
			}

			auto& cache = ShaderCache::Instance();
			auto key = SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);

			// Atomically check the shaderMap and either:
			//  - return the blob if already Completed (cache hit),
			//  - wait if another thread is compiling (Pending),
			//  - claim the slot with Pending if nobody started yet.
			auto [claimResult, cachedBlob] = cache.ClaimCompilation(key);
			if (claimResult == ShaderCache::ClaimResult::CacheHit) {
				cache.IncCacheHitTasks();
				return cachedBlob;
			}

			const auto type = shader.shaderType.get();

			// check diskcache
			auto diskPath = GetDiskPath(shader.fxpFilename, descriptor, shaderClass);
			ID3DBlob* shaderBlob = nullptr;

			if (useDiskCache && std::filesystem::exists(diskPath)) {
				// Determine whether the disk-cached shader is still valid.
				bool diskCacheOutdated = false;

				// Manifest-first: a recorded digest is authoritative, falling back
				// to the mtime checks below only when no digest is on record yet.
				bool decidedByDigest = false;
				const std::wstring shaderSourcePath = GetShaderPath(
					shader.shaderType == RE::BSShader::Type::ImageSpace ?
						static_cast<const RE::BSImagespaceShader&>(shader).originalShaderName :
						shader.fxpFilename);
				// Manifest lookup is a cheap map Get; gate the expensive digest walk
				// (file read + full include-closure hash) on it actually finding
				// something to compare against, not the other way around -- a blob
				// with no recorded entry yet (e.g. a cache from before this manifest
				// existed) would otherwise pay that cost every boot, forever.
				if (const auto recorded = GetShaderCacheManifest().Get(GetManifestKey(diskPath))) {
					if (std::filesystem::exists(shaderSourcePath)) {
						if (const auto digest = GetShaderContentDigestTimed(shaderSourcePath, std::filesystem::path(shaderSourcePath).parent_path(), cache)) {
							decidedByDigest = true;
							cache.IncDigestDecidedTasks();
							const auto combined = Util::ContentHash::CombineHashes(*digest, GetGlobalDefinesDigest());
							diskCacheOutdated = *recorded != combined.ToHex();
							if (diskCacheOutdated)
								logger::debug("Disk-cached shader {} outdated: content digest changed", SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true));
						}
					}
				}

				if (!decidedByDigest && cache.UseFileWatcher()) {
					// File watcher tracks runtime changes in memory: compare disk-cache mtime against tracked source mtime.
					auto diskCacheTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(diskPath));
					diskCacheOutdated = cache.ShaderModifiedSince(shader.fxpFilename, diskCacheTime);
					if (diskCacheOutdated)
						logger::debug("Diskcached shader {} older than {}", SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true), std::format("{:%Y%m%d%H%M}", diskCacheTime));
				} else if (!decidedByDigest && cache.IsSkipUnchangedShaders()) {
					// Compare disk cache mtime against max mtime over the entire include tree to handle shared include changes.
					std::error_code ec;
					const auto diskCacheTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(diskPath, ec));
					if (ec) {
						logger::debug("Failed to read disk cache mtime for {}: {}", Util::WStringToString(diskPath), ec.message());
					} else if (std::filesystem::exists(shaderSourcePath)) {
						const auto sourceTime = GetMaxShaderMTime(shaderSourcePath, std::filesystem::path(shaderSourcePath).parent_path());
						if (sourceTime > diskCacheTime) {
							diskCacheOutdated = true;
							logger::debug("Disk-cached shader {} outdated: source is newer than cache", SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true));
						}
					}
				}

				if (diskCacheOutdated) {
					// Fall through to recompile from source.
				} else if (FAILED(D3DReadFileToBlob(diskPath.c_str(), &shaderBlob))) {
					logger::error("Failed to load {} shader {}::{:X}", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor);

					if (shaderBlob != nullptr) {
						shaderBlob->Release();
					}
				} else {
					logger::debug("Loaded shader from {}", Util::WStringToString(diskPath));
					cache.AddCompletedShader(shaderClass, shader, descriptor, shaderBlob, /*fromDisk=*/true);
					return shaderBlob;
				}
			}

			// prepare preprocessor defines
			std::array<D3D_SHADER_MACRO, 64> defines{};
			auto lastIndex = 0;
			if (shaderClass == ShaderClass::Vertex) {
				defines[lastIndex++] = { "VSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Pixel) {
				defines[lastIndex++] = { "PSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Compute) {
				defines[lastIndex++] = { "CSHADER", nullptr };
			}
			if (globals::state->IsDeveloperMode()) {
				defines[lastIndex++] = { "D3DCOMPILE_SKIP_OPTIMIZATION", nullptr };
				defines[lastIndex++] = { "D3DCOMPILE_DEBUG", nullptr };
			}
			if (globals::game::isVR)
				defines[lastIndex++] = { "VR", nullptr };
			auto shaderDefines = globals::state->GetDefines();
			if (!shaderDefines->empty()) {
				for (unsigned int i = 0; i < shaderDefines->size(); i++)
					defines[lastIndex++] = { shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() };
			}
			defines[lastIndex] = { nullptr, nullptr };  // do final entry
			GetShaderDefines(shader, descriptor, std::span{ defines }.subspan(lastIndex));

			const std::wstring path = GetShaderPath(
				shader.shaderType == RE::BSShader::Type::ImageSpace ?
					static_cast<const RE::BSImagespaceShader&>(shader).originalShaderName :
					shader.fxpFilename);
			auto pathString = Util::WStringToString(path);
			if (!std::filesystem::exists(path)) {
				logger::error("Failed to compile {} shader {}::{:X}: {} does not exist", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor, pathString);
				cache.AddCompletedShader(shaderClass, shader, descriptor, nullptr);
				return nullptr;
			}
			logger::debug("Compiling {} {}:{}:{:X} to {}", pathString, magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor, MergeDefinesString(defines));

			// compile shaders — match Utils/D3D.cpp CompileShader flag policy (strictness, optional toggles, validation).
			ID3DBlob* errorBlob = nullptr;
			uint32_t flags = !globals::state->IsDeveloperMode() ? (D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3) : D3DCOMPILE_DEBUG;
			if (globals::state->enablePartialPrecision.load(std::memory_order_relaxed)) {
				flags |= D3DCOMPILE_PARTIAL_PRECISION;
			}
			if (globals::state->enableAvoidFlowControl.load(std::memory_order_relaxed)) {
				flags |= D3DCOMPILE_AVOID_FLOW_CONTROL;
			}
			if (useDiskCache) {
				flags |= D3DCOMPILE_SKIP_VALIDATION;
			}

			// Track includes
			TrackingIncludeHandler includeHandler(std::filesystem::path(path).parent_path());
			const HRESULT compileResult = D3DCompileFromFile(path.c_str(), defines.data(), &includeHandler, "main",
				GetShaderProfile(shaderClass), flags, 0, &shaderBlob, &errorBlob);
			// If the include handler captured any includes, register them so the watcher
			// can invalidate dependents even if this compilation fails. Do NOT clear
			// mappings when there are no captured includes to avoid removing prior
			// dependency information on transient failures.
			if (dependencyTracker && !includeHandler.includes.empty()) {
				dependencyTracker->RegisterDependencies(Util::WStringToString(path), includeHandler.includes);
			}

			if (FAILED(compileResult)) {
				if (errorBlob != nullptr) {
					logger::error("Failed to compile {} shader {}::{:X}:\n{}",
						magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor,
						static_cast<char*>(errorBlob->GetBufferPointer()));
					errorBlob->Release();
				} else {
					logger::error("Failed to compile {} shader {}::{:X}",
						magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor);
				}
				if (shaderBlob != nullptr) {
					shaderBlob->Release();
				}

#ifdef TRACY_ENABLE
				{
					// Timeline annotation: a (re)compile failed. Pairs with the
					// MCP shadercache status (failedTasks) for build-agnostic
					// detection; this gives the exact frame for perf correlation.
					const auto tracyMsg = std::format("Shader compile FAILED: {} {} {:X}",
						magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);
					TracyMessageC(tracyMsg.c_str(), tracyMsg.size(), 0xFF4444);
				}
#endif

				cache.AddCompletedShader(shaderClass, shader, descriptor, nullptr);
				return nullptr;
			}
			if (errorBlob)
				logger::debug("Shader logs:\n{}", static_cast<char*>(errorBlob->GetBufferPointer()));
			logger::debug("Compiled shader {}:{}:{:X}", magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);

#ifdef TRACY_ENABLE
			{
				// Timeline annotation: a shader (re)compiled successfully. During
				// a hot-reload this marks the exact frame the new shader went
				// live, so A/B perf windows split precisely on it.
				const auto tracyMsg = std::format("Shader compiled: {} {} {:X}",
					magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);
				TracyMessage(tracyMsg.c_str(), tracyMsg.size());
			}
#endif

			// strip debug info
			if (!globals::state->IsDeveloperMode()) {
				ID3DBlob* strippedShaderBlob = nullptr;

				const uint32_t stripFlags = D3DCOMPILER_STRIP_DEBUG_INFO |
				                            D3DCOMPILER_STRIP_TEST_BLOBS |
				                            D3DCOMPILER_STRIP_PRIVATE_DATA;

				D3DStripShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), stripFlags, &strippedShaderBlob);
				std::swap(shaderBlob, strippedShaderBlob);
				strippedShaderBlob->Release();
			}

			// save shader to disk
			if (useDiskCache) {
				auto directoryPath = std::format("Data/ShaderCache/{}", shader.fxpFilename);
				if (!std::filesystem::is_directory(directoryPath)) {
					try {
						std::filesystem::create_directories(directoryPath);
					} catch (std::filesystem::filesystem_error const& ex) {
						logger::error("Failed to create folder: {}", ex.what());
					}
				}

				const HRESULT saveResult = D3DWriteBlobToFile(shaderBlob, diskPath.c_str(), true);
				if (FAILED(saveResult)) {
					logger::error("Failed to save shader to {}", Util::WStringToString(diskPath));
				} else {
					logger::debug("Saved shader to {}", Util::WStringToString(diskPath));
					// Record the digest of what just got compiled; the manifest-first
					// check above reads this back to decide disk-cache validity.
					if (const auto digest = GetShaderContentDigestTimed(path, std::filesystem::path(path).parent_path(), cache)) {
						const auto combined = Util::ContentHash::CombineHashes(*digest, GetGlobalDefinesDigest());
						RecordDigestAndMaybeFlush(GetShaderCacheManifest(), GetManifestKey(diskPath), combined.ToHex());
					}
				}
			}
			cache.AddCompletedShader(shaderClass, shader, descriptor, shaderBlob);
			return shaderBlob;
		}

		std::unique_ptr<RE::BSGraphics::VertexShader> CreateVertexShader(ID3DBlob& shaderData,
			const RE::BSShader& shader, uint32_t descriptor)
		{
			static const auto perTechniqueBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524755, 411371));
			static const auto perMaterialBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524757, 411373));
			static const auto perGeometryBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524759, 411375));
			static const auto bufferData = REL::Relocation<void*>(RELOCATION_ID(524965, 411446));

			auto rawPtr =
				new uint8_t[sizeof(RE::BSGraphics::VertexShader) + shaderData.GetBufferSize()];
			auto shaderPtr = new (rawPtr) RE::BSGraphics::VertexShader;
			memcpy(rawPtr + sizeof(RE::BSGraphics::VertexShader), shaderData.GetBufferPointer(),
				shaderData.GetBufferSize());
			std::unique_ptr<RE::BSGraphics::VertexShader> newShader{ shaderPtr };
			newShader->byteCodeSize = (uint32_t)shaderData.GetBufferSize();
			newShader->id = descriptor;
			newShader->vertexDesc = 0;

			winrt::com_ptr<ID3D11ShaderReflection> reflector;
			const auto reflectionResult = D3DReflect(shaderData.GetBufferPointer(), shaderData.GetBufferSize(),
				IID_PPV_ARGS(&reflector));
			if (FAILED(reflectionResult)) {
				logger::error("Failed to reflect vertex shader {}::{:X}", magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
				std::fill(newShader->constantTable.begin(), newShader->constantTable.end(), static_cast<uint8_t>(0));
				ReflectConstantBuffers(*reflector.get(), bufferSizes, newShader->constantTable, newShader->vertexDesc,
					ShaderClass::Vertex, descriptor, shader);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(REX::W32::ID3D11Buffer*)perTechniqueBuffersArray.get()[bufferSizes[0]];
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(REX::W32::ID3D11Buffer*)perMaterialBuffersArray.get()[bufferSizes[1]];
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(REX::W32::ID3D11Buffer*)perGeometryBuffersArray.get()[bufferSizes[2]];
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}

		std::unique_ptr<RE::BSGraphics::PixelShader> CreatePixelShader(ID3DBlob& shaderData,
			const RE::BSShader& shader, uint32_t descriptor)
		{
			static const auto perTechniqueBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524761, 411377));
			static const auto perMaterialBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524763, 411379));
			static const auto perGeometryBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524765, 411381));
			static const auto bufferData = REL::Relocation<void*>(RELOCATION_ID(524967, 411448));

			auto newShader = std::make_unique<RE::BSGraphics::PixelShader>();
			newShader->id = descriptor;

			winrt::com_ptr<ID3D11ShaderReflection> reflector;
			const auto reflectionResult = D3DReflect(shaderData.GetBufferPointer(),
				shaderData.GetBufferSize(), IID_PPV_ARGS(&reflector));
			if (FAILED(reflectionResult)) {
				logger::error("Failed to reflect vertex shader {}::{:X}", magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
				std::ranges::fill(newShader->constantTable, (int8_t)0);
				uint64_t dummy;
				ReflectConstantBuffers(*reflector.get(), bufferSizes, newShader->constantTable,
					dummy,
					ShaderClass::Pixel, descriptor, shader);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(REX::W32::ID3D11Buffer*)perTechniqueBuffersArray.get()[bufferSizes[0]];
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(REX::W32::ID3D11Buffer*)perMaterialBuffersArray.get()[bufferSizes[1]];
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(REX::W32::ID3D11Buffer*)perGeometryBuffersArray.get()[bufferSizes[2]];
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}

		std::unique_ptr<RE::BSGraphics::ComputeShader> CreateComputeShader([[maybe_unused]] ID3DBlob& shaderData,
			[[maybe_unused]] const RE::BSShader& shader, uint32_t descriptor)
		{
			auto newShader = std::make_unique<RE::BSGraphics::ComputeShader>();
			newShader->id = descriptor;
			return newShader;
		}

		static bool GetImagespaceShaderDescriptor(const RE::BSImagespaceShader& imagespaceShader, uint32_t& descriptor)
		{
			using enum RE::ImageSpaceManager::ImageSpaceEffectEnum;

			static const ankerl::unordered_dense::map<std::string_view, uint32_t> descriptors{
				// { "BSImagespaceShaderISBlur", RE::ImageSpaceManager::GetCurrentIndex(ISBlur) },
				// { "BSImagespaceShaderBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISBlur3) },
				// { "BSImagespaceShaderBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISBlur5) },
				// { "BSImagespaceShaderBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISBlur7) },
				// { "BSImagespaceShaderBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISBlur9) },
				// { "BSImagespaceShaderBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISBlur11) },
				// { "BSImagespaceShaderBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISBlur13) },
				// { "BSImagespaceShaderBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISBlur15) },
				// { "BSImagespaceShaderBrightPassBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur3) },
				// { "BSImagespaceShaderBrightPassBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur5) },
				// { "BSImagespaceShaderBrightPassBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur7) },
				// { "BSImagespaceShaderBrightPassBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur9) },
				// { "BSImagespaceShaderBrightPassBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur11) },
				// { "BSImagespaceShaderBrightPassBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur13) },
				// { "BSImagespaceShaderBrightPassBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur15) },
				// { "BSImagespaceShaderNonHDRBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur3) },
				// { "BSImagespaceShaderNonHDRBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur5) },
				// { "BSImagespaceShaderNonHDRBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur7) },
				// { "BSImagespaceShaderNonHDRBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur9) },
				// { "BSImagespaceShaderNonHDRBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur11) },
				// { "BSImagespaceShaderNonHDRBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur13) },
				// { "BSImagespaceShaderNonHDRBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur15) },
				// { "BSImagespaceShaderISBasicCopy", RE::ImageSpaceManager::GetCurrentIndex(ISBasicCopy) },
				// { "BSImagespaceShaderISSimpleColor", RE::ImageSpaceManager::GetCurrentIndex(ISSimpleColor) },
				// { "BSImagespaceShaderApplyReflections", RE::ImageSpaceManager::GetCurrentIndex(ISApplyReflections) },
				// { "BSImagespaceShaderISExp", RE::ImageSpaceManager::GetCurrentIndex(ISExp) },
				// { "BSImagespaceShaderISDisplayDepth", RE::ImageSpaceManager::GetCurrentIndex(ISDisplayDepth) },
				// { "BSImagespaceShaderAlphaBlend", RE::ImageSpaceManager::GetCurrentIndex(ISAlphaBlend) },
				// { "BSImagespaceShaderWaterFlow", RE::ImageSpaceManager::GetCurrentIndex(ISWaterFlow) },
				{ "BSImagespaceShaderISWaterBlend", RE::ImageSpaceManager::GetCurrentIndex(ISWaterBlend) },
				// { "BSImagespaceShaderGreyScale", RE::ImageSpaceManager::GetCurrentIndex(ISCopyGrayScale) },
				// { "BSImagespaceShaderCopy", RE::ImageSpaceManager::GetCurrentIndex(ISCopy) },
				// { "BSImagespaceShaderCopyScaleBias", RE::ImageSpaceManager::GetCurrentIndex(ISCopyScaleBias) },
				// { "BSImagespaceShaderCopyCustomViewport",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISCopyCustomViewport) },
				// { "BSImagespaceShaderCopyTextureMask", RE::ImageSpaceManager::GetCurrentIndex(ISCopyTextureMask) },
				// { "BSImagespaceShaderCopyDynamicFetchDisabled",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISCopyDynamicFetchDisabled) },
				{ "BSImagespaceShaderISCompositeVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeVolumetricLighting) },
				{ "BSImagespaceShaderISCompositeLensFlare",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeLensFlare) },
				{ "BSImagespaceShaderISCompositeLensFlareVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeLensFlareVolumetricLighting) },
				// { "BSImagespaceShaderISDebugSnow", RE::ImageSpaceManager::GetCurrentIndex(ISDebugSnow) },
				{ "BSImagespaceShaderDepthOfField", RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfField) },
				{ "BSImagespaceShaderDepthOfFieldFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfFieldFogged) },
				{ "BSImagespaceShaderDepthOfFieldMaskedFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfFieldMaskedFogged) },
				// { "BSImagespaceShaderDistantBlur", RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlur) },
				// { "BSImagespaceShaderDistantBlurFogged",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlurFogged) },
				// { "BSImagespaceShaderDistantBlurMaskedFogged",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlurMaskedFogged) },
				// { "BSImagespaceShaderDoubleVision", RE::ImageSpaceManager::GetCurrentIndex(ISDoubleVision) },
				{ "BSImagespaceShaderISDownsample", RE::ImageSpaceManager::GetCurrentIndex(ISDownsample) },
				{ "BSImagespaceShaderISDownsampleIgnoreBrightest",
					RE::ImageSpaceManager::GetCurrentIndex(ISDownsampleIgnoreBrightest) },
				// { "BSImagespaceShaderISUpsampleDynamicResolution",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISUpsampleDynamicResolution) },
				{ "BSImageSpaceShaderVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLighting) },
				{ "BSImagespaceShaderHDRDownSample4", RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4) },
				{ "BSImagespaceShaderHDRDownSample4LightAdapt",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4LightAdapt) },
				{ "BSImagespaceShaderHDRDownSample4LumClamp",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4LumClamp) },
				{ "BSImagespaceShaderHDRDownSample4RGB2Lum",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4RGB2Lum) },
				{ "BSImagespaceShaderHDRDownSample16", RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16) },
				{ "BSImagespaceShaderHDRDownSample16LightAdapt",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16LightAdapt) },
				{ "BSImagespaceShaderHDRDownSample16Lum",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16Lum) },
				{ "BSImagespaceShaderHDRDownSample16LumClamp",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16LumClamp) },
				{ "BSImagespaceShaderHDRTonemapBlendCinematic",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematic) },
				{ "BSImagespaceShaderHDRTonemapBlendCinematicFade",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematicFade) },
				// { "BSImagespaceShaderISIBLensFlares", RE::ImageSpaceManager::GetCurrentIndex(ISIBLensFlares) },

				// Those cause issue because of typo in shader name in vanilla code but at the same time they are not used by vanilla game.
				// { "BSImagespaceShaderISLightingComposite",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingComposite) },
				// { "BSImagespaceShaderISLightingCompositeMenu",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingCompositeMenu) },
				// { "BSImagespaceShaderISLightingCompositeNoDirectionalLight",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingCompositeNoDirectionalLight) },

				// { "BSImagespaceShaderLocalMap", RE::ImageSpaceManager::GetCurrentIndex(ISLocalMap) },
				// { "BSISWaterBlendHeightmaps", RE::ImageSpaceManager::GetCurrentIndex(ISWaterBlendHeightmaps) },
				// { "BSISWaterDisplacementClearSimulation",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementClearSimulation) },
				// { "BSISWaterDisplacementNormals",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementNormals) },
				// { "BSISWaterDisplacementRainRipple",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementRainRipple) },
				// { "BSISWaterDisplacementTexOffset",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementTexOffset) },
				// { "BSISWaterWadingHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterWadingHeightmap) },
				// { "BSISWaterRainHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterRainHeightmap) },
				// { "BSISWaterSmoothHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterSmoothHeightmap) },
				// { "BSISWaterWadingHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterWadingHeightmap) },
				// { "BSImagespaceShaderMap", RE::ImageSpaceManager::GetCurrentIndex(ISMap) },
				// { "BSImagespaceShaderMap", RE::ImageSpaceManager::GetCurrentIndex(ISMap) },
				// { "BSImagespaceShaderWorldMap", RE::ImageSpaceManager::GetCurrentIndex(ISWorldMap) },
				// { "BSImagespaceShaderWorldMapNoSkyBlur",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWorldMapNoSkyBlur) },
				// { "BSImagespaceShaderISMinify", RE::ImageSpaceManager::GetCurrentIndex(ISMinify) },
				// { "BSImagespaceShaderISMinifyContrast", RE::ImageSpaceManager::GetCurrentIndex(ISMinifyContrast) },
				// { "BSImagespaceShaderNoiseNormalmap", RE::ImageSpaceManager::GetCurrentIndex(ISNoiseNormalmap) },
				// { "BSImagespaceShaderNoiseScrollAndBlend",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISNoiseScrollAndBlend) },
				// { "BSImagespaceShaderRadialBlur",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlur) },
				// { "BSImagespaceShaderRadialBlurHigh", RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlurHigh) },
				// { "BSImagespaceShaderRadialBlurMedium", RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlurMedium) },
				{ "BSImagespaceShaderRefraction", RE::ImageSpaceManager::GetCurrentIndex(ISRefraction) },
				{ "BSImagespaceShaderISSAOCompositeSAO", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeSAO) },
				{ "BSImagespaceShaderISSAOCompositeFog", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeFog) },
				{ "BSImagespaceShaderISSAOCompositeSAOFog", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeSAOFog) },
				// { "BSImagespaceShaderISSAOCameraZ", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCameraZ) },
				// { "BSImagespaceShaderISSILComposite", RE::ImageSpaceManager::GetCurrentIndex(ISSILComposite) },
				// { "BSImagespaceShaderISSnowSSS", RE::ImageSpaceManager::GetCurrentIndex(ISSnowSSS) },
				// { "BSImagespaceShaderISSAOBlurH", RE::ImageSpaceManager::GetCurrentIndex(ISSAOBlurH) },
				// { "BSImagespaceShaderISSAOBlurV", RE::ImageSpaceManager::GetCurrentIndex(ISSAOBlurV) },
				// { "BSImagespaceShaderISUnderwaterMask", RE::ImageSpaceManager::GetCurrentIndex(ISUnderwaterMask) },
				{ "BSImagespaceShaderISApplyVolumetricLighting", RE::ImageSpaceManager::GetCurrentIndex(ISApplyVolumetricLighting) },
				{ "BSImagespaceShaderReflectionsRayTracing", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionsRayTracing) },
				//{ "BSImagespaceShaderReflectionsDebugSpecMask", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionsDebugSpecMask) },
				{ "BSImagespaceShaderISTemporalAA", RE::ImageSpaceManager::GetCurrentIndex(ISTemporalAA) },
				{ "BSImagespaceShaderVolumetricLightingRaymarchCS", 256 },
				{ "BSImagespaceShaderVolumetricLightingGenerateCS", 257 },
				{ "BSImagespaceShaderVolumetricLightingBlurHCS", RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLightingBlurHCS) },
				{ "BSImagespaceShaderVolumetricLightingBlurVCS", RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLightingBlurVCS) },

				// VR only shaders
				// Disable BSImagespaceShaderCopyDepthBuffer since we don't have it REed and it causes issues with cache and upscaling
				// https://github.com/community-shaders/skyrim-community-shaders/issues/1552
				// { "BSImagespaceShaderCopyDepthBuffer", RE::ImageSpaceManager::GetCurrentIndex(ISCopyDepthBuffer) },
				// { "BSImagespaceShaderCopyDepthBuffer_DR", RE::ImageSpaceManager::GetCurrentIndex(ISCopyDepthBuffer_DR) },
				// { "BSImagespaceShaderCopyDepthBufferTargetSize", RE::ImageSpaceManager::GetCurrentIndex(ISCopyDepthBufferTargetSize) },
				// BSImagespaceShaderGraphicsTextureFilterMode is intentionally omitted because VR index 111 is ISReflectionBlurHCS.
				{ "BSImagespaceShaderISDownsampleHierarchicalDepthBufferCS", RE::ImageSpaceManager::GetCurrentIndex(ISDownsampleHierarchicalDepthBufferCS) },
				{ "BSImagespaceShaderISDiffScaleDownsampleDepthBufferCS", RE::ImageSpaceManager::GetCurrentIndex(ISDiffScaleDownsampleDepthBufferCS) },
				{ "BSImagespaceShaderISFullScreenVR", RE::ImageSpaceManager::GetCurrentIndex(ISFullScreenVR) },
				{ "BSImagespaceShaderISTransformLvl7PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISTransformLvl7PreTest) },
				{ "BSImagespaceShaderISLvl6PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl6PreTest) },
				{ "BSImagespaceShaderISLvl5PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl5PreTest) },
				{ "BSImagespaceShaderISLvl4PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl4PreTest) },
				{ "BSImagespaceShaderISLvl3PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl3PreTest) },
				{ "BSImagespaceShaderISLvl2PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl2PreTest) },
				{ "BSImagespaceShaderISLvl1PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl1PreTest) },
				{ "BSImagespaceShaderISLvl0PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl0PreTest) },
				{ "BSImagespaceShaderISSetupPreTest", RE::ImageSpaceManager::GetCurrentIndex(ISSetupPreTest) },
			};

			auto it = descriptors.find(imagespaceShader.name);
			if (it == descriptors.cend()) {
				return false;
			}
			descriptor = it->second;
			return true;
		}

		static bool ResolveImageSpaceDescriptor(const RE::BSShader& shader, uint32_t& descriptor)
		{
			if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
				const auto& isShader = static_cast<const RE::BSImagespaceShader&>(shader);
				return GetImagespaceShaderDescriptor(isShader, descriptor);
			}
			return true;
		}
	}

	// Removes manifest entries (and blob files) whose source shader no longer
	// exists under Data/Shaders, e.g. removed/renamed by an upstream sync --
	// nothing else prunes these once a version bump can keep the cache.
	static void PruneOrphanedShaderCacheEntries()
	{
		auto& manifest = GetShaderCacheManifest();
		std::unordered_map<std::string, bool> shaderExists;  // shader name -> source .hlsl still on disk
		size_t removedBlobs = 0;
		const size_t removedEntries = manifest.PruneIf([&](const std::string& relativePath) {
			const auto sep = relativePath.find('/');
			if (sep == std::string::npos)
				return false;  // unrecognized key shape -- never delete on a shape we don't understand
			// Manifest.json is plain user-writable JSON; reject any key that could
			// escape Data/ShaderCache via a traversal segment before ever deleting.
			if (relativePath.find("..") != std::string::npos)
				return false;
			const std::string shaderName = relativePath.substr(0, sep);
			auto [it, inserted] = shaderExists.try_emplace(shaderName, false);
			if (inserted)
				it->second = std::filesystem::exists(SShaderCache::GetShaderPath(shaderName));
			if (it->second)
				return false;

			std::error_code ec;
			const std::wstring wRelative(relativePath.begin(), relativePath.end());
			std::filesystem::remove(std::filesystem::path(L"Data/ShaderCache") / wRelative, ec);
			if (!ec)
				++removedBlobs;
			return true;
		});
		if (removedEntries > 0) {
			logger::info("Pruned {} orphaned shader-cache entries ({} blob files) for shaders no longer on disk", removedEntries, removedBlobs);
			manifest.Save();
		}
	}

	RE::BSGraphics::VertexShader* ShaderCache::GetVertexShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		auto state = globals::state;
		if (globals::game::isVR && strcmp(shader.fxpFilename, "OBBOcclusionTesting") == 0)
			// use vanilla shader
			return nullptr;

		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enableVShaders)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Vertex, shader, descriptor);

			auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Vertex, shader, descriptor, true);
			if (blockedKeyIndex != -1 && !blockedKey.empty() && key == blockedKey) {
				if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
					blockedIDs.push_back(descriptor);
					logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
				}
				return nullptr;
			}
		}

		{
			std::lock_guard lockGuard(vertexShadersMutex);
			auto& typeCache = vertexShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Vertex, shader, descriptor });
		} else {
			return MakeAndAddVertexShader(shader, descriptor);
		}

		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::GetPixelShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto state = globals::state;
		if (globals::game::isVR && strcmp(shader.fxpFilename, "OBBOcclusionTesting") == 0)
			// use vanilla shader
			return nullptr;

		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enablePShaders)) {
			return nullptr;
		}

		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Pixel, shader, descriptor);

			auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Pixel, shader, descriptor, true);
			if (blockedKeyIndex != -1 && !blockedKey.empty() && key == blockedKey) {
				if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
					blockedIDs.push_back(descriptor);
					logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
				}
				return nullptr;
			}
		}

		{
			std::lock_guard lockGuard(pixelShadersMutex);
			auto& typeCache = pixelShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Pixel, shader, descriptor });
		} else {
			return MakeAndAddPixelShader(shader, descriptor);
		}

		return nullptr;
	}

	RE::BSGraphics::ComputeShader* ShaderCache::GetComputeShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto state = globals::state;
		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enableCShaders)) {
			return nullptr;
		}

		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Compute, shader, descriptor);

			auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Compute, shader, descriptor, true);
			if (blockedKeyIndex != -1 && !blockedKey.empty() && key == blockedKey) {
				if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
					blockedIDs.push_back(descriptor);
					logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
				}
				return nullptr;
			}
		}

		{
			std::lock_guard lockGuard(computeShadersMutex);
			auto& typeCache = computeShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Compute, shader, descriptor });
		} else {
			return MakeAndAddComputeShader(shader, descriptor);
		}

		return nullptr;
	}

	ShaderCache::~ShaderCache()
	{
		Clear();
		StopFileWatcher();
		// Signal management thread to stop dispatching; pool workers observe the same
		// stop token and will not pick up new tasks after current compilations finish.
		HANDLE managementHandle = managementJthread.native_handle();
		managementJthread.request_stop();
		// Purge unstarted tasks so we only wait for compilations already in flight.
		compilationPool.purge();
		if (!compilationPool.wait_for(std::chrono::milliseconds(1000))) {
			logger::info("Tasks still running despite request to stop; killing management thread {}!", GetThreadId(managementHandle));
			WaitForSingleObject(managementHandle, 1000);
			TerminateThread(managementHandle, 0);
		}
	}

	void ShaderCache::Clear()
	{
		{
			std::lock_guard lockGuardV(vertexShadersMutex);
			for (auto& shaders : vertexShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::lock_guard lockGuardP(pixelShadersMutex);
			for (auto& shaders : pixelShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::lock_guard lockGuardC(computeShadersMutex);
			for (auto& shaders : computeShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::unique_lock lockM{ mapMutex };
			shaderMap.clear();
			// Applying a parked eviction after this clear would hit a since-recompiled
			// key and burn a spurious recompile.
			deferredEvictions.clear();
			deferredEvictionCount.store(0, std::memory_order_relaxed);
		}
		{
			std::unique_lock lockH{ hlslMapMutex };
			hlslToShaderMap.clear();
		}
		compilationSet.Clear();
		globals::deferred->ClearShaderCache();
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded) {
				feature->ClearShaderCache();
			}
		}
	}

	bool ShaderCache::Clear(const std::string& a_path)
	{
		std::string lowerFilePath = Util::FixFilePath(a_path);

		// Step 1: Lock hlslMapMutex to find and copy the relevant entries
		std::set<hlslRecord> entries;
		{
			std::unique_lock lockH{ hlslMapMutex };
			auto it = hlslToShaderMap.find(lowerFilePath);

			if (it == hlslToShaderMap.end()) {
				return false;
			}

			entries = it->second;  // Copy the entries
			hlslToShaderMap.erase(it);
		}

		// Step 2: Process the copied entries without holding hlslMapMutex. A Pending
		// entry gets parked (TryDeferEviction) instead of evicted immediately.
		for (auto& entry : entries) {
			if (!TryDeferEviction(entry))
				EvictShader(entry.key, entry.type, entry.descriptor, entry.shaderClass, entry.diskPath);
		}

		if (!entries.empty()) {
			logger::debug("Marked {} entries for recompile due to change to {}", entries.size(), a_path);
			compilationSet.Clear();
		}

		return true;
	}

	void ShaderCache::Clear(RE::BSShader::Type a_type)
	{
		logger::debug("Clearing cache for {}", magic_enum::enum_name(a_type));
		std::lock_guard lockGuardV(vertexShadersMutex);
		{
			for (auto& [id, shader] : vertexShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			vertexShaders[static_cast<size_t>(a_type)].clear();
		}
		std::lock_guard lockGuardP(pixelShadersMutex);
		{
			for (auto& [id, shader] : pixelShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			pixelShaders[static_cast<size_t>(a_type)].clear();
		}
		std::lock_guard lockGuardC(computeShadersMutex);
		{
			for (auto& [id, shader] : computeShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			computeShaders[static_cast<size_t>(a_type)].clear();
		}
		ClearShaderMap(a_type);
		compilationSet.Clear();
	}

	bool ShaderCache::AddCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, ID3DBlob* a_blob, bool fromDisk)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		auto keyWithDescriptor = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, false);
		auto status = a_blob ? ShaderCompilationTask::Status::Completed : ShaderCompilationTask::Status::Failed;
		logger::debug("Adding {} shader to map: {}", magic_enum ::enum_name(status), keyWithDescriptor);
		{
			std::unique_lock lockM{ mapMutex };
			shaderMap.insert_or_assign(key, ShaderCacheResult{ a_blob, status, system_clock::now(), fromDisk });
		}
		mapCV.notify_all();  // wake threads waiting on a Pending→Completed/Failed transition
		const std::wstring path = SIE::SShaderCache::GetShaderPath(
			shader.shaderType == RE::BSShader::Type::ImageSpace ?
				static_cast<const RE::BSImagespaceShader&>(shader).originalShaderName :
				shader.fxpFilename);
		auto pathString = Util::WStringToString(path);
		// Always create or update an hlsl->shader record so failing compiles are
		// trackable and can be invalidated by the file watcher. This allows
		// Clear(path) to find failed shaders and mark them for recompilation.
		std::string lowerFilePath = Util::FixFilePath(pathString);
		{
			std::unique_lock lockH{ hlslMapMutex };
			auto it = hlslToShaderMap.find(lowerFilePath);
			hlslRecord newRecord{ key, shader.shaderType.get(), descriptor, shaderClass, SIE::SShaderCache::GetDiskPath(shader.fxpFilename, descriptor, shaderClass) };

			if (it != hlslToShaderMap.end()) {
				auto& entries = it->second;

				// Find and remove existing record with the same key
				auto existingRecord = std::find_if(entries.begin(), entries.end(),
					[&](const hlslRecord& r) { return r.key == key; });

				if (existingRecord != entries.end()) {
					entries.erase(existingRecord);  // Remove the old record
				}

				// Insert the new or updated record
				entries.insert(newRecord);
			} else {
				// Create a new entry in hlslToShaderMap for this file path
				hlslToShaderMap.emplace(lowerFilePath, std::set<hlslRecord>{ newRecord });
			}
		}

		// This key's Pending claim (if any) just resolved above -- apply any eviction
		// a concurrent Clear(path) parked against it while it was in flight.
		ApplyDeferredEviction(key);

		return a_blob != nullptr;
	}

	std::pair<ShaderCache::ClaimResult, ID3DBlob*> ShaderCache::ClaimCompilation(const std::string& key)
	{
		std::unique_lock lockM{ mapMutex };

		for (;;) {
			auto it = shaderMap.find(key);
			if (it != shaderMap.end()) {
				auto& entry = it->second;
				if (entry.status == ShaderCompilationTask::Status::Completed) {
					if (entry.blob) {
						logger::debug("Shader already compiled; using cache: {}", key);
						return { ClaimResult::CacheHit, entry.blob };
					}
					break;  // Completed with nullptr blob — re-compile
				}
				if (entry.status == ShaderCompilationTask::Status::Failed) {
					break;  // Previous attempt failed — re-compile
				}
				// Status is Pending — another thread is compiling this shader.
				logger::debug("Shader compilation in progress, waiting: {}", key);
				mapCV.wait(lockM);
				continue;  // re-check after wakeup
			}
			break;  // not in map at all
		}

		// Claim the slot as Pending before releasing the lock
		shaderMap.insert_or_assign(key, ShaderCacheResult{ nullptr, ShaderCompilationTask::Status::Pending, system_clock::now() });
		return { ClaimResult::Claimed, nullptr };
	}

	void ShaderCache::ResolvePendingFailure(const std::string& key)
	{
		bool changed = false;
		{
			std::unique_lock lockM{ mapMutex };
			auto it = shaderMap.find(key);
			if (it != shaderMap.end() && it->second.status == ShaderCompilationTask::Status::Pending) {
				it->second = ShaderCacheResult{ nullptr, ShaderCompilationTask::Status::Failed, system_clock::now() };
				changed = true;
			}
		}
		if (changed) {
			mapCV.notify_all();
			ApplyDeferredEviction(key);
		}
	}

	ID3DBlob* ShaderCache::GetCompletedShader(const std::string& a_key)
	{
		std::string type = SIE::SShaderCache::GetTypeFromShaderString(a_key);
		UpdateShaderModifiedTime(type);
		std::scoped_lock lockM{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			if (ShaderModifiedSince(type, shaderMap.at(a_key).compileTime)) {
				logger::debug("Shader {} compiled {} before changes at {}",
					a_key,
					std::format("{:%H:%M:%S}", shaderMap.at(a_key).compileTime),
					std::format("{:%H:%M:%S}", GetModifiedShaderMapTime(type)));
				return nullptr;
			}
			auto status = shaderMap.at(a_key).status;
			if (status != ShaderCompilationTask::Status::Pending)
				return shaderMap.at(a_key).blob;
		}
		return nullptr;
	}

	ID3DBlob* ShaderCache::GetCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		return GetCompletedShader(key);
	}

	ID3DBlob* ShaderCache::GetCompletedShader(const ShaderCompilationTask& a_task)
	{
		auto key = a_task.GetString();
		return GetCompletedShader(key);
	}

	bool ShaderCache::IsShaderLoadedFromDisk(const std::string& a_key)
	{
		std::scoped_lock lockM{ mapMutex };
		auto it = shaderMap.find(a_key);
		if (it != shaderMap.end())
			return it->second.loadedFromDisk;
		return false;
	}

	ShaderCompilationTask::Status ShaderCache::GetShaderStatus(const std::string& a_key)
	{
		std::scoped_lock lockM{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			return shaderMap.at(a_key).status;
		}
		return ShaderCompilationTask::Status::Pending;
	}

	std::string ShaderCache::GetShaderStatsString(bool a_timeOnly, bool a_elapsedOnly)
	{
		return compilationSet.GetStatsString(a_timeOnly, a_elapsedOnly);
	}

	inline bool ShaderCache::IsShaderSourceAvailable(const RE::BSShader& shader)
	{
		const std::wstring path = SIE::SShaderCache::GetShaderPath(shader.fxpFilename);

		std::string strPath;
		std::transform(path.begin(), path.end(), std::back_inserter(strPath), [](wchar_t c) {
			return (char)c;
		});
		try {
			return std::filesystem::exists(path);
		} catch (const std::filesystem::filesystem_error& e) {
			logger::warn("Error accessing {} : {}", strPath, e.what());
			return false;
		}
	}

	bool ShaderCache::IsCompiling()
	{
		return compilationSet.totalTasks && compilationSet.completedTasks + compilationSet.failedTasks < compilationSet.totalTasks;
	}

	void ShaderCache::StopCompilation()
	{
		if (IsCompiling()) {
			logger::info("Stopping {} remaining shader compilation tasks", compilationSet.totalTasks - compilationSet.completedTasks - compilationSet.failedTasks);
		}
		ssource.request_stop();            // signals any legacy stop_token users
		managementJthread.request_stop();  // stops management thread + in-flight compilations
		compilationSet.Clear();
	}

	bool ShaderCache::IsEnabled() const
	{
		return isEnabled;
	}

	void ShaderCache::SetEnabled(bool value)
	{
		isEnabled = value;
	}

	bool ShaderCache::IsAsync() const
	{
		return isAsync;
	}

	void ShaderCache::SetAsync(bool value)
	{
		isAsync = value;
	}

	bool ShaderCache::IsDump() const
	{
		return isDump;
	}

	void ShaderCache::SetDump(bool value)
	{
		isDump = value;
	}

	bool ShaderCache::IsDiskCache() const
	{
		return isDiskCache;
	}

	void ShaderCache::SetDiskCache(bool value)
	{
		isDiskCache = value;
	}

	bool ShaderCache::IsSkipUnchangedShaders() const
	{
		return isSkipUnchangedShaders;
	}

	void ShaderCache::SetSkipUnchangedShaders(bool value)
	{
		isSkipUnchangedShaders = value;
	}

	static const std::filesystem::path& DiskCachePath()
	{
		static const std::filesystem::path path{ L"Data/ShaderCache" };
		return path;
	}

	static const std::filesystem::path& PreviousDiskCachePath()
	{
		static const std::filesystem::path path{ L"Data/ShaderCache.Previous" };
		return path;
	}

	static const std::filesystem::path& SwapDiskCachePath()
	{
		static const std::filesystem::path path{ L"Data/ShaderCache.Swap" };
		return path;
	}

	/// Info.ini presence is the "this is a real cache" marker for both slots.
	static bool HasDiskCacheInfo(const std::filesystem::path& cachePath)
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(cachePath / L"Info.ini", ec);
		return exists && !ec;
	}

	static bool LoadDiskCacheInfo(const std::filesystem::path& cachePath, CSimpleIniA& ini)
	{
		ini.SetUnicode();
		return ini.LoadFile((cachePath / L"Info.ini").c_str()) >= 0;
	}

	static bool RemoveCachePath(const std::filesystem::path& path, std::string_view label)
	{
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		if (ec) {
			logger::error("Failed to remove {} shader cache path {}: {}", label, Util::WStringToString(path.wstring()), ec.message());
			return false;
		}
		return true;
	}

	static std::vector<Util::CacheInvalidation::FeatureState> GetCurrentFeatureStates()
	{
		std::vector<Util::CacheInvalidation::FeatureState> featureStates;
		for (auto* feature : Feature::GetFeatureList()) {
			// Only a non-empty failedLoadedMessage is a genuine load failure; !loaded
			// alone also covers ordinary and environment-gated disables.
			featureStates.push_back({ feature->GetShortName(), feature->GetDisplayName(), feature->loaded,
				feature->version, std::string(feature->GetShaderDefineName()),
				!feature->loaded && !feature->failedLoadedMessage.empty() });
		}
		return featureStates;
	}

	/// Compare a cache manifest (active or rollback slot) against the current runtime state.
	static std::vector<Util::CacheInvalidation::CacheMismatch> ClassifyCacheInfo(const CSimpleIniA& ini,
		const std::vector<Util::CacheInvalidation::FeatureState>& featureStates)
	{
		std::optional<std::string> cachedPluginVersion;
		if (auto pluginVersion = ini.GetValue("Cache", "PluginVersion"))
			cachedPluginVersion = pluginVersion;

		std::map<std::string, Util::CacheInvalidation::CacheIniEntry> cacheEntries;
		for (const auto& featureState : featureStates) {
			Util::CacheInvalidation::CacheIniEntry entry;
			entry.enabled = ini.GetBoolValue(featureState.shortName.c_str(), "Enabled", false);
			if (auto version = ini.GetValue(featureState.shortName.c_str(), "Version"))
				entry.version = version;
			cacheEntries[featureState.shortName] = entry;
		}
		return Util::CacheInvalidation::ClassifyMismatches(
			Plugin::VERSION.string(), cachedPluginVersion, featureStates, cacheEntries);
	}

	static std::vector<std::string> GetDefinesForMismatches(
		const std::vector<Util::CacheInvalidation::CacheMismatch>& mismatches,
		const std::vector<Util::CacheInvalidation::FeatureState>& featureStates,
		Util::CacheInvalidation::CacheMismatch::Kind kind)
	{
		std::vector<std::string> defines;
		for (const auto& mismatch : mismatches) {
			if (mismatch.kind != kind)
				continue;
			const auto stateIt = std::ranges::find_if(featureStates,
				[&](const Util::CacheInvalidation::FeatureState& featureState) {
					return featureState.shortName == mismatch.shortName;
				});
			if (stateIt != featureStates.end())
				defines.push_back(stateIt->define);
		}
		return defines;
	}

	using Util::CacheInvalidation::OnlyEnabledFlips;

	// Thin runtime wrapper: real logic in Utils/CacheInvalidation.h (unit-tested).
	static bool HasMissingOrFailedFeature(const std::vector<Util::CacheInvalidation::CacheMismatch>& mismatches)
	{
		return Util::CacheInvalidation::HasFailedFeature(mismatches);
	}

	// The rollback slot's on-disk presence is the one filesystem check these
	// can't do without ShaderCache's path helpers, so it's evaluated here and
	// passed in rather than the callee reaching for PreviousDiskCachePath() itself.
	static bool ArePreviousCacheMismatchesRestorable(const std::vector<Util::CacheInvalidation::CacheMismatch>& mismatches)
	{
		return Util::CacheInvalidation::AreCacheMismatchesRestorable(mismatches);
	}

	static bool SetPreviousCacheRestoreCandidate(
		std::vector<Util::CacheInvalidation::CacheMismatch> mismatches,
		bool& previousDiskCacheAvailable,
		std::vector<Util::CacheInvalidation::CacheMismatch>& previousCacheMismatches)
	{
		return Util::CacheInvalidation::TrySetRestoreCandidate(std::move(mismatches),
			HasDiskCacheInfo(PreviousDiskCachePath()),
			previousDiskCacheAvailable, previousCacheMismatches);
	}

	// Thin runtime wrapper: real logic in Utils/CacheInvalidation.h (unit-tested).
	// outDestructive is set when the active cache was left partially deleted --
	// the caller must wipe it outright rather than rotate it into the rollback slot.
	static bool PartialInvalidation(const std::vector<std::string>& defines, bool& outDestructive)
	{
		size_t deleted = 0, kept = 0;
		const bool ok = Util::CacheInvalidation::TryPartialInvalidation(
			DiskCachePath(), L"Data/Shaders", defines, &deleted, &kept, &outDestructive);
		if (ok)
			logger::info("Partial disk cache invalidation: deleted {} shader dirs, kept {}", deleted, kept);
		else if (outDestructive)
			logger::warn("Partial disk cache invalidation failed mid-delete: active cache is now inconsistent, wiping outright");
		else
			logger::warn("Partial disk cache invalidation unavailable, falling back to full wipe");
		return ok;
	}

	void ShaderCache::DeleteActiveDiskCache()
	{
		std::scoped_lock lock{ compilationSet.compilationMutex };
		if (RemoveCachePath(DiskCachePath(), "active"))
			logger::info("Deleted active disk cache");
	}

	void ShaderCache::DeleteDiskCacheFiles()
	{
		std::scoped_lock lock{ compilationSet.compilationMutex };
		const bool removedActive = RemoveCachePath(DiskCachePath(), "active");
		const bool removedPrevious = RemoveCachePath(PreviousDiskCachePath(), "previous");
		const bool removedSwap = RemoveCachePath(SwapDiskCachePath(), "temporary");
		if (removedActive && removedPrevious && removedSwap)
			logger::info("Deleted disk cache and rollback cache");
	}

	// Main-thread-only: also resets the boot-mismatch/rollback UI state, which the
	// menu reads unsynchronized (the file-watcher thread calls DeleteDiskCacheFiles()).
	void ShaderCache::DeleteDiskCache()
	{
		DeleteDiskCacheFiles();

		diskCacheHeld = false;
		featureSetChanged = false;
		featureSetRevertPending = false;
		featureSetCacheBackedUp = false;
		previousDiskCacheAvailable = false;
		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches.clear();
			previousCacheMismatches.clear();
		}
		heldMismatchDefines.clear();
	}

	bool ShaderCache::BackupActiveDiskCache()
	{
		std::scoped_lock lock{ compilationSet.compilationMutex };
		if (!HasDiskCacheInfo(DiskCachePath())) {
			logger::warn("Cannot back up shader cache: active cache info is missing");
			return false;
		}

		std::string error;
		const bool backedUp = Util::CacheInvalidation::BackupCacheDirectory(
			DiskCachePath(), PreviousDiskCachePath(), SwapDiskCachePath(), &error);
		if (!backedUp)
			logger::warn("Failed to back up shader cache: {}", error);
		else
			logger::info("Saved previous shader cache for feature rollback");
		RefreshPreviousDiskCacheInfo();
		return backedUp;
	}

	void ShaderCache::RefreshPreviousDiskCacheInfo()
	{
		previousDiskCacheAvailable = false;
		{
			std::lock_guard lock{ mismatchesMutex };
			previousCacheMismatches.clear();
		}

		if (!HasDiskCacheInfo(PreviousDiskCachePath()))
			return;

		CSimpleIniA ini;
		if (!LoadDiskCacheInfo(PreviousDiskCachePath(), ini)) {
			logger::warn("Previous shader cache exists but its cache info could not be read");
			return;
		}

		auto mismatches = ClassifyCacheInfo(ini, GetCurrentFeatureStates());
		if (mismatches.empty())
			return;

		// Restore is only offered across pure toggle flips: a plugin/feature version
		// change makes the rollback blobs stale, and a missing feature can't re-enable.
		if (!OnlyEnabledFlips(mismatches)) {
			logger::info("Previous shader cache is not offered for restore because versions changed");
			return;
		}
		if (HasMissingOrFailedFeature(mismatches)) {
			logger::info("Previous shader cache is not offered for restore because a cached feature is missing or failed to load");
			return;
		}

		{
			std::lock_guard lock{ mismatchesMutex };
			previousCacheMismatches = std::move(mismatches);
		}
		previousDiskCacheAvailable = true;
	}

	void ShaderCache::ValidateDiskCache()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile((DiskCachePath() / L"Info.ini").c_str());
		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches.clear();
			previousCacheMismatches.clear();
		}
		diskCacheHeld = false;
		featureSetChanged = false;
		featureSetRevertPending = false;
		featureSetCacheBackedUp = false;
		previousDiskCacheAvailable = false;
		heldMismatchDefines.clear();

		RefreshPreviousDiskCacheInfo();

		std::optional<std::string> cachedPluginVersion;
		if (auto pluginVersion = ini.GetValue("Cache", "PluginVersion"))
			cachedPluginVersion = pluginVersion;

		std::vector<Util::CacheInvalidation::FeatureState> featureStates;
		std::map<std::string, Util::CacheInvalidation::CacheIniEntry> cacheEntries;
		// A prior settings save may have recorded, per feature, the enabled state
		// it was about to persist (MarkExpectedFeatureFlip). If a feature's actual
		// loaded state this boot matches what was recorded, its flip was already
		// confirmed by the user and doesn't need to hold for input below.
		std::map<std::string, bool> expectedEnabledMatches;
		for (auto* feature : Feature::GetFeatureList()) {
			const auto shortName = feature->GetShortName();
			featureStates.push_back({ shortName, feature->GetDisplayName(), feature->loaded,
				feature->version, std::string(feature->GetShaderDefineName()),
				!feature->loaded && !feature->failedLoadedMessage.empty() });
			Util::CacheInvalidation::CacheIniEntry entry;
			entry.enabled = ini.GetBoolValue(shortName.c_str(), "Enabled", false);
			if (auto v = ini.GetValue(shortName.c_str(), "Version"))
				entry.version = v;
			cacheEntries[shortName] = entry;
			if (ini.GetValue(shortName.c_str(), "ExpectedEnabled"))
				expectedEnabledMatches[shortName] = ini.GetBoolValue(shortName.c_str(), "ExpectedEnabled", false) == feature->loaded;
		}

		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches = Util::CacheInvalidation::ClassifyMismatches(
				Plugin::VERSION.string(), cachedPluginVersion, featureStates, cacheEntries);
		}

		// Defines of mismatched features, for partial invalidation or the held accept path.
		heldMismatchDefines = GetDefinesForMismatches(cacheMismatches, featureStates, CacheMismatch::Kind::EnabledFlip);
		const auto versionMismatchDefines = GetDefinesForMismatches(cacheMismatches, featureStates, CacheMismatch::Kind::FeatureVersion);

		if (cacheMismatches.empty()) {
			logger::info("Using disk cache");
			return;
		}

		for (const auto& mismatch : cacheMismatches)
			logger::info("Disk cache mismatch: {} - {}", mismatch.feature, mismatch.detail);

		// Version mismatches = expected update path (rebuild silently). A flip set
		// already confirmed via a settings save auto-resolves in place; otherwise
		// rotate the old cache into the rollback slot and build a fresh one.
		if (OnlyEnabledFlips(cacheMismatches)) {
			// A missing/failed feature is a broken install, not a chosen setup: hold the
			// cache untouched so a fixed install revalidates it with no recompile.
			if (HasMissingOrFailedFeature(cacheMismatches)) {
				diskCacheHeld = true;
				logger::info("Disk cache HELD (not deleted): a previously cached feature is missing or failed to load; compiling memory-only this session");
				return;
			}

			// No genuine failure here, so recompile just the flipped features'
			// shaders instead of rotating the whole cache below.
			bool partialInvalidationDestructive = false;
			if (PartialInvalidation(heldMismatchDefines, partialInvalidationDestructive)) {
				const bool allExpected = std::ranges::all_of(cacheMismatches, [&](const CacheMismatch& m) {
					auto it = expectedEnabledMatches.find(m.shortName);
					return it != expectedEnabledMatches.end() && it->second;
				});
				WriteDiskCacheInfo();  // also drops any now-consumed ExpectedEnabled markers
				heldMismatchDefines.clear();
				{
					std::lock_guard lock{ mismatchesMutex };
					cacheMismatches.clear();
				}
				if (allExpected)
					logger::info("Disk cache mismatch matches a settings save from last session; auto-resolving");
				else
					logger::info("Disk cache mismatch resolved: recompiling only the affected features");
				return;
			}

			// A partially-deleted active cache is unsafe to keep as a rollback
			// candidate -- wipe it outright instead of rotating it into Previous.
			if (partialInvalidationDestructive) {
				DeleteActiveDiskCache();
				featureSetChanged = true;
				WriteDiskCacheInfo();
				logger::info("Feature set changed: compiling a new active disk cache; the inconsistent one was discarded, no restore available");
			} else if (BackupActiveDiskCache()) {
				featureSetChanged = true;
				featureSetCacheBackedUp = true;
				bool previousRestoreAvailable;
				{
					std::lock_guard lock{ mismatchesMutex };
					previousRestoreAvailable =
						SetPreviousCacheRestoreCandidate(cacheMismatches, previousDiskCacheAvailable, previousCacheMismatches);
				}
				WriteDiskCacheInfo();
				if (previousRestoreAvailable) {
					logger::info("Feature set changed: compiling a new active disk cache; previous cache is available for restore");
				} else {
					logger::info("Feature set changed: compiling a new active disk cache; previous cache was saved but is not currently available for restore");
				}
			} else {
				// Backup failure falls back to the plain hold so the user keeps the
				// match or rebuild choice; never mutate a cache we could not save.
				diskCacheHeld = true;
				featureSetCacheBackedUp = false;
				logger::warn("Feature set changed but previous cache backup failed; preserving the active cache and compiling memory-only");
			}
			return;
		}

		// Feature version bumps only affect shaders that actually reference the
		// feature's define; keep the rest of the cache. Anything else (plugin
		// version change, missing define, scan failure) falls back to a full wipe
		// of the active slot only; the rollback slot stays for its own validation.
		const bool onlyFeatureVersions = std::ranges::all_of(cacheMismatches,
			[](const CacheMismatch& m) { return m.kind == CacheMismatch::Kind::FeatureVersion; });
		// A plugin-version bump with every feature's enabled/version state still
		// matching claims no shader-affecting change at all; the per-shader
		// content digest (GetShaderContentDigest) is authoritative for individual
		// staleness now, so this no longer needs a full wipe to stay safe.
		const bool onlyPluginVersion = std::ranges::all_of(cacheMismatches,
			[](const CacheMismatch& m) { return m.kind == CacheMismatch::Kind::PluginVersion; });
		// Any failure here (destructive or not) already falls through to the
		// unconditional DeleteActiveDiskCache() below, so the distinction doesn't
		// change behavior at this call site.
		[[maybe_unused]] bool versionInvalidationDestructive = false;
		if (onlyFeatureVersions && PartialInvalidation(versionMismatchDefines, versionInvalidationDestructive)) {
			WriteDiskCacheInfo();  // refresh the manifest so surviving blobs validate next boot
			// The cache survives this mismatch instead of a full wipe, so nothing else
			// prunes entries for shaders removed/renamed since the last version that did wipe.
			PruneOrphanedShaderCacheEntries();
		} else if (onlyPluginVersion) {
			logger::info("Plugin version changed with no feature-state changes; keeping disk cache");
			WriteDiskCacheInfo();
			PruneOrphanedShaderCacheEntries();
		} else {
			DeleteActiveDiskCache();
		}
	}

	void ShaderCache::CommitFeatureSetChange()
	{
		if (!featureSetChanged)
			return;

		const bool committedFeatureSetBackup = featureSetCacheBackedUp;
		std::vector<CacheMismatch> committedPreviousCacheMismatches;
		if (committedFeatureSetBackup) {
			std::lock_guard lock{ mismatchesMutex };
			committedPreviousCacheMismatches = cacheMismatches;
		}

		[[maybe_unused]] bool commitInvalidationDestructive = false;
		if (!featureSetCacheBackedUp && !PartialInvalidation(heldMismatchDefines, commitInvalidationDestructive))
			DeleteActiveDiskCache();

		diskCacheHeld = false;
		heldMismatchDefines.clear();
		WriteDiskCacheInfo();
		featureSetChanged = false;
		featureSetRevertPending = false;
		featureSetCacheBackedUp = false;
		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches.clear();
		}
		RefreshPreviousDiskCacheInfo();
		{
			std::lock_guard lock{ mismatchesMutex };
			if (committedFeatureSetBackup && !previousDiskCacheAvailable &&
				SetPreviousCacheRestoreCandidate(std::move(committedPreviousCacheMismatches), previousDiskCacheAvailable, previousCacheMismatches)) {
				logger::info("Previous shader cache restore retained from feature-change backup");
			}
		}
		logger::info("Feature set change committed: disk cache rebuilt for the current feature set");
	}

	bool ShaderCache::RestorePreviousDiskCache()
	{
		const bool hadPreviousRestoreCandidate = previousDiskCacheAvailable;
		std::vector<CacheMismatch> retainedPreviousCacheMismatches;
		{
			std::lock_guard lock{ mismatchesMutex };
			retainedPreviousCacheMismatches = previousCacheMismatches;
		}

		RefreshPreviousDiskCacheInfo();
		{
			std::lock_guard lock{ mismatchesMutex };
			if (!previousDiskCacheAvailable && hadPreviousRestoreCandidate &&
				SetPreviousCacheRestoreCandidate(std::move(retainedPreviousCacheMismatches), previousDiskCacheAvailable, previousCacheMismatches)) {
				logger::info("Previous shader cache restore retained from feature-change backup");
			}
		}
		if (!previousDiskCacheAvailable) {
			logger::warn("Cannot restore previous shader cache: no compatible previous cache is available");
			return false;
		}
		if (!globals::state) {
			logger::warn("Cannot restore previous shader cache: state is not available");
			return false;
		}

		CSimpleIniA previousInfo;
		if (!LoadDiskCacheInfo(PreviousDiskCachePath(), previousInfo)) {
			logger::warn("Cannot restore previous shader cache: previous cache info could not be read");
			return false;
		}

		{
			// Re-check IsCompiling() under the same lock the writers hold, closing
			// the window where compilation could start between check and restore.
			std::scoped_lock lock{ compilationSet.compilationMutex };
			if (IsCompiling()) {
				logger::warn("Cannot restore previous shader cache while shader compilation is still running");
				return false;
			}
			std::string error;
			std::string warning;
			if (!Util::CacheInvalidation::RestoreCacheDirectory(
					DiskCachePath(), PreviousDiskCachePath(), SwapDiskCachePath(), &error, &warning)) {
				logger::warn("Failed to restore previous shader cache: {}", error);
				return false;
			}
			if (!warning.empty())
				logger::warn("Previous shader cache restored, but {}", warning);
		}

		// Match boot toggles to the restored cache so the next boot revalidates clean.
		for (auto* feature : Feature::GetFeatureList()) {
			const auto shortName = feature->GetShortName();
			const bool enabledInPreviousCache = previousInfo.GetBoolValue(shortName.c_str(), "Enabled", false);
			globals::state->SetFeatureDisabled(shortName, !enabledInPreviousCache);
		}
		globals::state->Save();

		featureSetChanged = false;
		featureSetRevertPending = true;
		featureSetCacheBackedUp = false;
		diskCacheHeld = false;
		heldMismatchDefines.clear();
		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches.clear();
		}
		RefreshPreviousDiskCacheInfo();
		logger::info("Previous shader cache restored: restart to load it");
		return true;
	}

	void ShaderCache::AcceptCacheRebuild()
	{
		if (!diskCacheHeld)
			return;

		[[maybe_unused]] bool acceptInvalidationDestructive = false;
		if (!PartialInvalidation(heldMismatchDefines, acceptInvalidationDestructive))
			DeleteActiveDiskCache();

		heldMismatchDefines.clear();
		WriteDiskCacheInfo();
		diskCacheHeld = false;
		featureSetChanged = false;
		featureSetRevertPending = false;
		featureSetCacheBackedUp = false;
		{
			std::lock_guard lock{ mismatchesMutex };
			cacheMismatches.clear();
		}
		RefreshPreviousDiskCacheInfo();
		Clear();
		logger::info("Cache rebuild accepted: rebuilding disk cache for the current feature set");
	}

	void ShaderCache::WriteDiskCacheInfo()
	{
		// Shares compilationMutex with DeleteDiskCacheFiles() so the file-watcher
		// thread can't delete this directory mid-write.
		std::scoped_lock lock{ compilationSet.compilationMutex };
		std::error_code ec;
		std::filesystem::create_directories(DiskCachePath(), ec);
		if (ec) {
			logger::error("Failed to create shader cache folder: {}", ec.message());
			return;
		}

		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetValue("Cache", "PluginVersion", Plugin::VERSION.string().c_str());
		globals::state->WriteDiskCacheInfo(ini);
		if (ini.SaveFile((DiskCachePath() / L"Info.ini").c_str()) < 0) {
			logger::error("Failed to save disk cache info; the cache will be revalidated from scratch next boot");
			return;
		}
		logger::info("Saved disk cache info (plugin version: {})", Plugin::VERSION.string());
	}

	void ShaderCache::MarkExpectedFeatureFlip()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(L"Data\\ShaderCache\\Info.ini");
		bool changed = false;
		for (auto* feature : Feature::GetFeatureList()) {
			const auto shortName = feature->GetShortName();
			const bool cachedEnabled = ini.GetBoolValue(shortName.c_str(), "Enabled", false);
			const bool nextEnabled = !globals::state->IsFeatureDisabled(shortName);
			if (nextEnabled != cachedEnabled) {
				ini.SetBoolValue(shortName.c_str(), "ExpectedEnabled", nextEnabled);
				changed = true;
			} else if (ini.GetValue(shortName.c_str(), "ExpectedEnabled")) {
				ini.Delete(shortName.c_str(), "ExpectedEnabled");
				changed = true;
			}
		}
		if (changed)
			ini.SaveFile(L"Data\\ShaderCache\\Info.ini");
	}

	/// True when an env var is set to a truthy value ("1" or "true", case-insensitive).
	static bool IsEnvVarTruthy(const char* a_name)
	{
		char buffer[16] = {};
		const DWORD len = GetEnvironmentVariableA(a_name, buffer, sizeof(buffer));
		if (len == 0 || len >= sizeof(buffer))
			return false;
		std::string value(buffer, len);
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value == "1" || value == "true";
	}

	ShaderCache::ShaderCache()
	{
		// Env var is available at process start, so it works even when the saved
		// menu setting loads after the first compile would begin (headless/CI). It
		// ORs with BackgroundShaderCompilationOnBoot (applied later in Menu::Load).
		if (IsEnvVarTruthy("OPENSHADERS_BACKGROUND_COMPILE")) {
			backgroundCompilation = true;
			logger::info("OPENSHADERS_BACKGROUND_COMPILE set; starting shaders in background compilation mode");
		}

		dependencyTracker = std::make_unique<ShaderFileDependencyTracker>();
		logger::debug("ShaderCache initialized: {} startup threads, {} background threads, {} pool threads",
			(int)compilationThreadCount, (int)backgroundCompilationThreadCount, (int)compilationPool.get_thread_count());
		// Management thread runs on a dedicated jthread, not in the compilation pool,
		// so it doesn't consume a pool slot that could be used for shader compilation.
		managementJthread = std::jthread([this](std::stop_token stoken) {
			ManageCompilationSet(stoken);
		});
	}

	bool ShaderCache::UseFileWatcher() const
	{
		return useFileWatcher;
	}

	void ShaderCache::SetFileWatcher(bool value)
	{
		auto oldValue = useFileWatcher;
		useFileWatcher = value;
		if (useFileWatcher && !oldValue)
			StartFileWatcher();
		else if (!useFileWatcher && oldValue)
			StopFileWatcher();
	}

	void ShaderCache::StartFileWatcher()
	{
		logger::info("Starting FileWatcher");
		if (!fileWatcher) {
			fileWatcher = new efsw::FileWatcher();
			listener = new UpdateListener(dependencyTracker.get());
			// Add a folder to watch, and get the efsw::WatchID
			// Reporting the files and directories changes to the instance of the listener
			watchID = fileWatcher->addWatch("Data\\Shaders", listener, true);
			// Start watching asynchronously the directories
			fileWatcher->watch();
			std::string pathStr = "";
			for (auto path : fileWatcher->directories()) {
				pathStr += std::format("{}; ", path);
			}
			logger::debug("ShaderCache watching for changes in {}", pathStr);
			// Capture listener by value so the thread does not race with StopFileWatcher()
			// nulling this->listener before the thread has had a chance to start.
			auto* capturedListener = listener;
			capturedListener->fileWatcherThread = std::jthread([capturedListener]() {
				capturedListener->processQueue();
			});
		} else {
			logger::debug("ShaderCache already enabled");
		}
	}

	void ShaderCache::StopFileWatcher()
	{
		logger::info("Stopping FileWatcher");
		// Set flag first so processQueue()'s loop condition becomes false before we join.
		useFileWatcher = false;
		if (fileWatcher) {
			fileWatcher->removeWatch(watchID);
			fileWatcher = nullptr;
		}
		if (listener) {
			// ~jthread() calls request_stop() + join(); processQueue() exits when
			// UseFileWatcher() returns false (set above).
			delete listener;
			listener = nullptr;
		}
	}

	bool ShaderCache::UpdateShaderModifiedTime(const std::string& a_type, boolean a_forceUpdate)
	{
		if (!UseFileWatcher())
			return false;
		// Validate the shader type
		if (a_type.empty() || !magic_enum::enum_cast<RE::BSShader::Type>(a_type, magic_enum::case_insensitive).has_value()) {
			return false;  // Invalid type
		}

		std::lock_guard lockGuard(modifiedMapMutex);

		// Check for force update
		if (a_forceUpdate) {
			// Set an artificial timestamp far in the future (100 years)
			auto futureTime = std::chrono::system_clock::now() + std::chrono::hours(24 * 365 * 100);
			modifiedShaderMap.insert_or_assign(a_type, futureTime);
			return true;
		}

		// Otherwise, update with the actual file time
		std::filesystem::path filePath{ SIE::SShaderCache::GetShaderPath(a_type) };
		if (std::filesystem::exists(filePath)) {
			auto fileTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath));

			// Update only if timestamp has changed
			if (!modifiedShaderMap.contains(a_type) || modifiedShaderMap.at(a_type) != fileTime) {
				modifiedShaderMap.insert_or_assign(a_type, fileTime);
				return true;
			}
		}
		return false;
	}

	bool ShaderCache::ShaderModifiedSince(const std::string& a_type, std::chrono::system_clock::time_point a_current)
	{
		if (!UseFileWatcher())
			return false;
		// Validate the shader type
		if (a_type.empty() || !magic_enum::enum_cast<RE::BSShader::Type>(a_type, magic_enum::case_insensitive).has_value()) {
			return false;  // Invalid type
		}

		std::lock_guard lockGuard(modifiedMapMutex);

		// Check if the shader type exists in the map and if its modification time is newer than a_current
		return !modifiedShaderMap.empty() && modifiedShaderMap.contains(a_type) && modifiedShaderMap.at(a_type) > a_current;
	}

	RE::BSGraphics::VertexShader* ShaderCache::MakeAndAddVertexShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Vertex, shader, descriptor, IsDiskCacheActive(), dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreateVertexShader(*shaderBlob, shader,
				descriptor);

			std::lock_guard lockGuard(vertexShadersMutex);

			const auto result = device->CreateVertexShader(shaderBlob->GetBufferPointer(),
				newShader->byteCodeSize, nullptr, reinterpret_cast<ID3D11VertexShader**>(&newShader->shader));
			if (FAILED(result)) {
				logger::error("Failed to create vertex shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()), descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				return vertexShaders[static_cast<size_t>(shader.shaderType.get())]
				    .insert_or_assign(descriptor, std::move(newShader))
				    .first->second.get();
			}
		}
		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::MakeAndAddPixelShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Pixel, shader, descriptor, IsDiskCacheActive(), dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreatePixelShader(*shaderBlob, shader,
				descriptor);

			std::lock_guard lockGuard(pixelShadersMutex);
			const auto result = device->CreatePixelShader(shaderBlob->GetBufferPointer(),
				shaderBlob->GetBufferSize(), nullptr, reinterpret_cast<ID3D11PixelShader**>(&newShader->shader));
			if (FAILED(result)) {
				logger::error("Failed to create pixel shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				return pixelShaders[static_cast<size_t>(shader.shaderType.get())]
				    .insert_or_assign(descriptor, std::move(newShader))
				    .first->second.get();
			}
		}
		return nullptr;
	}

	RE::BSGraphics::ComputeShader* ShaderCache::MakeAndAddComputeShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Compute, shader, descriptor, IsDiskCacheActive(), dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreateComputeShader(*shaderBlob, shader,
				descriptor);

			std::lock_guard lockGuard(computeShadersMutex);
			const auto result = device->CreateComputeShader(shaderBlob->GetBufferPointer(),
				shaderBlob->GetBufferSize(), nullptr, reinterpret_cast<ID3D11ComputeShader**>(&newShader->shader));
			if (FAILED(result)) {
				logger::error("Failed to create pixel shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				return computeShaders[static_cast<size_t>(shader.shaderType.get())]
				    .insert_or_assign(descriptor, std::move(newShader))
				    .first->second.get();
			}
		}
		return nullptr;
	}

	std::string ShaderCache::GetDefinesString(const RE::BSShader& shader, uint32_t descriptor)
	{
		std::array<D3D_SHADER_MACRO, 64> defines{};
		SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });

		return SIE::SShaderCache::MergeDefinesString(defines, true);
	}

	uint64_t ShaderCache::GetCachedHitTasks()
	{
		return compilationSet.cacheHitTasks;
	}
	uint64_t ShaderCache::GetCompletedTasks()
	{
		return compilationSet.completedTasks;
	}
	uint64_t ShaderCache::GetFailedTasks()
	{
		return compilationSet.failedTasks;
	}

	uint64_t ShaderCache::GetCurrentFailedCount()
	{
		std::scoped_lock lock(mapMutex);
		uint64_t count = 0;
		for (const auto& [key, result] : shaderMap) {
			if (result.status == ShaderCompilationTask::Status::Failed) {
				++count;
			}
		}
		return count;
	}

	uint64_t ShaderCache::GetTotalTasks()
	{
		return compilationSet.totalTasks;
	}
	uint64_t ShaderCache::GetDiskHitTasks()
	{
		return compilationSet.diskHitTasks;
	}
	uint64_t ShaderCache::GetDigestComputeCount()
	{
		return compilationSet.digestComputeCount;
	}
	int64_t ShaderCache::GetDigestComputeTimeUs()
	{
		return compilationSet.digestComputeTimeUs;
	}
	uint64_t ShaderCache::GetDigestDecidedTasks()
	{
		return compilationSet.digestDecidedTasks;
	}
	void ShaderCache::IncCacheHitTasks()
	{
		compilationSet.cacheHitTasks++;
	}
	void ShaderCache::RecordDigestComputeTime(int64_t a_elapsedUs)
	{
		compilationSet.digestComputeCount++;
		compilationSet.digestComputeTimeUs += a_elapsedUs;
	}
	void ShaderCache::IncDigestDecidedTasks()
	{
		compilationSet.digestDecidedTasks++;
	}

	bool ShaderCache::IsHideErrors()
	{
		return hideError;
	}

	int ShaderCache::GetHeavyTasksInFlight()
	{
		return static_cast<int>(compilationSet.heavyTasksInFlight.load(std::memory_order_relaxed));
	}

	uint64_t ShaderCache::GetSlowTasks()
	{
		return compilationSet.slowTasks.load(std::memory_order_relaxed);
	}

	uint64_t ShaderCache::GetVerySlowTasks()
	{
		return compilationSet.verySlowTasks.load(std::memory_order_relaxed);
	}

	std::vector<CompilationSet::SlowTaskRecord> CompilationSet::GetTopSlowTasks(size_t n) const
	{
		std::lock_guard lock(slowTasksMutex);
		// Partial sort to get the N highest without fully sorting the whole vector.
		std::vector<SlowTaskRecord> result = slowTaskRecords;
		if (result.size() > n) {
			std::partial_sort(result.begin(), result.begin() + n, result.end(),
				[](const SlowTaskRecord& a, const SlowTaskRecord& b) { return a.elapsedMs > b.elapsedMs; });
			result.resize(n);
		} else {
			std::sort(result.begin(), result.end(),
				[](const SlowTaskRecord& a, const SlowTaskRecord& b) { return a.elapsedMs > b.elapsedMs; });
		}
		return result;
	}

	std::vector<CompilationSet::SlowTaskRecord> ShaderCache::GetTopSlowTasks(size_t n)
	{
		return compilationSet.GetTopSlowTasks(n);
	}

	std::optional<CompilationSet::ParallelismStats> CompilationSet::GetParallelismStats() const
	{
		std::vector<SlowTaskRecord> records;
		{
			std::lock_guard lock(slowTasksMutex);
			if (slowTaskRecords.empty()) {
				return std::nullopt;
			}
			records = slowTaskRecords;
		}

		ParallelismStats stats;
		stats.sampleCount = records.size();
		for (const auto& rec : records) {
			stats.workMs += rec.elapsedMs;
			stats.spanMs = std::max(stats.spanMs, rec.elapsedMs);
			stats.avgQueueWaitMs += rec.queueWaitMs;
			stats.maxQueueWaitMs = std::max(stats.maxQueueWaitMs, rec.queueWaitMs);
		}
		stats.avgQueueWaitMs /= static_cast<double>(stats.sampleCount);

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		int64_t endTime = completionTime.load(std::memory_order_relaxed);
		if (endTime == 0) {
			endTime = now.QuadPart;
		}
		stats.makespanMs = static_cast<double>(endTime - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;

		if (stats.spanMs > 0.0) {
			stats.avgParallelism = stats.workMs / stats.spanMs;
		}
		if (stats.makespanMs > 0.0) {
			stats.infiniteCoreEfficiency = stats.spanMs / stats.makespanMs;
			stats.infiniteCoreGapPercent = std::max(0.0, 100.0 * (1.0 - stats.infiniteCoreEfficiency));
		}

		return stats;
	}

	std::optional<CompilationSet::ParallelismStats> ShaderCache::GetParallelismStats()
	{
		return compilationSet.GetParallelismStats();
	}

	void ShaderCache::ClearShaderMap(RE::BSShader::Type a_type)
	{
		std::string_view shaderTypeStr = magic_enum::enum_name(a_type);

		std::unique_lock lockM{ SIE::ShaderCache::mapMutex };
		logger::debug("Clearing shaderMap of {}", shaderTypeStr);
		for (auto it = shaderMap.begin(); it != shaderMap.end();) {
			auto typeInKey = SIE::SShaderCache::GetTypeFromShaderString(it->first);
			if (typeInKey == shaderTypeStr) {
				it = shaderMap.erase(it);
			} else {
				++it;
			}
		}
	}

	void ShaderCache::InsertModifiedShaderMap(const std::string& a_shader, std::chrono::time_point<std::chrono::system_clock> a_time)
	{
		std::lock_guard lockGuard(modifiedMapMutex);
		modifiedShaderMap.insert_or_assign(a_shader, a_time);
	}

	std::chrono::time_point<std::chrono::system_clock> ShaderCache::GetModifiedShaderMapTime(const std::string& a_shader)
	{
		std::lock_guard lockGuard(modifiedMapMutex);
		return modifiedShaderMap.at(a_shader);
	}

	void ShaderCache::ToggleErrorMessages()
	{
		hideError = !hideError;
	}

	void ShaderCache::IterateShaderBlock(bool a_forward)
	{
		// Try to use active shaders list if available in developer mode
		if (globals::state->IsDeveloperMode()) {
			std::lock_guard lockActive(activeShadersMutex);
			if (!activeShaders.empty()) {
				// Build sorted list of active shader keys
				std::vector<std::string> keys;
				keys.reserve(activeShaders.size());
				for (const auto& [key, _] : activeShaders) {
					keys.push_back(key);
				}
				std::sort(keys.begin(), keys.end());

				// Find current position or start
				int currentIdx = -1;
				if (!blockedKey.empty()) {
					auto it = std::find(keys.begin(), keys.end(), blockedKey);
					if (it != keys.end()) {
						currentIdx = static_cast<int>(std::distance(keys.begin(), it));
					}
				}

				// Calculate next index
				int targetIdx = 0;
				if (currentIdx >= 0) {
					targetIdx = a_forward ? (currentIdx + 1) % static_cast<int>(keys.size()) : (currentIdx - 1 + static_cast<int>(keys.size())) % static_cast<int>(keys.size());
				} else {
					targetIdx = a_forward ? 0 : static_cast<int>(keys.size()) - 1;
				}

				blockedKey = keys[targetIdx];
				blockedKeyIndex = -2;  // Set to -2 for dev selections to distinguish from shaderMap indices
				blockedIDs.clear();
				logger::debug("Blocking active shader ({}/{}) {}", targetIdx + 1, keys.size(), blockedKey);
				return;
			}
		}

		// Fallback to original behavior with full shader map
		std::scoped_lock lockM{ mapMutex };
		auto targetIndex = a_forward ? 0 : shaderMap.size() - 1;           // default start or last element
		if (blockedKeyIndex >= 0 && shaderMap.size() > blockedKeyIndex) {  // grab next element
			targetIndex = (blockedKeyIndex + (a_forward ? 1 : -1)) % shaderMap.size();
		}
		auto index = 0;
		for (auto& [key, value] : shaderMap) {
			if (index++ == targetIndex) {
				blockedKey = key;
				blockedKeyIndex = -1;
				blockedIDs.clear();
				logger::debug("Blocking shader ({}/{}) {}", blockedKeyIndex + 1, shaderMap.size(), blockedKey);
				return;
			}
		}
	}

	void ShaderCache::DisableShaderBlocking()
	{
		blockedKey = "";
		blockedKeyIndex = -1;
		blockedIDs.clear();
		logger::debug("Stopped blocking shaders");
	}

	void ShaderCache::TrackActiveShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor)
	{
		if (!IsTrackingActiveShaders())
			return;

		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		std::lock_guard lock(activeShadersMutex);

		auto& info = activeShaders[key];
		if (info.key.empty()) {
			// First time seeing this shader
			info.key = key;
			info.shaderType = shader.shaderType.get();
			info.shaderClass = shaderClass;
			info.descriptor = descriptor;

			// Construct disk path. Unlike the HLSL source path (which uses originalShaderName for
			// ImageSpace shaders), the compiled blob is always keyed on fxpFilename - see GetDiskPath's
			// other call sites (AddCompletedShader, hlslRecord construction).
			info.diskPath = SIE::SShaderCache::GetDiskPath(shader.fxpFilename, descriptor, shaderClass);
		}

		info.isActive = true;
		info.drawCalls++;
		info.lastUsed = std::chrono::steady_clock::now();

		// Render thread only: BSShader::LoadShaders drives Get*Shader in bulk off-thread
		// (Hooks.cpp BSShader_LoadShaders, TruePBR::GenerateShaderPermutations). Ingesting that
		// would balloon a scene-scoped capture into a near-full clear.
		if (activeShaderCaptureFramesRemaining.load(std::memory_order_relaxed) > 0 &&
			std::this_thread::get_id() == activeShaderCaptureThread.load(std::memory_order_relaxed)) {
			capturedShaders.try_emplace(key, info);  // first sighting wins; info is descriptor-complete
		}
	}

	void ShaderCache::ResetFrameShaderTracking()
	{
		if (!globals::state->IsDeveloperMode())
			return;

		std::lock_guard lock(activeShadersMutex);

		// Mark all shaders as inactive for this frame
		// Keep shaders that were used recently (within last 60 frames / ~1 second at 60fps)
		auto now = std::chrono::steady_clock::now();
		auto timeout = std::chrono::seconds(1);

		for (auto it = activeShaders.begin(); it != activeShaders.end();) {
			auto& info = it->second;
			info.isActive = false;
			info.drawCalls = 0;

			// Remove shaders that haven't been used recently
			if (now - info.lastUsed > timeout) {
				it = activeShaders.erase(it);
			} else {
				++it;
			}
		}
	}

	std::vector<ShaderCache::ActiveShaderInfo> ShaderCache::GetActiveShaders() const
	{
		std::lock_guard lock(activeShadersMutex);
		std::vector<ActiveShaderInfo> result;
		result.reserve(activeShaders.size());

		for (const auto& [key, info] : activeShaders) {
			result.push_back(info);
		}

		return result;
	}

	void ShaderCache::ManageCompilationSet(std::stop_token stoken)
	{
		managementThread = GetCurrentThread();
		SetThreadPriority(managementThread, THREAD_PRIORITY_BELOW_NORMAL);
		while (!stoken.stop_requested()) {
			const auto& task = compilationSet.WaitTake(stoken);
			if (!task.has_value())
				break;  // exit because thread told to end
			compilationPool.detach_task([this, stoken, t = task.value()] { ProcessCompilationSet(stoken, t); });
		}
	}

	void ShaderCache::ProcessCompilationSet(std::stop_token stoken, SIE::ShaderCompilationTask task)
	{
		if (stoken.stop_requested()) {
			return;
		}

		const auto taskKey = task.GetString();

		// Run all shader compilation work at below-normal priority.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		LARGE_INTEGER start, end, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		const double queueWaitMs = task.GetEnqueuedQpc() > 0 ?
		                               static_cast<double>(start.QuadPart - task.GetEnqueuedQpc()) * 1000.0 / freq.QuadPart :
		                               0.0;

		try {
			task.Perform();
		} catch (const std::exception& e) {
			logger::error("Unhandled exception compiling shader task {}: {}", taskKey, e.what());
			ResolvePendingFailure(taskKey);
		} catch (...) {
			logger::error("Unhandled non-standard exception compiling shader task {}", taskKey);
			ResolvePendingFailure(taskKey);
		}

		QueryPerformanceCounter(&end);
		const double elapsedMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
		// Use saturating math: without a lock, Clear() can zero totalTasks while completedTasks
		// still reads high briefly, which would otherwise underflow uint64_t (logs as ~2^64-1).
		const uint64_t total = compilationSet.totalTasks.load(std::memory_order_relaxed);
		const uint64_t done = compilationSet.completedTasks.load(std::memory_order_relaxed) +
		                      compilationSet.failedTasks.load(std::memory_order_relaxed);
		// This task has already finished running, but Complete(task) has not yet updated the counters.
		// Include the current task in the local progress snapshot so the logged remaining count is accurate.
		const uint64_t doneIncludingCurrent = (done < total) ? (done + 1) : total;
		const uint64_t remaining = (total > doneIncludingCurrent) ? (total - doneIncludingCurrent) : 0;

		// Proxy for permutation complexity: descriptor low 32 bits from GetId(); popcount = active defines.
		// Shader file size provides a secondary signal for source complexity.
		const auto descriptorComplexity = std::popcount(static_cast<uint32_t>(task.GetId()));
		uintmax_t sourceBytes = 0;
		{
			// GetString() format: "fxpFilename:ShaderClass:defines" — filename is before the first colon.
			const auto taskStr = task.GetString();
			const auto sep = taskStr.find(':');
			if (sep != std::string::npos) {
				const auto shaderName = taskStr.substr(0, sep);
				if (auto path = SIE::SShaderCache::GetShaderPath(shaderName); !path.empty()) {
					std::error_code ec;
					sourceBytes = std::filesystem::file_size(path, ec);
				}
			}
		}

		// Debug: full per-task record for post-mortem straggler analysis.
		logger::debug("[ShaderTiming] {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | tid={} | {}",
			elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes,
			task.GetPriority(), GetCurrentThreadId(), taskKey);

		constexpr double kSlowMs = 2000.0;
		constexpr double kVerySlowMs = 8000.0;

		// Record every task for post-mortem analysis and developer UI (top-N display).
		{
			std::lock_guard lock(compilationSet.slowTasksMutex);
			compilationSet.slowTaskRecords.push_back({ taskKey, elapsedMs, queueWaitMs, task.GetPriority(),
				static_cast<int>(descriptorComplexity), sourceBytes });
		}

		if (elapsedMs >= kVerySlowMs) {
			compilationSet.verySlowTasks++;
			compilationSet.slowTasks++;
			logger::info("[ShaderTiming] Very slow {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | {}",
				elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes, task.GetPriority(), taskKey);
		} else if (elapsedMs >= kSlowMs) {
			compilationSet.slowTasks++;
			logger::debug("[ShaderTiming] Slow {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | {}",
				elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes, task.GetPriority(), taskKey);
		}

		if (stoken.stop_requested()) {
			return;
		}

		compilationSet.Complete(task);
	}

	ShaderCompilationTask::ShaderCompilationTask(ShaderClass aShaderClass,
		const RE::BSShader& aShader,
		uint32_t aDescriptor) :
		shaderClass(aShaderClass),
		shader(aShader), descriptor(aDescriptor),
		cachedPriority(ComputePriority(aShaderClass, aShader, aDescriptor))
	{}

	void ShaderCompilationTask::Perform() const
	{
		ZoneScoped;
		ZoneText(GetString().c_str(), GetString().size());

		if (shaderClass == ShaderClass::Vertex) {
			ShaderCache::Instance().MakeAndAddVertexShader(shader, descriptor);
		} else if (shaderClass == ShaderClass::Pixel) {
			ShaderCache::Instance().MakeAndAddPixelShader(shader, descriptor);
		} else if (shaderClass == ShaderClass::Compute) {
			ShaderCache::Instance().MakeAndAddComputeShader(shader, descriptor);
		}
	}

	size_t ShaderCompilationTask::GetId() const
	{
		return MakeId(shaderClass, shader.shaderType.get(), descriptor);
	}

	size_t ShaderCompilationTask::MakeId(ShaderClass shaderClass, RE::BSShader::Type shaderType, uint32_t descriptor)
	{
		return descriptor + (static_cast<size_t>(shaderType) << 32) +
		       (static_cast<size_t>(shaderClass) << 60);
	}

	std::string ShaderCompilationTask::GetString() const
	{
		return SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
	}

	bool ShaderCompilationTask::operator==(const ShaderCompilationTask& other) const
	{
		return GetId() == other.GetId();
	}

	int ShaderCompilationTask::ComputePriority(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor)
	{
		int priority = 0;
		const auto type = shader.shaderType.get();

		// Base priority by shader type — Lighting is consistently the slowest
		// (123KB source, 12s+ compile), followed by Effect (~31KB, up to 12s).
		switch (type) {
		case RE::BSShader::Type::Lighting:
			priority += 1000;
			break;
		case RE::BSShader::Type::Effect:
			priority += 500;
			break;
		case RE::BSShader::Type::Water:
			priority += 300;
			break;
		default:
			break;
		}

		// Pixel shaders compile significantly slower than vertex shaders
		if (shaderClass == ShaderClass::Pixel)
			priority += 200;

		// More active descriptor bits → more #defines → more code paths for the compiler
		priority += std::popcount(descriptor) * 30;

		// Known heavy Lighting techniques and flags from straggler analysis
		if (type == RE::BSShader::Type::Lighting) {
			const auto technique = static_cast<ShaderCache::LightingShaderTechniques>(0x3F & (descriptor >> 24));

			// LANDSCAPE techniques (MTLand, MTLandLODBlend) are among the heaviest
			// due to multi-texture blending codegen — regularly 60-130s compile times
			if (technique == ShaderCache::LightingShaderTechniques::MTLand ||
				technique == ShaderCache::LightingShaderTechniques::MTLandLODBlend)
				priority += 500;
			if (technique == ShaderCache::LightingShaderTechniques::Parallax ||
				technique == ShaderCache::LightingShaderTechniques::ParallaxOcc)
				priority += 300;
			if (technique == ShaderCache::LightingShaderTechniques::Eye)
				priority += 200;
			if (technique == ShaderCache::LightingShaderTechniques::MultilayerParallax)
				priority += 200;

			// TRUE_PBR and ANISO_LIGHTING are the dominant cost drivers,
			// especially in combination with LANDSCAPE (115-130s observed)
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr))
				priority += 500;
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::AnisoLighting))
				priority += 300;
			// Deferred adds extra codegen overhead
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::Deferred))
				priority += 200;

			// LANDSCAPE + TRUE_PBR combination triggers extreme register pressure
			// (6x unrolled texture layers * PBR params = 30+ textures, 180s+ compile)
			if ((technique == ShaderCache::LightingShaderTechniques::MTLand ||
					technique == ShaderCache::LightingShaderTechniques::MTLandLODBlend) &&
				(descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr)))
				priority += 500;
		}

		return priority;
	}

	std::optional<ShaderCompilationTask> CompilationSet::WaitTake(std::stop_token stoken)
	{
		std::unique_lock lock(compilationMutex);
		auto shaderCache = globals::shaderCache;
		if (!conditionVariable.wait(
				lock, stoken,
				[this, &shaderCache]() { return !availableTasks.empty() &&
			                                    // Dispatch when pool has room. Use < (not <=) so that after
			                                    // push_task() the total never exceeds the limit.
			                                    (int)shaderCache->compilationPool.get_tasks_total() <
			                                        (!shaderCache->backgroundCompilation ? shaderCache->compilationThreadCount : shaderCache->backgroundCompilationThreadCount); })) {
			/*Woke up because of a stop request. */
			return std::nullopt;
		}
		// Session clock is now managed by CompilationSet::Add(); this branch is kept
		// as a safety net but will not trigger because totalTasks is incremented
		// before the conditionVariable notification.
		if (!shaderCache->IsCompiling()) {
			QueryPerformanceCounter(&lastReset);
			lastCalculation = lastReset;
		}

		// Startup policy: keep dispatching the hardest queued work first.
		// This preserves the existing priority score while preventing light tasks
		// from bypassing queued heavy shaders and stretching the tail.
		auto bestIt = availableTasks.end();
		if (!availableTasks.empty()) {
			bestIt = std::prev(availableTasks.end());
		}

		if (bestIt == availableTasks.end()) {
			return std::nullopt;
		}

		ShaderCompilationTask task = *bestIt;
		availableTasks.erase(bestIt);

		if (task.GetPriority() >= kHeavyPriorityThreshold) {
			heavyTasksInFlight.fetch_add(1, std::memory_order_relaxed);
		}

		tasksInProgress.insert(task);
		return task;
	}

	void CompilationSet::Add(const ShaderCompilationTask& task)
	{
		std::unique_lock lock(compilationMutex);
		auto inProgressIt = tasksInProgress.find(task);
		auto processedIt = processedTasks.find(task);
		if (inProgressIt == tasksInProgress.end() && processedIt == processedTasks.end() && !globals::shaderCache->GetCompletedShader(task)) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			auto queuedTask = task;
			queuedTask.SetEnqueuedQpc(now.QuadPart);
			auto [_, wasAdded] = availableTasks.insert(queuedTask);
			if (wasAdded) {
				// Increment counters inside the lock so that WaitTake, which reads
				// IsCompiling() after waking up, sees the updated totalTasks and
				// does NOT incorrectly treat the new work as a "fresh start" and
				// reset the session clock via its !IsCompiling() branch.
				// Only the very first task starts the clock here -- a later "new
				// session" needs to know whether this task is a disk-cache hit or
				// a real compile, which isn't known yet at enqueue time; see
				// Complete() and Forget(), which do know.
				if (totalTasks.load(std::memory_order_relaxed) == 0) {
					QueryPerformanceCounter(&lastReset);
					lastCalculation = lastReset;
				}

				totalTasks++;
				totalPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;
			}
			lock.unlock();
			if (wasAdded) {
				conditionVariable.notify_one();
			}
		}
	}

	/**
	 * @brief Marks a shader compilation task as complete and updates compilation state.
	 *
	 * Updates task completion counters, tracks compilation timing and cache behavior,
	 * detects batch-level completion, and emits notifications and events when the
	 * entire compilation session finishes.
	 *
	 * @param task The compilation task that has completed.
	 */
	void CompilationSet::Complete(const ShaderCompilationTask& task)
	{
		auto& cache = ShaderCache::Instance();
		auto key = task.GetString();
		auto shaderBlob = cache.GetCompletedShader(task);

		bool shouldLogCompletion = false;
		double completionTimeMs = 0.0;
#ifdef DEVBENCH_BRIDGE_ENABLED
		// Snapshot of the counters latched under the lock at the moment completion is
		// detected, so the emitted event reflects that exact state — not whatever a
		// concurrent Complete()/Clear() may have changed it to after we release the lock.
		uint64_t completedSnapshot = 0;
		uint64_t failedSnapshot = 0;
		uint64_t totalSnapshot = 0;
#endif

		// Determine whether this task was resolved from the disk cache or actually compiled.
		bool wasDiskHit = cache.IsShaderLoadedFromDisk(key);

		// Perform all completion operations under one mutex acquisition
		{
			std::scoped_lock lock(compilationMutex);

			// Update task counters
			if (shaderBlob) {
				logger::debug("Compiling Task succeeded: {}", key);
				completedTasks++;
			} else {
				logger::debug("Compiling Task failed: {}", key);
				failedTasks++;
			}
			completedPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;

			// Track disk-cache hits separately so ETA can use compilation-only timing.
			if (wasDiskHit) {
				diskHitTasks++;
				diskHitPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;
			} else {
				// A real compile finishing after a prior completion is an
				// unambiguous new-session signal (disk hits can't produce it) --
				// restart the clock and un-freeze completion tracking.
				if (completionTime.load(std::memory_order_relaxed) != 0) {
					QueryPerformanceCounter(&lastReset);
					lastCalculation = lastReset;
					completionTime.store(0, std::memory_order_relaxed);
					compilationPhaseStarted.store(false, std::memory_order_relaxed);
				}
				if (!compilationPhaseStarted.load(std::memory_order_relaxed)) {
					// First actual compilation of this session: start the
					// compilation-phase clock.
					// Write the start time before the release-store so readers see it.
					QueryPerformanceCounter(&compilationPhaseStart);
					compilationPhaseStarted.store(true, std::memory_order_release);
				}
			}

			// Track heavy task completion for P-core concurrency limiting
			if (task.GetPriority() >= kHeavyPriorityThreshold) {
				auto current = heavyTasksInFlight.load(std::memory_order_relaxed);
				while (current > 0 &&
					   !heavyTasksInFlight.compare_exchange_weak(current, current - 1,
						   std::memory_order_relaxed,
						   std::memory_order_relaxed)) {
				}
			}

			// Update timing
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			totalTime.QuadPart += now.QuadPart - lastCalculation.QuadPart;
			lastCalculation = now;

			// Check if compilation is complete and set completion time if needed
			if (completionTime.load(std::memory_order_relaxed) == 0 && completedTasks + failedTasks >= totalTasks) {
				completionTime.store(now.QuadPart, std::memory_order_relaxed);
				completionTimeMs = static_cast<double>(now.QuadPart - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;
				shouldLogCompletion = true;
#ifdef DEVBENCH_BRIDGE_ENABLED
				completedSnapshot = completedTasks.load(std::memory_order_relaxed);
				failedSnapshot = failedTasks.load(std::memory_order_relaxed);
				totalSnapshot = totalTasks.load(std::memory_order_relaxed);
#endif
			}

			// Update task tracking
			processedTasks.insert(task);
			tasksInProgress.erase(task);
		}

		// Log completion outside the lock
		if (shouldLogCompletion) {
			logger::debug("Compilation completed in {} ms", GetHumanTime(completionTimeMs));

			// Unconditional final flush: the per-shader writes during the batch were
			// debounced (RecordDigestAndMaybeFlush), so guarantee the manifest is
			// durable on disk the moment the batch actually finishes, not left
			// waiting on the next batch to cross the flush threshold.
			GetShaderCacheManifest().Save();

#ifdef DEVBENCH_BRIDGE_ENABLED
			// A compilation batch finished (initial build OR a hot-reload recompile).
			// Emit one summary event so a benchmark scenario can split its A/B window
			// precisely on the moment a recompiled shader went live, and detect failures
			// without polling. Guarded on the devbench host being present.
			if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
				const nlohmann::json payload{
					{ "completedTasks", completedSnapshot },
					{ "failedTasks", failedSnapshot },
					{ "totalTasks", totalSnapshot },
					{ "durationMs", completionTimeMs },
				};
				const std::string dumped = payload.dump();
				dvb->EmitEvent("openshaders.shaderRecompiled", dumped.c_str());
			}
#endif
		}

		conditionVariable.notify_one();
	}

	void CompilationSet::Clear()
	{
		std::scoped_lock lock(compilationMutex);
		availableTasks.clear();
		tasksInProgress.clear();
		processedTasks.clear();
		totalTasks = 0;
		completedTasks = 0;
		failedTasks = 0;
		cacheHitTasks = 0;
		diskHitTasks = 0;
		diskHitPriorityWeight = 0;
		digestComputeCount = 0;
		digestComputeTimeUs = 0;
		digestDecidedTasks = 0;
		compilationPhaseStarted = false;
		compilationPhaseStart = { 0 };
		slowTasks = 0;
		verySlowTasks = 0;
		totalPriorityWeight = 0;
		completedPriorityWeight = 0;
		heavyTasksInFlight = 0;
		QueryPerformanceCounter(&lastReset);
		QueryPerformanceCounter(&lastCalculation);
		completionTime = { 0 };  // Reset completion time
		totalTime = { 0 };
		{
			std::lock_guard slowLock(slowTasksMutex);
			slowTaskRecords.clear();
		}
	}

	std::string CompilationSet::GetHumanTime(double a_totalMs)
	{
		return Util::FormatDuration(a_totalMs);
	}

	double CompilationSet::GetEta()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const int64_t endQpc = (completionTime.load(std::memory_order_relaxed) != 0) ? completionTime.load(std::memory_order_relaxed) : now.QuadPart;

		// Helper: given elapsed time and done/total priority weights, return remaining ms (or 0).
		auto weightedEta = [](double elapsedMs, double doneW, double totalW) -> double {
			if (elapsedMs <= 0.0 || doneW <= 0.0 || totalW <= 0.0)
				return 0.0;
			double fraction = doneW / totalW;
			return std::max(elapsedMs / fraction - elapsedMs, 0.0);
		};

		const uint64_t diskWeight = diskHitPriorityWeight.load(std::memory_order_relaxed);
		const uint64_t totalWeight = totalPriorityWeight.load(std::memory_order_relaxed);
		const uint64_t doneWeight = completedPriorityWeight.load(std::memory_order_relaxed);

		if (diskWeight > 0) {
			// There are disk-cache hits in this session.
			if (!compilationPhaseStarted.load(std::memory_order_acquire)) {
				// Compilations haven't started yet (still loading from disk cache).
				// We have no compilation rate to extrapolate from, so return 0 to
				// avoid a wildly wrong ETA based purely on the fast disk-hit rate.
				return 0.0;
			}

			// At least one actual compilation has completed.  Use compilation-phase
			// timing so that fast disk loads at the start of the session don't inflate
			// the apparent progress rate and produce an underestimated ETA.
			const int64_t phaseStart = compilationPhaseStart.QuadPart;  // visible due to acquire above
			double compilationElapsedMs = static_cast<double>(endQpc - phaseStart) * 1000.0 / frequency.QuadPart;

			// Exclude disk-hit weight from both numerator and denominator so the
			// rate reflects only the actual compilation speed.
			double compiledDone = static_cast<double>(doneWeight > diskWeight ? doneWeight - diskWeight : 0);
			double compiledTotal = static_cast<double>(totalWeight > diskWeight ? totalWeight - diskWeight : 0);
			return weightedEta(compilationElapsedMs, compiledDone, compiledTotal);
		}

		// No disk hits: fall back to the original whole-session ETA.
		// Priority-weighted so heavy tasks completing early don't inflate the estimate.
		double elapsedMs = static_cast<double>(endQpc - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;
		return weightedEta(elapsedMs, static_cast<double>(doneWeight), static_cast<double>(totalWeight));
	}

	std::string CompilationSet::GetStatsString(bool a_timeOnly, bool a_elapsedOnly)
	{
		// Calculate elapsed time since compilation started
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);

		// Use completion time if compilation is finished, otherwise current time
		int64_t endTime = (completionTime.load(std::memory_order_relaxed) != 0) ? completionTime.load(std::memory_order_relaxed) : currentTime.QuadPart;
		double totalMs = static_cast<double>(endTime - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;

		if (a_timeOnly) {
			if (a_elapsedOnly) {
				// Only elapsed
				return GetHumanTime(totalMs);
			} else {
				// Elapsed + estimated
				return fmt::format("{}/{}",
					GetHumanTime(totalMs),
					GetHumanTime(GetEta() + totalMs));
			}
		}

		return fmt::format("{}/{} (successful/total)\tfailed: {}\tdeduplicated: {}\tdisk cache: {}\tdigest-verified: {}\tdigest time: {:.1f}ms ({} calls)\nElapsed/Estimated Time: {}/{}",
			(std::uint64_t)completedTasks,
			(std::uint64_t)totalTasks,
			(std::uint64_t)failedTasks,
			(std::uint64_t)cacheHitTasks,
			(std::uint64_t)diskHitTasks,
			(std::uint64_t)digestDecidedTasks,
			(double)digestComputeTimeUs / 1000.0,
			(std::uint64_t)digestComputeCount,
			GetHumanTime(totalMs),
			GetHumanTime(GetEta() + totalMs));
	}

	UpdateListener::UpdateListener(ShaderFileDependencyTracker* deps_) :
		deps(deps_) {}

	void UpdateListener::UpdateCache(const std::filesystem::path& filePath, SIE::ShaderCache* cache, bool& clearCache, bool& fileDone)
	{
		fileDone = true;
		// Skip directories
		if (std::filesystem::is_directory(filePath)) {
			return;
		}
		// Extract file components
		const std::string extension = filePath.extension().string();
		const std::string shaderTypeString = filePath.stem().string();
		std::chrono::time_point<std::chrono::system_clock> modifiedTime{};
		auto shaderType = magic_enum::enum_cast<RE::BSShader::Type>(shaderTypeString, magic_enum::case_insensitive);
		// Check if the file exists and get its modified time
		if (std::filesystem::exists(filePath)) {
			modifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath));
		} else {
			return;
		}

		// Ensure the file is not a directory and is a valid shader file (.hlsl)
		std::string lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (!std::filesystem::is_directory(filePath) && lowerExtension == ".hlsl") {
			// Update cache with the modified shader
			cache->InsertModifiedShaderMap(shaderTypeString, modifiedTime);

			// Attempt to mark the shader for recompilation
			bool foundPath = cache->Clear(filePath.string());

			if (!foundPath) {
				// File was not found in the the map so check its shader type
				std::string parentDirName = filePath.parent_path().filename().string();
				std::transform(parentDirName.begin(), parentDirName.end(), parentDirName.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				// Check if the parent directory name matches "shaders" in a case-insensitive way
				if (lowerExtension == ".hlsl" && parentDirName == "shaders" && shaderType.has_value()) {
					cache->Clear(shaderType.value());
				} else {
					// If it's not specifically handled, clear all cache
					clearCache = true;
				}
			}
		}
		// Handle include file changes (.hlsli) by invalidating dependents
		else if (!std::filesystem::is_directory(filePath) && lowerExtension == ".hlsli") {
			// Normalize to absolute canonical path to match how dependencies are tracked
			std::error_code ec;
			auto canonicalPath = std::filesystem::weakly_canonical(filePath, ec);
			std::string pathStr = (ec ? filePath.string() : canonicalPath.string());
			// On Windows, normalize to lowercase to match TrackingIncludeHandler
#ifdef _WIN32
			std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
			// Invalidate all .hlsl files that depend on this .hlsli
			auto dependents = deps->GetDependents(pathStr);
			for (const auto& hlsl : dependents) {
				cache->Clear(hlsl);
			}
		}
		// Indicate that file processing is not yet complete
		fileDone = false;
	}

	void UpdateListener::processQueue()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		std::unique_lock lock(actionMutex, std::defer_lock);
		auto cache = globals::shaderCache;
		while (cache->UseFileWatcher()) {
			lock.lock();
			if (!queue.empty()) {
				bool clearCache = false;
				for (fileAction fAction : queue) {
					const std::filesystem::path filePath = std::filesystem::path(std::format("{}\\{}", fAction.dir, fAction.filename));
					bool fileDone = false;
					switch (fAction.action) {
					case efsw::Actions::Add:
						logger::debug("Detected Added path {}", filePath.string());
						UpdateCache(filePath, cache, clearCache, fileDone);
						break;
					case efsw::Actions::Delete:
						logger::debug("Detected Deleted path {}", filePath.string());
						break;
					case efsw::Actions::Modified:
						if (!std::filesystem::is_directory(filePath)) {
							logger::debug("Detected Changed path {}", filePath.string());
						}
						UpdateCache(filePath, cache, clearCache, fileDone);
						break;
					case efsw::Actions::Moved:
						logger::debug("Detected Moved path {}", filePath.string());
						break;
					default:
						logger::error("Filewatcher received invalid action {}", magic_enum::enum_name(fAction.action));
					}
					if (fileDone)
						continue;
				}
				if (clearCache) {
					// DeleteDiskCache() also resets boot-mismatch/rollback UI state that
					// the menu reads unsynchronized on the main thread; this watcher
					// thread only needs the on-disk directories gone.
					cache->DeleteDiskCacheFiles();
					cache->Clear();
				}
				queue.clear();
			}
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		queue.clear();
	}

	void UpdateListener::handleFileAction(efsw::WatchID watchid, const std::string& dir, const std::string& filename, efsw::Action action, std::string oldFilename)
	{
		std::lock_guard lock(actionMutex);
		if (queue.empty() || (queue.back().action != action && queue.back().filename != filename)) {
			// only add if not a duplicate; esfw is very spammy
			queue.push_back({ watchid, dir, filename, action, oldFilename });
		}
	}
}
