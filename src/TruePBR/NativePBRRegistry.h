#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NativePBR
{
	enum class MaterialKind : uint8_t { Mesh, Landscape };
	enum class TextureRole : size_t { BaseColor, Normal, Rmaos, Emission, Displacement, Features0, Features1, Count };

	struct Glint
	{
		bool enabled = false;
		float screenScale = 1.5f, microfacetDensity = 40.0f, roughness = 0.015f, randomness = 2.0f;
		bool operator==(const Glint&) const = default;
	};

	struct LandscapeTile
	{
		bool isPbr = false;
		float roughnessScale = 1.0f, displacementScale = 1.0f, specularLevel = 0.04f;
		std::array<std::string, 4> textureNames{};
		Glint glint;
		bool operator==(const LandscapeTile&) const = default;
	};

	/** @brief Records DX11 texture properties observed during a material capture. */
	struct TextureResourceInfo
	{
		uint32_t width = 0, height = 0, mipLevels = 0, arraySize = 0, format = 0;
		uint32_t miscFlags = 0, bindFlags = 0, usage = 0, sampleCount = 0;
		bool texture2D = false;
		bool operator==(const TextureResourceInfo&) const = default;
	};

	/** @brief Engine-pointer-free material metadata for native consumers. */
	struct MaterialData
	{
		MaterialKind kind = MaterialKind::Mesh;
		uint64_t textureGeneration = 0;
		uint32_t pbrFlags = 0, clampMode = 0, landscapeTileCount = 0;
		std::array<std::array<float, 4>, 2> uvOffsetScale{ { { 0, 0, 1, 1 }, { 0, 0, 1, 1 } } };
		float materialAlpha = 1.0f;
		float roughnessScale = 1.0f, displacementScale = 1.0f, specularLevel = 0.04f;
		float coatStrength = 0.0f, coatRoughness = 1.0f, coatSpecularLevel = 0.04f;
		float subsurfaceOpacity = 0.0f, fuzzWeight = 0.0f;
		std::array<float, 3> coatColor{ 1.0f, 1.0f, 1.0f }, subsurfaceColor{}, fuzzColor{};
		std::array<float, 3> projectedColor{ 1.0f, 1.0f, 1.0f };
		float projectedRoughness = 1.0f, projectedSpecularLevel = 0.04f;
		Glint glint, projectedGlint;
		std::array<std::string, static_cast<size_t>(TextureRole::Count)> texturePaths{};
		std::array<std::string, static_cast<size_t>(TextureRole::Count)> textureResourceNames{};
		std::array<TextureResourceInfo, static_cast<size_t>(TextureRole::Count)> textureResources{};
		std::array<LandscapeTile, 6> landscapeTiles{};
		bool texturePathsComplete = false;
		bool operator==(const MaterialData&) const = default;
	};

	/** @brief Rejects non-finite material parameters before they reach native consumers. */
	inline bool IsFinite(const MaterialData& a_data)
	{
		auto finite = [](float value) { return std::isfinite(value); };
		auto floats = [&](const auto& values) { for (float value : values) if (!finite(value)) return false; return true; };
		auto glint = [&](const Glint& value) {
			return finite(value.screenScale) && finite(value.microfacetDensity) && finite(value.roughness) && finite(value.randomness);
		};
		for (const auto& uv : a_data.uvOffsetScale) if (!floats(uv)) return false;
		if (!floats(a_data.coatColor) || !floats(a_data.subsurfaceColor) || !floats(a_data.fuzzColor) ||
			!floats(a_data.projectedColor) || !glint(a_data.glint) || !glint(a_data.projectedGlint)) return false;
		const std::array scalars{ a_data.materialAlpha, a_data.roughnessScale, a_data.displacementScale, a_data.specularLevel,
			a_data.coatStrength, a_data.coatRoughness, a_data.coatSpecularLevel, a_data.subsurfaceOpacity,
			a_data.fuzzWeight, a_data.projectedRoughness, a_data.projectedSpecularLevel };
		if (!floats(scalars) || a_data.landscapeTileCount > a_data.landscapeTiles.size()) return false;
		for (const auto& tile : a_data.landscapeTiles)
			if (!finite(tile.roughnessScale) || !finite(tile.displacementScale) || !finite(tile.specularLevel) || !glint(tile.glint)) return false;
		return true;
	}

	struct Snapshot
	{
		uint64_t identity = 0, revision = 0;
		MaterialData material;
	};

	/** @brief Captures per-geometry shader and alpha state without retaining engine objects. */
	struct InstanceProperties
	{
		uint64_t shaderFlags = 0;
		uint16_t alphaFlags = 0;
		uint8_t alphaThreshold = 0;
		bool alphaProperty = false, skinned = false, emissionKnown = false;
		std::array<float, 3> emissiveColor{};
		float emissiveMultiplier = 0;
		bool operator==(const InstanceProperties&) const = default;
	};

	struct InstanceHint
	{
		uint64_t materialIdentity = 0;
		std::string referenceKey, shapeName;
		InstanceProperties properties;
		bool propertiesConflict = false;
	};

	struct Statistics
	{
		uint64_t revision = 0, published = 0, forgotten = 0;
		size_t live = 0, dirty = 0, mesh = 0, landscape = 0, instanceHints = 0;
		uint64_t hintLimitHits = 0;
	};

	/** @brief Thread-safe snapshot boundary; engine objects are accessed only by the caller. */
	class Registry
	{
	public:
		static constexpr size_t kMaximumInstanceHints = 4096;
		static constexpr size_t kMaximumHintsPerMaterial = 64;
		struct Capture
		{
			uint64_t identity = 0, mutation = 0, textureGeneration = 0;
			explicit operator bool() const { return identity != 0; }
		};

		/** @brief Claims the current material generation for a render-thread snapshot. */
		Capture BeginCapture(const void* a_source)
		{
			if (!a_source) return {};
			{
				std::shared_lock lock(mutex);
				if (auto it = entries.find(a_source); it != entries.end() && !it->second.dirty) return {};
			}
			std::unique_lock lock(mutex);
			auto [it, inserted] = entries.try_emplace(a_source);
			if (inserted) { it->second.identity = nextIdentity++; ++revision; }
			const auto& entry = it->second;
			return entry.dirty ? Capture{ entry.identity, entry.mutation, entry.textureGeneration } : Capture{};
		}

		/** @brief Observes base and texture identities and returns a revision-safe capture token. */
		template <size_t N>
		Capture ObserveAndBegin(const void* a_source, const std::array<std::array<float, 4>, 2>& a_uv,
			float a_alpha, uint32_t a_clamp, const std::array<const void*, N>& a_textures,
			const std::array<const void*, static_cast<size_t>(TextureRole::Count) * 2>& a_resources = {})
		{
			static_assert(N == static_cast<size_t>(TextureRole::Count) + 1 || N == 24);
			if (!a_source) return {};
			auto unchanged = [&](const Entry& entry) {
				const bool base = entry.baseObserved && entry.observedUv == a_uv && entry.observedAlpha == a_alpha && entry.observedClamp == a_clamp;
				if constexpr (N == 24) return base && entry.landscapeTexturesObserved && entry.landscapeTextures == a_textures;
				else return base && entry.texturesObserved && entry.textures == a_textures && entry.resourcesObserved && entry.resources == a_resources;
			};
			{
				std::shared_lock lock(mutex);
				auto it = entries.find(a_source);
				if (it != entries.end() && !it->second.dirty && unchanged(it->second)) return {};
			}
			std::unique_lock lock(mutex);
			auto [it, inserted] = entries.try_emplace(a_source);
			auto& entry = it->second;
			if (inserted) { entry.identity = nextIdentity++; ++revision; }
			if (!unchanged(entry)) {
				entry.baseObserved = true;
				entry.observedUv = a_uv;
				entry.observedAlpha = a_alpha;
				entry.observedClamp = a_clamp;
				if constexpr (N == 24) { entry.landscapeTextures = a_textures; entry.landscapeTexturesObserved = true; }
				else {
					if (!entry.texturesObserved || entry.textures != a_textures || !entry.resourcesObserved || entry.resources != a_resources) ++entry.textureGeneration;
					entry.textures = a_textures; entry.texturesObserved = true;
					entry.resources = a_resources; entry.resourcesObserved = true;
				}
				++entry.mutation;
				if (!entry.dirty) { entry.dirty = true; ++revision; }
			}
			return entry.dirty ? Capture{ entry.identity, entry.mutation, entry.textureGeneration } : Capture{};
		}

		/** @brief Invalidates a published snapshot when observed base parameters change. */
		void ObserveBase(const void* a_source, const std::array<std::array<float, 4>, 2>& a_uv, float a_alpha, uint32_t a_clamp)
		{
			auto unchanged = [&](const Entry& entry) {
				return entry.baseObserved && entry.observedUv == a_uv && entry.observedAlpha == a_alpha && entry.observedClamp == a_clamp;
			};
			{
				std::shared_lock lock(mutex);
				auto it = entries.find(a_source);
				if (it == entries.end() || unchanged(it->second)) return;
			}
			std::unique_lock lock(mutex);
			auto it = entries.find(a_source);
			if (it == entries.end() || unchanged(it->second)) return;
			auto& entry = it->second;
			entry.baseObserved = true;
			entry.observedUv = a_uv;
			entry.observedAlpha = a_alpha;
			entry.observedClamp = a_clamp;
			++entry.mutation;
			if (!entry.dirty && (entry.data.uvOffsetScale != a_uv || entry.data.materialAlpha != a_alpha || entry.data.clampMode != a_clamp)) {
				entry.dirty = true;
				++revision;
			}
		}

		/** @brief Tracks texture-object replacement without exposing engine pointers to consumers. */
		void ObserveTextures(const void* a_source, const std::array<const void*, static_cast<size_t>(TextureRole::Count) + 1>& a_textures)
		{
			ObserveTextureSet(a_source, a_textures, &Entry::textures, &Entry::texturesObserved);
		}

		/** @brief Tracks texture replacement across the six landscape layers. */
		void ObserveLandscapeTextures(const void* a_source, const std::array<const void*, 24>& a_textures)
		{
			ObserveTextureSet(a_source, a_textures, &Entry::landscapeTextures, &Entry::landscapeTexturesObserved);
		}

		/** @brief Invalidates a live material after a texture or parameter mutation. */
		void MarkDirty(const void* a_source)
		{
			std::unique_lock lock(mutex);
			if (auto it = entries.find(a_source); it != entries.end()) {
				++it->second.mutation;
				if (!it->second.dirty) { it->second.dirty = true; ++revision; }
			}
		}

		/** @brief Invalidates cached data without changing stable material identities. */
		void MarkAllDirty()
		{
			std::unique_lock lock(mutex);
			for (auto& [source, entry] : entries) {
				++entry.mutation;
				if (!entry.dirty) { entry.dirty = true; ++revision; }
			}
		}

		/** @brief Publishes only if the captured identity and mutation are still current. */
		bool Publish(const void* a_source, Capture a_capture, MaterialData a_data)
		{
			if (!IsFinite(a_data)) return false;
			std::unique_lock lock(mutex);
			auto it = entries.find(a_source);
			if (it == entries.end() || !a_capture || it->second.identity != a_capture.identity ||
				it->second.mutation != a_capture.mutation || !it->second.dirty) return false;
			auto& entry = it->second;
			a_data.textureGeneration = a_capture.textureGeneration;
			if (entry.baseObserved && (entry.observedUv != a_data.uvOffsetScale || entry.observedAlpha != a_data.materialAlpha ||
				entry.observedClamp != a_data.clampMode)) return false;
			if (entry.revision && entry.data == a_data) {
				entry.dirty = false;
				++revision;
				return true;
			}
			entry.data = std::move(a_data);
			entry.dirty = false;
			entry.revision = ++revision;
			++published;
			return true;
		}

		/** @brief Records a bounded, unverified reference-and-shape association. */
		void ObserveInstance(const void* a_source, std::string a_reference, std::string a_shape, InstanceProperties a_properties = {})
		{
			if (!a_source || a_reference.empty() || a_reference.size() > 256 || a_shape.empty() || a_shape.size() > 1024) return;
			{
				std::shared_lock lock(mutex);
				if (auto it = entries.find(a_source); it != entries.end())
					for (const auto& hint : it->second.hints)
						if (hint.referenceKey == a_reference && hint.shapeName == a_shape && (hint.propertiesConflict || hint.properties == a_properties)) return;
			}
			std::unique_lock lock(mutex);
			auto [it, inserted] = entries.try_emplace(a_source);
			auto& entry = it->second;
			if (inserted) { entry.identity = nextIdentity++; ++revision; }
			for (auto& hint : entry.hints)
				if (hint.referenceKey == a_reference && hint.shapeName == a_shape) {
					if (!hint.propertiesConflict && hint.properties != a_properties) { hint.propertiesConflict = true; ++revision; }
					return;
				}
			if (entry.hints.size() >= kMaximumHintsPerMaterial || totalHints >= kMaximumInstanceHints) { ++hintLimitHits; return; }
			entry.hints.push_back({ entry.identity, std::move(a_reference), std::move(a_shape), a_properties });
			++totalHints;
			++revision;
		}

		/** @brief Copies unverified instance associations without engine pointers. */
		std::vector<InstanceHint> ExportInstanceHints() const
		{
			std::shared_lock lock(mutex);
			std::vector<InstanceHint> result;
			for (const auto& [source, entry] : entries)
				result.insert(result.end(), entry.hints.begin(), entry.hints.end());
			return result;
		}

		/** @brief Drops scene observations while retaining material identities and snapshots. */
		void ClearInstanceHints()
		{
			std::unique_lock lock(mutex);
			if (!totalHints) return;
			for (auto& [source, entry] : entries) entry.hints.clear();
			totalHints = 0;
			++revision;
		}

		/** @brief Removes the old identity before an engine material address can be reused. */
		void Forget(const void* a_source)
		{
			std::unique_lock lock(mutex);
			if (auto it = entries.find(a_source); it != entries.end()) {
				totalHints -= it->second.hints.size();
				entries.erase(it);
				++revision;
				++forgotten;
			}
		}

		/** @brief Copies only owned metadata, safe for background and DX12 preparation. */
		std::vector<Snapshot> Export() const
		{
			std::shared_lock lock(mutex);
			std::vector<Snapshot> result;
			result.reserve(entries.size());
			for (const auto& [source, entry] : entries)
				if (!entry.dirty) result.push_back({ entry.identity, entry.revision, entry.data });
			return result;
		}

		struct ExportedGeneration
		{
			uint64_t revision = 0;
			std::vector<Snapshot> snapshots;
			std::vector<uint64_t> pendingIdentities;
		};

		/** @brief Copies a consistent generation only when the registry revision differs. */
		std::optional<ExportedGeneration> ExportIfChanged(uint64_t a_revision) const
		{
			std::shared_lock lock(mutex);
			if (revision == a_revision) return std::nullopt;
			ExportedGeneration result;
			result.revision = revision;
			result.snapshots.reserve(entries.size());
			for (const auto& [source, entry] : entries) {
				if (entry.dirty) result.pendingIdentities.push_back(entry.identity);
				else result.snapshots.push_back({ entry.identity, entry.revision, entry.data });
			}
			return result;
		}

		struct ChangeSet
		{
			uint64_t revision = 0;
			bool hasMore = false;
			std::vector<Snapshot> changed;
			std::vector<uint64_t> liveIdentities, pendingIdentities;
		};

		/** @brief Exports changed metadata and current identities against consumer-owned revisions. */
		std::optional<ChangeSet> ExportChanges(uint64_t a_revision, const std::unordered_map<uint64_t, uint64_t>& a_known, size_t a_limit = 32) const
		{
			std::shared_lock lock(mutex);
			if (revision == a_revision) return std::nullopt;
			ChangeSet result;
			if (!a_limit) a_limit = 1;
			result.revision = revision;
			result.liveIdentities.reserve(entries.size());
			for (const auto& [source, entry] : entries) {
				result.liveIdentities.push_back(entry.identity);
				if (entry.dirty) result.pendingIdentities.push_back(entry.identity);
				else if (auto known = a_known.find(entry.identity); known == a_known.end() || known->second != entry.revision) {
					if (result.changed.size() < a_limit) result.changed.push_back({ entry.identity, entry.revision, entry.data });
					else { result.hasMore = true; result.pendingIdentities.push_back(entry.identity); }
				}
			}
			return result;
		}

		/** @brief Returns the current registry generation without copying its entries. */
		uint64_t Revision() const
		{
			std::shared_lock lock(mutex);
			return revision;
		}

		/** @brief Returns registration diagnostics without exposing engine pointers. */
		Statistics GetStatistics() const
		{
			std::shared_lock lock(mutex);
			Statistics result{};
			result.revision = revision;
			result.published = published;
			result.forgotten = forgotten;
			result.hintLimitHits = hintLimitHits;
			result.live = entries.size();
			for (const auto& [source, entry] : entries) {
				result.dirty += entry.dirty ? 1u : 0u;
				result.instanceHints += entry.hints.size();
				if (entry.revision) {
					result.mesh += entry.data.kind == MaterialKind::Mesh ? 1u : 0u;
					result.landscape += entry.data.kind == MaterialKind::Landscape ? 1u : 0u;
				}
			}
			return result;
		}

	private:
		struct Entry
		{
			uint64_t identity = 0, revision = 0, mutation = 0, textureGeneration = 0;
			bool dirty = true, texturesObserved = false, landscapeTexturesObserved = false, resourcesObserved = false, baseObserved = false;
			std::array<std::array<float, 4>, 2> observedUv{};
			float observedAlpha = 0;
			uint32_t observedClamp = 0;
			std::array<const void*, static_cast<size_t>(TextureRole::Count) + 1> textures{};
			std::array<const void*, static_cast<size_t>(TextureRole::Count) * 2> resources{};
			std::array<const void*, 24> landscapeTextures{};
			MaterialData data;
			std::vector<InstanceHint> hints;
		};

		template <size_t N>
		void ObserveTextureSet(const void* a_source, const std::array<const void*, N>& a_textures,
			std::array<const void*, N> Entry::* a_values, bool Entry::* a_observed)
		{
			if (!a_source) return;
			{
				std::shared_lock lock(mutex);
				auto it = entries.find(a_source);
				if (it != entries.end() && it->second.*a_observed && it->second.*a_values == a_textures) return;
			}
			std::unique_lock lock(mutex);
			auto [it, inserted] = entries.try_emplace(a_source);
			auto& entry = it->second;
			if (inserted) { entry.identity = nextIdentity++; ++revision; }
			if (!(entry.*a_observed) || entry.*a_values != a_textures) {
				entry.*a_values = a_textures;
				entry.*a_observed = true;
				++entry.mutation;
				if (!entry.dirty) { entry.dirty = true; ++revision; }
			}
		}

		mutable std::shared_mutex mutex;
		std::unordered_map<const void*, Entry> entries;
		size_t totalHints = 0;
		uint64_t nextIdentity = 1, revision = 0, published = 0, forgotten = 0, hintLimitHits = 0;
	};
}
