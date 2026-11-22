#pragma once

#include "DX12Resources.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Native
{
	/** @brief Borrowed rigid CPU arrays, valid only inside the SCM cull callback. */
	struct RuntimeGeometryCandidate
	{
		std::string referenceKey, shapeName;
		std::array<float, 12> transform{};
		const uint8_t* vertices = nullptr;
		const uint16_t* indices = nullptr;
		uint32_t stride = 0, vertexCount = 0, triangleCount = 0;
		float centerX = 0, centerY = 0;
		bool requiresStability = false;  // Render-pass-only shapes must be stationary for several frames.
		uint64_t objectIdentity = 0;  // Per-session BSGeometry identity independent of NIF shape names.
	};

	/** @brief Bounded CPU-only SCM capture, with a separate conversion/publication worker. */
	class RuntimeGeometryCapture
	{
	public:
		RuntimeGeometryCapture();
		~RuntimeGeometryCapture();
		RuntimeGeometryCapture(const RuntimeGeometryCapture&) = delete;
		RuntimeGeometryCapture& operator=(const RuntimeGeometryCapture&) = delete;

		/** @brief No D3D calls, GPU readbacks or waits. At most 2 MiB copied per frame. */
		void Observe(const RuntimeGeometryCandidate& candidate, uint32_t frame,
			const std::shared_ptr<const DX12::Grid>& grid);
		void Reset();

	private:
		struct Packet
		{
			uint64_t identity = 0, generation = 0;
			DX12::SubcellKey sourceCenter{};
			std::string referenceKey;  // Match a runtime addition with unresolved NIF provenance.
			std::array<float, 12> transform{};
			uint32_t stride = 0, vertexCount = 0;
			float centerX = 0, centerY = 0;
			std::vector<uint8_t> vertices;
			std::vector<uint16_t> indices;
		};
		void Run(std::stop_token stop);

		std::mutex mutex;
		std::condition_variable_any wake;
		std::deque<Packet> pending;
		std::unordered_set<uint64_t> captured;
		std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> stableObservations;
		DX12::SubcellKey activeCenter{};
		uint32_t activeSize = 0, lastFrame = UINT32_MAX, frameBytes = 0;
		uint64_t generation = 0;
		std::jthread worker;
	};
}
