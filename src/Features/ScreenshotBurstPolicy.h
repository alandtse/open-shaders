#pragma once

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ScreenshotBurst
{
	inline constexpr uint32_t MaximumFrames = 240;
	inline constexpr uint32_t MaximumRegions = 8;
	inline constexpr uint32_t MaximumDimension = 16384;
	inline constexpr uint64_t MaximumFrameBytes = 128ull * 1024 * 1024;
	inline constexpr uint64_t MaximumBytes = 512ull * 1024 * 1024;

	struct Region
	{
		uint32_t x, y, width, height;
	};

	struct Copy
	{
		uint32_t x, y, width, height, destinationY;
	};

	/** Bounded native-pixel atlas; regions use oriented, submission-relative pixels. */
	struct Plan
	{
		std::vector<Region> regions;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t maximumBytes = MaximumBytes;
		uint64_t payloadBytes = 0;
		explicit operator bool() const { return !regions.empty(); }

		/** Resolves copies so a final whole-plane flip preserves region order. */
		std::vector<Copy> Copies(uint32_t eyeWidth, uint32_t eyeHeight, bool flipX, bool flipY) const
		{
			std::vector<Copy> copies;
			uint32_t top = 0;
			for (const auto& r : regions) {
				if (r.width > eyeWidth || r.height > eyeHeight || r.x > eyeWidth - r.width || r.y > eyeHeight - r.height)
					throw std::invalid_argument("burst region exceeds the submitted native eye");
				copies.push_back({ flipX ? eyeWidth - r.x - r.width : r.x,
					flipY ? eyeHeight - r.y - r.height : r.y, r.width, r.height,
					flipY ? height - top - r.height : top });
				top += r.height;
			}
			return copies;
		}
	};

	/** Rejects unknown fields and non-integer bounds before any allocation. */
	inline uint64_t Read(const nlohmann::json& object, const char* key, uint64_t minimum, uint64_t maximum)
	{
		const auto& value = object.at(key);
		if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
			throw std::invalid_argument(std::string("burst.") + key + " must be an unsigned integer");
		const auto number = value.get<uint64_t>();
		if (number < minimum || number > maximum)
			throw std::invalid_argument(std::string("burst.") + key + " is outside supported limits");
		return number;
	}

	/** Validates an entire fixed-frame burst and its raw staging payload budget. */
	inline Plan Parse(const nlohmann::json& value, uint32_t frames, uint32_t eyes)
	{
		if (!value.is_object() || value.size() != (value.contains("maximumBytes") ? 2u : 1u) || !value.contains("regions"))
			throw std::invalid_argument("burst supports only regions and maximumBytes");
		if (!frames || frames > MaximumFrames || !eyes || eyes > 2)
			throw std::invalid_argument("burst exceeds the frame or eye limit");
		Plan plan;
		if (value.contains("maximumBytes"))
			plan.maximumBytes = Read(value, "maximumBytes", 1, MaximumBytes);
		const auto& regions = value.at("regions");
		if (!regions.is_array() || regions.empty() || regions.size() > MaximumRegions)
			throw std::invalid_argument("burst requires one to eight regions");
		for (const auto& region : regions) {
			if (!region.is_object() || region.size() != 4)
				throw std::invalid_argument("burst regions require x, y, width and height");
			Region r{ static_cast<uint32_t>(Read(region, "x", 0, MaximumDimension - 1)),
				static_cast<uint32_t>(Read(region, "y", 0, MaximumDimension - 1)),
				static_cast<uint32_t>(Read(region, "width", 1, MaximumDimension)),
				static_cast<uint32_t>(Read(region, "height", 1, MaximumDimension)) };
			if ((plan.width && r.width != plan.width) || r.height > MaximumDimension - plan.height ||
				r.x > MaximumDimension - r.width || r.y > MaximumDimension - r.height)
				throw std::invalid_argument("burst regions must have equal widths and fit native texture limits");
			plan.width = r.width;
			plan.height += r.height;
			plan.regions.push_back(r);
		}
		plan.payloadBytes = uint64_t(plan.width) * plan.height * 4 * eyes * frames;
		if (plan.payloadBytes / frames > MaximumFrameBytes || plan.payloadBytes > plan.maximumBytes)
			throw std::invalid_argument("burst exceeds maximumBytes; reduce regions or frameCount");
		return plan;
	}

	/** Tracks observed frame and compositor continuity independently of file writing. */
	struct Continuity
	{
		uint32_t acquired = 0;
		uint64_t firstFrame = 0;
		uint64_t lastFrame = 0;
		std::optional<uint64_t> lastCycle;
		std::string failure;

		void Fail(std::string_view reason)
		{
			if (failure.empty())
				failure = reason;
		}

		bool Observe(uint64_t frame, std::optional<uint64_t> cycle)
		{
			if (acquired && (lastFrame == std::numeric_limits<uint64_t>::max() || frame != lastFrame + 1 ||
								cycle.has_value() != lastCycle.has_value() ||
								(cycle && (*lastCycle == std::numeric_limits<uint64_t>::max() || *cycle != *lastCycle + 1))))
				Fail("nonconsecutive_acquisition");
			if (!acquired)
				firstFrame = frame;
			++acquired;
			lastFrame = frame;
			lastCycle = cycle;
			return failure.empty();
		}

		nlohmann::json Receipt(uint32_t requested, bool allWritten) const
		{
			return { { "complete", failure.empty() && acquired == requested && allWritten },
				{ "acquired", acquired }, { "requested", requested },
				{ "firstEngineFrame", acquired ? nlohmann::json(firstFrame) : nlohmann::json(nullptr) },
				{ "lastEngineFrame", acquired ? nlohmann::json(lastFrame) : nlohmann::json(nullptr) },
				{ "failure", failure } };
		}
	};
}
