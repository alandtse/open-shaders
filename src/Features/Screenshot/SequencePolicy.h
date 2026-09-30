#pragma once

#include "Features/ScreenshotBurstPolicy.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace OS::Capture
{
	inline constexpr uint32_t MaximumFrames = 10'000;
	inline constexpr uint32_t MaximumSpanFrames = 216'000;
	inline constexpr uint64_t MaximumFrameBytes = 128ull * 1024 * 1024;
	inline constexpr uint64_t MaximumQueuedBytes = 256ull * 1024 * 1024;
	inline constexpr uint32_t MaximumQueuedFrames = 4;

	/** Rejects unknown fields instead of silently changing requested capture semantics. */
	inline void CheckFields(const nlohmann::json& object, std::initializer_list<std::string_view> names)
	{
		if (!object.is_object())
			throw std::invalid_argument("expected an object");
		for (const auto& [key, value] : object.items()) {
			if (std::find(names.begin(), names.end(), key) == names.end())
				throw std::invalid_argument("unsupported field: " + key);
		}
	}

	/** Reads an integer without truncating floats, negatives or overflowing values. */
	inline uint32_t ReadUnsigned(const nlohmann::json& object, const char* key, uint32_t fallback, uint32_t minimum, uint32_t maximum)
	{
		if (!object.contains(key))
			return fallback;
		const auto& value = object.at(key);
		if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
			throw std::invalid_argument(std::string(key) + " must be an unsigned integer");
		const auto number = value.get<uint64_t>();
		if (number < minimum || number > maximum)
			throw std::invalid_argument(std::string(key) + " is outside supported limits");
		return static_cast<uint32_t>(number);
	}

	struct SequencePlan
	{
		uint32_t frameCount = 30;
		uint32_t intervalFrames = 1;
		uint32_t startDelayFrames = 0;
		uint32_t maximumConsecutiveSkips = 30;
		bool stopOnFailure = true;
		bool png = false;
		ScreenshotBurst::Plan burst;
		std::string source;
		std::string directory;
	};

	/** Validates the supported CSX sequence-request subset before allocating resources. */
	inline SequencePlan ParsePlan(const nlohmann::json& sequence, bool vrRuntime)
	{
		using json = nlohmann::json;
		CheckFields(sequence, { "frameCount", "schedule", "backpressure", "failurePolicy", "capture", "packaging", "useSettings", "burst" });
		if (sequence.value("useSettings", false))
			throw std::invalid_argument("sequence.useSettings is not supported; supply explicit capture settings");
		SequencePlan plan;
		plan.frameCount = ReadUnsigned(sequence, "frameCount", plan.frameCount, 1, MaximumFrames);
		const auto schedule = sequence.value("schedule", json::object());
		CheckFields(schedule, { "basis", "intervalFrames", "startDelayFrames", "pausePolicy" });
		if (schedule.value("basis", "game_frames") != "game_frames" || schedule.value("pausePolicy", "hold") != "hold")
			throw std::invalid_argument("only game_frames scheduling with pausePolicy=hold is supported");
		plan.intervalFrames = ReadUnsigned(schedule, "intervalFrames", 1, 1, MaximumSpanFrames);
		plan.startDelayFrames = ReadUnsigned(schedule, "startDelayFrames", 0, 0, MaximumSpanFrames);
		const auto span = uint64_t(plan.startDelayFrames) + uint64_t(plan.intervalFrames) * (plan.frameCount - 1u);
		if (span > MaximumSpanFrames)
			throw std::invalid_argument("sequence exceeds the maximum scheduled frame span");
		const auto backpressure = sequence.value("backpressure", json::object());
		CheckFields(backpressure, { "policy", "maximumConsecutiveSkips" });
		if (backpressure.value("policy", "skip") != "skip")
			throw std::invalid_argument("only explicit skip backpressure is supported");
		plan.maximumConsecutiveSkips = ReadUnsigned(backpressure, "maximumConsecutiveSkips", 30, 1, MaximumFrames);
		const auto failure = sequence.value("failurePolicy", "stop");
		if (failure != "stop" && failure != "continue")
			throw std::invalid_argument("failurePolicy must be stop or continue");
		plan.stopOnFailure = failure == "stop";
		const auto& capture = sequence.at("capture");
		CheckFields(capture, { "source", "outputs", "destination" });
		const auto& source = capture.at("source");
		CheckFields(source, { "kind", "fallback" });
		plan.source = source.at("kind").get<std::string>();
		if (source.value("fallback", "reject") != "reject" || plan.source != (vrRuntime ? "hmd_submission" : "desktop_mirror"))
			throw std::invalid_argument("capture source must match the runtime; fallback is not supported");
		const auto& outputs = capture.at("outputs");
		if (!outputs.is_array() || outputs.size() != 1)
			throw std::invalid_argument("exactly one native output is required");
		CheckFields(outputs[0], { "view", "encoding" });
		if (outputs[0].at("view") != (vrRuntime ? "side_by_side" : "source_native"))
			throw std::invalid_argument("VR requires side_by_side; flat requires source_native");
		const auto& encoding = outputs[0].at("encoding");
		CheckFields(encoding, { "format", "colourContract" });
		const auto format = encoding.value("format", "bmp");
		if ((format != "bmp" && format != "png") || encoding.value("colourContract", "sdr_srgb") != "sdr_srgb")
			throw std::invalid_argument("only lossless BMP/PNG with sdr_srgb is supported");
		plan.png = format == "png";
		const auto& destination = capture.at("destination");
		CheckFields(destination, { "policy", "directory", "overwrite" });
		if (destination.value("policy", "absolute") != "absolute" || destination.value("overwrite", "never") != "never")
			throw std::invalid_argument("destination must be absolute with overwrite=never");
		plan.directory = destination.at("directory").get<std::string>();
		if (plan.directory.empty() || plan.directory.find('\0') != std::string::npos)
			throw std::invalid_argument("destination.directory must be a nonempty path without NUL characters");
		const auto packaging = sequence.value("packaging", json::object());
		CheckFields(packaging, { "frameManifest", "previewVideo" });
		if (!packaging.value("frameManifest", true))
			throw std::invalid_argument("the frame manifest is required");
		const auto preview = packaging.value("previewVideo", json::object());
		CheckFields(preview, { "requested", "required" });
		if (preview.value("requested", false) || preview.value("required", false))
			throw std::invalid_argument("preview video encoding is not supported");
		if (sequence.contains("burst")) {
			(void)ScreenshotBurst::Read(sequence, "frameCount", 1, ScreenshotBurst::MaximumFrames);
			plan.burst = ScreenshotBurst::Parse(sequence.at("burst"), plan.frameCount, vrRuntime ? 2u : 1u);
			if (plan.intervalFrames != 1)
				throw std::invalid_argument("burst requires intervalFrames=1");
		}
		return plan;
	}

	/** Builds the native SDR descriptor shared by menu sequences and reference captures. */
	inline nlohmann::json NativeCaptureDescriptor(bool vr, const std::filesystem::path& directory, bool png)
	{
		using json = nlohmann::json;
		return {
			{ "source", { { "kind", vr ? "hmd_submission" : "desktop_mirror" }, { "fallback", "reject" } } },
			{ "outputs", json::array({ { { "view", vr ? "side_by_side" : "source_native" },
							 { "encoding", { { "format", png ? "png" : "bmp" }, { "colourContract", "sdr_srgb" } } } } }) },
			{ "destination", { { "directory", Util::PathToUtf8(directory) }, { "overwrite", "never" } } }
		};
	}

	/** Builds a native reference request for DevBench's existing capture-provider contract. */
	inline nlohmann::json ReferenceCommand(const nlohmann::json& request, bool vr)
	{
		using json = nlohmann::json;
		const auto pathText = request.at("outputPath").get<std::string>();
		const auto requestId = request.at("requestId").get<std::string>();
		auto path = std::filesystem::u8path(pathText).lexically_normal();
		if (pathText.find('\0') != std::string::npos || !path.is_absolute() || !Util::IEquals(path.extension().string(), ".png") ||
			requestId.empty() || requestId.size() > 128)
			throw std::invalid_argument("reference capture requires an absolute PNG path and bounded requestId");
		path = std::filesystem::weakly_canonical(path.parent_path()) / path.filename();
		return { { "contractMajor", 1 }, { "clientId", "devbench.capture" }, { "commandId", requestId }, { "action", "sequence_start" },
			{ "reference", { { "requestId", requestId }, { "outputPath", Util::PathToUtf8(path) } } },
			{ "sequence", { { "frameCount", 1 }, { "useSettings", false },
							  { "capture", NativeCaptureDescriptor(vr, path.parent_path(), true) } } } };
	}

	/** Fixed frame cadence: late observations must report missed slots, never compress them. */
	struct SequenceSchedule
	{
		uint64_t nextFrame = 0;
		uint32_t nextOrdinal = 1;
		SequencePlan plan;

		/** Reports whether every requested slot has been scheduled. */
		bool Finished() const { return nextOrdinal > plan.frameCount; }
		/** Includes overdue slots so callers must account for cadence gaps. */
		bool Due(uint64_t frame) const { return !Finished() && frame >= nextFrame; }
		/** Consumes one slot without shifting the cadence to the observation time. */
		void Advance()
		{
			++nextOrdinal;
			nextFrame += plan.intervalFrames;
		}
	};

	struct PixelBounds
	{
		uint32_t x, y, width, height;
		bool flipX, flipY;
	};

	/** Checks pair identity before accepting another plane into a stereo frame. */
	inline bool CanAcceptEye(uint32_t eye, uint8_t eyeMask, uint64_t frame, uint64_t expectedFrame)
	{
		return eye < 2 && frame == expectedFrame && (eyeMask & (1u << eye)) == 0;
	}

	/** Applies normalized submission bounds without resizing the selected eye. */
	inline std::optional<PixelBounds> ResolveBounds(uint32_t width, uint32_t height, const std::array<float, 4>& bounds)
	{
		constexpr uint32_t maximumTextureDimension = 16384;
		if (!width || !height || width > maximumTextureDimension || height > maximumTextureDimension)
			return std::nullopt;
		for (float value : bounds) {
			if (!std::isfinite(value) || value < 0 || value > 1)
				return std::nullopt;
		}
		const auto x = static_cast<uint32_t>(std::lround(std::min(bounds[0], bounds[2]) * width));
		const auto y = static_cast<uint32_t>(std::lround(std::min(bounds[1], bounds[3]) * height));
		const auto right = static_cast<uint32_t>(std::lround(std::max(bounds[0], bounds[2]) * width));
		const auto bottom = static_cast<uint32_t>(std::lround(std::max(bounds[1], bounds[3]) * height));
		if (right <= x || bottom <= y || right > width || bottom > height)
			return std::nullopt;
		return PixelBounds{ x, y, right - x, bottom - y, bounds[0] > bounds[2], bounds[1] > bounds[3] };
	}

	/** Copies a mapped native eye into opaque RGBA without changing its resolution. */
	inline std::vector<uint8_t> CopyPixels(std::span<const uint8_t> source, uint32_t rowPitch, const PixelBounds& bounds, bool bgra)
	{
		const uint64_t rowBytes = uint64_t(bounds.width) * 4;
		const uint64_t bytes = rowBytes * bounds.height;
		if (!bounds.width || !bounds.height || bytes > MaximumFrameBytes || rowPitch < rowBytes ||
			source.size() < uint64_t(bounds.height - 1) * rowPitch + rowBytes)
			throw std::invalid_argument("invalid mapped image layout");
		std::vector<uint8_t> pixels(bytes);
		for (uint32_t y = 0; y < bounds.height; ++y) {
			const auto* row = source.data() + uint64_t(bounds.flipY ? bounds.height - 1 - y : y) * rowPitch;
			for (uint32_t x = 0; x < bounds.width; ++x) {
				const auto* pixel = row + uint64_t(bounds.flipX ? bounds.width - 1 - x : x) * 4;
				auto* target = pixels.data() + (uint64_t(y) * bounds.width + x) * 4;
				target[0] = pixel[bgra ? 2 : 0];
				target[1] = pixel[1];
				target[2] = pixel[bgra ? 0 : 2];
				target[3] = 255;
			}
		}
		return pixels;
	}
}
