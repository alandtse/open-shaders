#include "DX12NativeRuntimeCapture.h"

#include "Globals.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <ranges>

namespace DX12Native
{
	namespace
	{
		constexpr uint64_t kSingleCopyBytes = 1024ull * 1024;
		constexpr uint32_t kFrameCopyBytes = 2u * 1024u * 1024u;
		constexpr size_t kPacketLimit = 128;
		constexpr uint64_t kFnvPrime = 1099511628211ull;
		constexpr uint64_t kFnvOffset = 14695981039346656037ull;

		uint64_t StableObjectKey(const RuntimeGeometryCandidate& source)
		{
			if (!source.objectIdentity)
				return 0;  // No reliable per-object key: never retire a similarly named sibling.
			uint64_t hash = kFnvOffset;
			const auto add = [&](unsigned char value) { hash = (hash ^ value) * kFnvPrime; };
			for (unsigned char c : source.referenceKey) add(c);
			add(0);
			for (unsigned char c : source.shapeName) add(c);
			add(0);
			for (unsigned n = 0; n < 8; ++n)
				add(static_cast<unsigned char>(source.objectIdentity >> (n * 8)));
			return hash ? hash : 1;
		}

		uint64_t Identity(const RuntimeGeometryCandidate& source)
		{
			uint64_t hash = kFnvOffset;
			auto add = [&](unsigned char value) { hash = (hash ^ value) * kFnvPrime; };
			for (unsigned char c : source.referenceKey) add(c);
			add(0);
			for (unsigned char c : source.shapeName) add(c);
			add(0);
			// Sibling meshes may share a name, transform and triangle count.
			// Pointer identity distinguishes those meshes for this runtime cell.
			for (unsigned n = 0; n < 8; ++n)
				add(static_cast<unsigned char>(source.objectIdentity >> (n * 8)));
			for (float value : source.transform) {
				const uint32_t bits = std::bit_cast<uint32_t>(value);
				for (unsigned n = 0; n < 4; ++n)
					add(static_cast<unsigned char>(bits >> (n * 8)));
			}
			for (uint32_t value : { source.vertexCount, source.triangleCount })
				for (unsigned n = 0; n < 4; ++n)
					add(static_cast<unsigned char>(value >> (n * 8)));
			return hash ? hash : 1;
		}

		// Compare actual rigid geometry, not subcell readiness or camera coverage.
		// Subcell-prepared shapes take precedence over equivalent runtime shapes.
		bool SameShape(const DX12::Shape& a, const DX12::Shape& b)
		{
			if (a.positions.size() != b.positions.size() || a.indices != b.indices)
				return false;
			for (size_t i = 0; i < a.transform.size(); ++i)
				if (std::abs(a.transform[i] - b.transform[i]) > 0.01f)
					return false;
			for (size_t i = 0; i < a.positions.size(); ++i) {
				const auto& x = a.positions[i];
				const auto& y = b.positions[i];
				if (std::abs(x.x - y.x) > 0.01f ||
					std::abs(x.y - y.y) > 0.01f ||
					std::abs(x.z - y.z) > 0.01f)
					return false;
			}
			return true;
		}
	}

	RuntimeGeometryCapture::RuntimeGeometryCapture() = default;

	RuntimeGeometryCapture::~RuntimeGeometryCapture()
	{
		worker.request_stop();
		wake.notify_all();
		if (worker.joinable())
			worker.join();
	}

	void RuntimeGeometryCapture::Reset()
	{
		std::scoped_lock lock(mutex);
		++generation;
		pending.clear();
		captured.clear();
		stableObservations.clear();
		activeCenter = {};
		activeSize = 0;
		lastFrame = UINT32_MAX;
		frameBytes = 0;
	}

	void RuntimeGeometryCapture::Observe(const RuntimeGeometryCandidate& candidate, uint32_t frame,
		const std::shared_ptr<const DX12::Grid>& grid)
	{
		if (!grid || !grid->requested.front().scope || !candidate.vertices || !candidate.indices ||
			candidate.referenceKey.empty() || candidate.shapeName.empty() ||
			candidate.stride < 16 || candidate.stride > 256 ||
			!candidate.vertexCount || !candidate.triangleCount)
			return;
		const uint64_t vertexBytes = uint64_t(candidate.stride) * candidate.vertexCount;
		const uint64_t indexCount = uint64_t(candidate.triangleCount) * 3;
		const uint64_t bytes = vertexBytes + indexCount * sizeof(uint16_t);
		if (!bytes || bytes > kSingleCopyBytes || indexCount > UINT32_MAX)
			return;
		const auto center = grid->requested.front();
		const uint64_t id = Identity(candidate);
		const uint64_t stableKey = StableObjectKey(candidate);
		uint64_t admittedGeneration = 0;
		{
			std::scoped_lock lock(mutex);
			if (activeCenter != center || activeSize != grid->size) {
				++generation;
				activeCenter = center;
				activeSize = grid->size;
				captured.clear();
				stableObservations.clear();
				pending.clear();
				lastFrame = UINT32_MAX;
				frameBytes = 0;
			}
			if (lastFrame != frame) {
				lastFrame = frame;
				frameBytes = 0;
			}
			if (candidate.requiresStability && !captured.contains(id)) {
				// Avoid freezing moving clutter and actor attachments into TLAS.
				// One observation per engine frame, irrespective of pass multiplicity.
				if (stableObservations.size() >= 8192 && !stableObservations.contains(id)) {
					// Drop observations that have not been visible recently instead
					// of permanently locking out later furniture in a large cell.
					std::erase_if(stableObservations, [frame](const auto& entry) {
						return frame - entry.second.first > 120u;
					});
					if (stableObservations.size() >= 8192)
						return;
				}
				auto& seen = stableObservations[id];
				if (seen.first != frame) {
					// Require consecutive visible frames, not three observations
					// scattered across unrelated camera positions.
					if (frame - seen.first > 4u)
						seen.second = 0;
					seen.first = frame;
					seen.second = std::min(seen.second + 1u, 3u);
				}
				if (seen.second < 3)
					return;
			}
			if (bytes > kFrameCopyBytes - frameBytes || pending.size() >= kPacketLimit ||
				!captured.insert(id).second)
				return;
			frameBytes += static_cast<uint32_t>(bytes);
			admittedGeneration = generation;
		}

		try {
			Packet packet;
			packet.identity = id;
			packet.runtimeObjectKey = stableKey;
			packet.sourceCenter = center;
			packet.transform = candidate.transform;
			packet.stride = candidate.stride;
			packet.vertexCount = candidate.vertexCount;
			packet.centerX = candidate.centerX;
			packet.centerY = candidate.centerY;
			packet.vertices.resize(static_cast<size_t>(vertexBytes));
			std::memcpy(packet.vertices.data(), candidate.vertices, packet.vertices.size());
			packet.indices.assign(candidate.indices, candidate.indices + indexCount);
			{
				std::scoped_lock lock(mutex);
				if (generation != admittedGeneration || activeCenter != center || activeSize != grid->size)
					return;
				if (!worker.joinable()) {
					worker = std::jthread([this](std::stop_token stop) {
						try {
							Run(stop);
						} catch (...) {
							logger::error("[DX12] Runtime geometry capture worker failed");
						}
					});
				}
				packet.generation = admittedGeneration;
				pending.push_back(std::move(packet));
			}
			wake.notify_one();
		} catch (...) {
			std::scoped_lock lock(mutex);
			if (generation == admittedGeneration)
				captured.erase(id);
		}
	}

	void RuntimeGeometryCapture::Run(std::stop_token stop)
	{
		while (!stop.stop_requested()) {
			Packet packet;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, stop, [&] { return !pending.empty(); });
				if (stop.stop_requested())
					return;
				packet = std::move(pending.front());
				pending.pop_front();
			}
			auto shape = std::make_shared<DX12::Shape>();
			shape->identity = packet.identity;
			shape->runtimeObjectKey = packet.runtimeObjectKey;
			shape->transform = packet.transform;
			shape->positions.reserve(packet.vertexCount);
			bool usable = true;
			for (uint32_t i = 0; i < packet.vertexCount; ++i) {
				float xyz[3];
				std::memcpy(xyz, packet.vertices.data() + size_t(i) * packet.stride, sizeof(xyz));
				if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
					usable = false;
					break;
				}
				shape->positions.push_back({ xyz[0], xyz[1], xyz[2] });
			}
			if (!usable || !std::ranges::all_of(packet.indices,
							   [&](uint16_t index) { return index < packet.vertexCount; }))
				continue;
			shape->indices.assign(packet.indices.begin(), packet.indices.end());

			const auto releaseIfCurrent = [&]() {
				std::scoped_lock lock(mutex);
				if (packet.generation == generation)
					captured.erase(packet.identity);
			};
			const auto grid = globals::features::dx12Resources.GetGrid();
			if (!grid || !grid->requested.front().scope ||
				grid->requested.front().scope != packet.sourceCenter.scope) {
				releaseIfCurrent();
				continue;
			}
			const auto x = DX12::Coordinate(packet.centerX, grid->size);
			const auto y = DX12::Coordinate(packet.centerY, grid->size);
			if (!x || !y || !std::ranges::contains(grid->requested, DX12::SubcellKey{ grid->requested.front().scope, *x, *y })) {
				releaseIfCurrent();
				continue;
			}
			bool preparedOwns = false;
			for (const auto& tile : grid->resident) {
				if (!tile)
					continue;
				for (const auto& prepared : tile->shapes)
					if (prepared && SameShape(*prepared, *shape)) {
						preparedOwns = true;
						break;
					}
				if (preparedOwns)
					break;
			}
			if (preparedOwns)
				continue;
			{
				std::scoped_lock lock(mutex);
				if (packet.generation != generation)
					continue;
			}
			// Existing immutable grid owns residency and geometry revisions.
			globals::features::dx12Resources.PublishRuntimeShape(
				std::move(shape), packet.centerX, packet.centerY, packet.sourceCenter.scope);
		}
	}
}
