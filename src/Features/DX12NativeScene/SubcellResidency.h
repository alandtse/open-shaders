#pragma once
#include "SpatialSubcells.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace DX12Native::SubcellResidency
{
	using SpatialSubcells::Coordinate;

	enum class State : uint8_t
	{
		Missing,
		Preparing,
		DiskCached,
		GPUUploaded,
		BLASReady,
		TLASIntegrated,
		GPUEvicted,
		Stale,
		Failed
	};

	struct Tile
	{
		State state = State::Missing;
		std::string digest;
		uint64_t sceneRevision = 0;
		uint64_t generation = 0;
		uint64_t gpuBytes = 0;
		bool active = false, predictive = false;
	};

	/** CPU-side bookkeeping only. The GPU owner remains responsible for
     *  fences, descriptors, BLAS lifetime, and actual resource destruction.
     *  No state transition can itself release an in-flight GPU resource. */
	class Ledger
	{
	public:
		using Key = std::pair<int64_t, int64_t>;
		void ResetWorld(uint64_t revision)
		{
			if (revision == worldRevision)
				return;
			worldRevision = revision;
			tiles.clear();
			center = {};
		}

		void Recenter(Coordinate target)
		{
			center = target;
			const auto active = SpatialSubcells::Active3x3(center);
			const auto predictive = SpatialSubcells::OuterRing(center);
			for (auto& [key, value] : tiles) value.active = value.predictive = false;
			for (auto p : active) Get(p).active = true;
			for (auto p : predictive.sides) Get(p).predictive = true;
			for (auto p : predictive.corners) Get(p).predictive = true;
		}

		Tile& Get(Coordinate p) { return tiles[{ p.x, p.y }]; }
		const Tile* Find(Coordinate p) const
		{
			auto it = tiles.find({ p.x, p.y });
			return it == tiles.end() ? nullptr : &it->second;
		}

		bool StartPrepare(Coordinate p, uint64_t revision)
		{
			if (revision != worldRevision)
				return false;
			auto& tile = Get(p);
			if (tile.state != State::Missing && tile.state != State::Stale &&
				tile.state != State::Failed)
				return false;
			tile.state = State::Preparing;
			tile.sceneRevision = revision;
			++tile.generation;
			return true;
		}

		bool PublishDisk(Coordinate p, uint64_t revision, uint64_t generation,
			std::string digest)
		{
			auto& tile = Get(p);
			if (revision != worldRevision || tile.sceneRevision != revision ||
				tile.generation != generation || tile.state != State::Preparing ||
				digest.empty())
				return false;
			tile.digest = std::move(digest);
			tile.state = State::DiskCached;
			return true;
		}

		bool PublishGpu(Coordinate p, uint64_t revision, uint64_t generation,
			uint64_t residentBytes)
		{
			auto& tile = Get(p);
			if (revision != worldRevision || tile.sceneRevision != revision ||
				tile.generation != generation ||
				(tile.state != State::DiskCached && tile.state != State::GPUEvicted))
				return false;
			tile.gpuBytes = residentBytes;
			tile.state = State::GPUUploaded;
			return true;
		}

		bool MarkBLAS(Coordinate p, uint64_t revision, uint64_t generation)
		{
			auto& tile = Get(p);
			if (revision != worldRevision || tile.sceneRevision != revision ||
				tile.generation != generation || tile.state != State::GPUUploaded)
				return false;
			tile.state = State::BLASReady;
			return true;
		}

		bool MarkTLAS(Coordinate p, uint64_t revision, uint64_t generation)
		{
			auto& tile = Get(p);
			if (revision != worldRevision || tile.sceneRevision != revision ||
				tile.generation != generation || tile.state != State::BLASReady ||
				!tile.active)
				return false;
			tile.state = State::TLASIntegrated;
			return true;
		}

		// Call only after the GPU owner's completion fence and all consumers
		// have retired. Disk digests and world identity remain intact.
		bool RetireGPU(Coordinate p)
		{
			auto& tile = Get(p);
			if (tile.active || tile.state < State::GPUUploaded ||
				tile.state > State::TLASIntegrated)
				return false;
			tile.gpuBytes = 0;
			tile.state = State::GPUEvicted;
			return true;
		}

		void Invalidate(Coordinate p)
		{
			auto& tile = Get(p);
			// GPU ownership must retire separately; this only rejects
			// subsequent publication and increments the cache generation.
			++tile.generation;
			tile.state = State::Stale;
			tile.digest.clear();
		}

		uint32_t ActiveReady() const
		{
			uint32_t count = 0;
			for (auto p : SpatialSubcells::Active3x3(center))
				if (const auto* t = Find(p); t && t->state >= State::DiskCached &&
											 t->state <= State::TLASIntegrated)
					++count;
			return count;
		}

		uint64_t TrackedGPUBytes() const
		{
			uint64_t bytes = 0;
			for (const auto& [key, tile] : tiles) bytes += tile.gpuBytes;
			return bytes;
		}

		Coordinate Center() const { return center; }
		uint64_t Revision() const { return worldRevision; }
		size_t TrackedTiles() const { return tiles.size(); }

	private:
		uint64_t worldRevision = 0;
		Coordinate center{};
		std::map<Key, Tile> tiles;
	};
}
