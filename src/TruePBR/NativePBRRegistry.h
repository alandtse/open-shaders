#pragma once

#include <array>
#include <cstdint>
#include <mutex>
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
		Glint glint;
		bool operator==(const LandscapeTile&) const = default;
	};

	/** @brief Engine-pointer-free material metadata for native consumers. */
	struct MaterialData
	{
		MaterialKind kind = MaterialKind::Mesh;
		uint32_t pbrFlags = 0, clampMode = 0, landscapeTileCount = 0;
		float roughnessScale = 1.0f, displacementScale = 1.0f, specularLevel = 0.04f;
		float coatStrength = 0.0f, coatRoughness = 1.0f, coatSpecularLevel = 0.04f;
		float subsurfaceOpacity = 0.0f, fuzzWeight = 0.0f;
		std::array<float, 3> coatColor{ 1.0f, 1.0f, 1.0f }, subsurfaceColor{}, fuzzColor{};
		std::array<float, 3> projectedColor{ 1.0f, 1.0f, 1.0f };
		float projectedRoughness = 1.0f, projectedSpecularLevel = 0.04f;
		Glint glint, projectedGlint;
		std::array<std::string, static_cast<size_t>(TextureRole::Count)> texturePaths{};
		std::array<LandscapeTile, 6> landscapeTiles{};
		bool texturePathsComplete = false;
		bool operator==(const MaterialData&) const = default;
	};

	struct Snapshot
	{
		uint64_t identity = 0, revision = 0;
		MaterialData material;
	};

	struct Statistics
	{
		uint64_t revision = 0, published = 0, forgotten = 0;
		size_t live = 0, dirty = 0, mesh = 0, landscape = 0;
	};

	/** @brief Thread-safe snapshot boundary; engine objects are accessed only by the caller. */
	class Registry
	{
	public:
		/** @brief Checks whether the live render thread should collect a material. */
		bool NeedsCapture(const void* a_source) const
		{
			std::shared_lock lock(mutex);
			auto it = entries.find(a_source);
			return it == entries.end() || it->second.dirty;
		}

		/** @brief Invalidates a live material after a texture or parameter mutation. */
		void MarkDirty(const void* a_source)
		{
			std::unique_lock lock(mutex);
			if (auto it = entries.find(a_source); it != entries.end()) it->second.dirty = true;
		}

		/** @brief Invalidates cached data without changing stable material identities. */
		void MarkAllDirty()
		{
			std::unique_lock lock(mutex);
			for (auto& [source, entry] : entries) entry.dirty = true;
		}

		/** @brief Publishes a fully copied material after render-thread validation. */
		void Publish(const void* a_source, MaterialData a_data)
		{
			if (!a_source) return;
			std::unique_lock lock(mutex);
			auto [it, inserted] = entries.try_emplace(a_source);
			auto& entry = it->second;
			if (inserted) entry.identity = nextIdentity++;
			if (!inserted && entry.data == a_data) {
				entry.dirty = false;
				return;
			}
			entry.data = std::move(a_data);
			entry.dirty = false;
			entry.revision = ++revision;
			++published;
		}

		/** @brief Removes the old identity before an engine material address can be reused. */
		void Forget(const void* a_source)
		{
			std::unique_lock lock(mutex);
			if (entries.erase(a_source)) { ++revision; ++forgotten; }
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

		/** @brief Returns registration diagnostics without exposing engine pointers. */
		Statistics GetStatistics() const
		{
			std::shared_lock lock(mutex);
			Statistics result{};
			result.revision = revision;
			result.published = published;
			result.forgotten = forgotten;
			result.live = entries.size();
			for (const auto& [source, entry] : entries) {
				result.dirty += entry.dirty ? 1u : 0u;
				result.mesh += entry.data.kind == MaterialKind::Mesh ? 1u : 0u;
				result.landscape += entry.data.kind == MaterialKind::Landscape ? 1u : 0u;
			}
			return result;
		}

	private:
		struct Entry
		{
			uint64_t identity = 0, revision = 0;
			bool dirty = true;
			MaterialData data;
		};

		mutable std::shared_mutex mutex;
		std::unordered_map<const void*, Entry> entries;
		uint64_t nextIdentity = 1, revision = 0, published = 0, forgotten = 0;
	};
}
