#pragma once

// Pure gaze maths for the VR eye-tracked foveation path: OSC parsing, gaze
// projection and crop placement. No Windows/ImGui/D3D headers, so Catch2 can include it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace Util::Gaze
{
	/** @brief A 2D point or size. For eye UV, x is right and y is down. */
	struct Vec2
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	/** @brief Unit gaze direction in eye space: x right, y up, forward is -z. */
	using Direction = std::array<float, 3>;

	/** @brief A crop region in normalised eye UV (y down). */
	struct Crop
	{
		float x = 0.0f;
		float y = 0.0f;
		float w = 1.0f;
		float h = 1.0f;
	};

	/** @brief OpenVR GetProjectionRaw tangents for one eye, in SteamVR's convention where top is negative. */
	struct ProjectionRaw
	{
		float left = -1.19f;
		float right = 1.19f;
		float top = -1.19f;
		float bottom = 1.19f;
	};

	/** @brief Tangents used when a runtime cannot supply a projection. */
	inline constexpr std::array<ProjectionRaw, 2> kFallbackProjection{};

	/** @brief Largest forward component (z) a direction may have and still be projected; beyond it looks sideways or backwards. */
	inline constexpr float kMaxForwardComponent = -0.05f;

	/** @brief Degrees to radians. */
	inline constexpr float kDegreesToRadians = 0.01745329252f;

	/** @brief Eye-tracking addresses this parser understands. */
	enum class OscAddress : std::uint8_t
	{
		kUnknown = 0,
		kLeftRightPitchYaw,
		kCenterPitchYaw,
		kLeftRightVec,
		kCenterVec,
		kCenterVecFull,
	};

	/** @brief One OSC message. The address view points into the datagram it was parsed from. */
	struct OscMessage
	{
		std::string_view address;
		OscAddress kind = OscAddress::kUnknown;
		bool hasGaze = false;
		Direction left{ 0.0f, 0.0f, -1.0f };
		Direction right{ 0.0f, 0.0f, -1.0f };
	};

	/** @brief Deepest #bundle nesting the parser follows. */
	inline constexpr int kMaxBundleDepth = 4;

	/** @brief Most messages one datagram may contribute. */
	inline constexpr std::size_t kMaxMessagesPerDatagram = 8;

	/**
	 * @brief Converts a VRChat pitch/yaw pair to a unit direction.
	 * @param pitchDegrees VRChat pitch in degrees; positive looks down.
	 * @param yawDegrees VRChat yaw in degrees; positive looks right.
	 */
	inline Direction PitchYawToDirection(float pitchDegrees, float yawDegrees)
	{
		const float down = -pitchDegrees * kDegreesToRadians;
		const float yaw = yawDegrees * kDegreesToRadians;
		const float horizontal = std::cos(down);
		return { std::sin(yaw) * horizontal, std::sin(down), -std::cos(yaw) * horizontal };
	}

	/**
	 * @brief Converts a Unity gaze vector (+x right, +y up, +z forward) to our convention (forward is -z).
	 * @param x Unity x.
	 * @param y Unity y.
	 * @param z Unity z.
	 */
	inline Direction UnityVectorToDirection(float x, float y, float z)
	{
		const float lengthSquared = x * x + y * y + z * z;
		if (!std::isfinite(lengthSquared) || !(lengthSquared > 1e-12f))
			return { 0.0f, 0.0f, -1.0f };
		const float inverseLength = 1.0f / std::sqrt(lengthSquared);
		return { x * inverseLength, y * inverseLength, -z * inverseLength };
	}

	namespace detail
	{
		inline bool ReadU32(const std::uint8_t* data, std::size_t size, std::size_t offset, std::uint32_t& out)
		{
			if (offset + 4 > size)
				return false;
			out = (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16) |
			      (std::uint32_t(data[offset + 2]) << 8) | std::uint32_t(data[offset + 3]);
			return true;
		}

		inline bool ReadFloat(const std::uint8_t* data, std::size_t size, std::size_t offset, float& out)
		{
			std::uint32_t bits = 0;
			if (!ReadU32(data, size, offset, bits))
				return false;
			std::memcpy(&out, &bits, sizeof(out));
			return true;
		}

		/** @brief Reads a NUL-terminated OSC string at @p offset; @p next receives the offset past its 4-byte padding. */
		inline bool ReadPaddedString(const std::uint8_t* data, std::size_t size, std::size_t offset, std::size_t& next, std::string_view& text)
		{
			if (offset >= size)
				return false;
			std::size_t terminator = offset;
			while (terminator < size && data[terminator] != 0)
				++terminator;
			if (terminator >= size)
				return false;
			const std::size_t padded = (terminator - offset + 4) & ~std::size_t(3);
			if (padded > size - offset)
				return false;
			text = std::string_view(reinterpret_cast<const char*>(data + offset), terminator - offset);
			next = offset + padded;
			return true;
		}

		inline bool IsBundle(const std::uint8_t* data, std::size_t size)
		{
			constexpr char kMarker[] = "#bundle";
			if (size < 8)
				return false;
			for (std::size_t i = 0; i < 7; ++i) {
				if (data[i] != static_cast<std::uint8_t>(kMarker[i]))
					return false;
			}
			return data[7] == 0;
		}

		inline bool IsFiniteUnit(const Direction& direction)
		{
			for (const float component : direction) {
				if (!std::isfinite(component))
					return false;
			}
			const float lengthSquared = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
			return lengthSquared > 0.25f && lengthSquared < 2.25f;
		}

		inline bool IsValidProjection(const ProjectionRaw& projection)
		{
			return std::isfinite(projection.left) && std::isfinite(projection.right) &&
			       std::isfinite(projection.top) && std::isfinite(projection.bottom) &&
			       projection.right - projection.left > 1e-4f &&
			       projection.bottom - projection.top > 1e-4f;
		}

		inline OscAddress ClassifyAddress(std::string_view address)
		{
			if (address == "/tracking/eye/LeftRightPitchYaw")
				return OscAddress::kLeftRightPitchYaw;
			if (address == "/tracking/eye/CenterPitchYaw")
				return OscAddress::kCenterPitchYaw;
			if (address == "/tracking/eye/LeftRightVec")
				return OscAddress::kLeftRightVec;
			if (address == "/tracking/eye/CenterVec")
				return OscAddress::kCenterVec;
			if (address == "/tracking/eye/CenterVecFull")
				return OscAddress::kCenterVecFull;
			return OscAddress::kUnknown;
		}

		struct Args
		{
			std::array<float, 8> values{};
			std::size_t count = 0;
			bool ok = true;
		};

		inline void ParseMessage(const std::uint8_t* data, std::size_t size,
			std::array<OscMessage, kMaxMessagesPerDatagram>& out, std::size_t& count)
		{
			if (count >= out.size() || size < 4 || data[0] != '/')
				return;

			std::size_t position = 0;
			std::string_view address;
			if (!ReadPaddedString(data, size, 0, position, address))
				return;

			std::string_view tags;
			if (position < size && data[position] == ',') {
				if (!ReadPaddedString(data, size, position, position, tags))
					return;
				tags.remove_prefix(1);
			}

			Args args;
			for (const char tag : tags) {
				if (!args.ok)
					break;
				switch (tag) {
				case 'f':
					{
						float value = 0.0f;
						if (!ReadFloat(data, size, position, value) || !std::isfinite(value) || args.count == args.values.size()) {
							args.ok = false;
						} else {
							args.values[args.count++] = value;
							position += 4;
						}
						break;
					}
				case 'i':
					{
						std::uint32_t bits = 0;
						if (!ReadU32(data, size, position, bits) || args.count == args.values.size()) {
							args.ok = false;
						} else {
							args.values[args.count++] = static_cast<float>(static_cast<std::int32_t>(bits));
							position += 4;
						}
						break;
					}
				case 'h':
				case 't':
				case 'd':
					if (size - position < 8)
						args.ok = false;
					else
						position += 8;
					break;
				case 's':
					{
						std::string_view ignored;
						if (!ReadPaddedString(data, size, position, position, ignored))
							args.ok = false;
						break;
					}
				case 'b':
					{
						std::uint32_t blobSize = 0;
						if (!ReadU32(data, size, position, blobSize)) {
							args.ok = false;
							break;
						}
						position += 4;
						const std::size_t padded = (static_cast<std::size_t>(blobSize) + 3) & ~std::size_t(3);
						if (padded > size - position)
							args.ok = false;
						else
							position += padded;
						break;
					}
				case 'c':
				case 'r':
				case 'm':
					if (size - position < 4)
						args.ok = false;
					else
						position += 4;
					break;
				case 'T':
				case 'F':
				case 'N':
				case 'I':
					break;
				default:
					args.ok = false;
					break;
				}
			}

			OscMessage message;
			message.address = address;
			message.kind = ClassifyAddress(address);
			if (message.kind != OscAddress::kUnknown && args.ok) {
				Direction left{ 0.0f, 0.0f, -1.0f };
				Direction right{ 0.0f, 0.0f, -1.0f };
				bool enough = true;
				switch (message.kind) {
				case OscAddress::kLeftRightPitchYaw:
					enough = args.count >= 4;
					if (enough) {
						left = PitchYawToDirection(args.values[0], args.values[1]);
						right = PitchYawToDirection(args.values[2], args.values[3]);
					}
					break;
				case OscAddress::kCenterPitchYaw:
					enough = args.count >= 2;
					if (enough) {
						left = PitchYawToDirection(args.values[0], args.values[1]);
						right = left;
					}
					break;
				case OscAddress::kLeftRightVec:
					enough = args.count >= 6;
					if (enough) {
						left = UnityVectorToDirection(args.values[0], args.values[1], args.values[2]);
						right = UnityVectorToDirection(args.values[3], args.values[4], args.values[5]);
					}
					break;
				case OscAddress::kCenterVec:
				case OscAddress::kCenterVecFull:
					enough = args.count >= 3;
					if (enough) {
						left = UnityVectorToDirection(args.values[0], args.values[1], args.values[2]);
						right = left;
					}
					break;
				default:
					enough = false;
					break;
				}
				if (enough && IsFiniteUnit(left) && IsFiniteUnit(right)) {
					message.hasGaze = true;
					message.left = left;
					message.right = right;
				}
			}
			out[count++] = message;
		}

		inline void ParsePacket(const std::uint8_t* data, std::size_t size, int depth,
			std::array<OscMessage, kMaxMessagesPerDatagram>& out, std::size_t& count)
		{
			if (depth > kMaxBundleDepth)
				return;
			if (IsBundle(data, size)) {
				std::size_t position = 16;
				while (position + 4 <= size && count < out.size()) {
					std::uint32_t elementSize = 0;
					if (!ReadU32(data, size, position, elementSize))
						break;
					position += 4;
					if (elementSize == 0 || elementSize > size - position)
						break;
					ParsePacket(data + position, elementSize, depth + 1, out, count);
					position += elementSize;
				}
				return;
			}
			ParseMessage(data, size, out, count);
		}
	}

	/**
	 * @brief Projects an eye-space gaze direction to a normalised position inside that eye's view.
	 * @param eye 0 for the left eye, 1 for the right.
	 * @param direction Unit gaze direction.
	 * @param projectionRaw Per-eye GetProjectionRaw tangents; a degenerate eye falls back to kFallbackProjection.
	 * @param out Receives the eye UV (y down) when this returns true.
	 * @return False when the direction points sideways or backwards.
	 */
	inline bool ProjectDirection(int eye, const Direction& direction,
		const std::array<ProjectionRaw, 2>& projectionRaw, Vec2& out)
	{
		if (!detail::IsFiniteUnit(direction) || direction[2] > kMaxForwardComponent)
			return false;

		const auto index = eye == 1 ? std::size_t(1) : std::size_t(0);
		const ProjectionRaw projection = detail::IsValidProjection(projectionRaw[index]) ?
		                                     projectionRaw[index] :
		                                     kFallbackProjection[index];
		const float depth = -direction[2];
		const float tanX = direction[0] / depth;
		const float tanYDown = -direction[1] / depth;
		out.x = std::clamp((tanX - projection.left) / (projection.right - projection.left), 0.0f, 1.0f);
		out.y = std::clamp((tanYDown - projection.top) / (projection.bottom - projection.top), 0.0f, 1.0f);
		return true;
	}

	/**
	 * @brief Converts OpenVR NDC (x right, y up, [-1,1]) to normalised eye UV (y down).
	 * @param ndc The per-eye NDC point; a non-finite point maps to the eye centre.
	 */
	inline Vec2 NdcToEyeUV(const Vec2& ndc)
	{
		if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
			return { 0.5f, 0.5f };
		return { std::clamp(ndc.x * 0.5f + 0.5f, 0.0f, 1.0f), std::clamp(0.5f - ndc.y * 0.5f, 0.0f, 1.0f) };
	}

	/**
	 * @brief Places a crop of @p size centred on @p centre, clamped fully inside [0,1].
	 * @param centre Desired crop centre in eye UV.
	 * @param size Crop extent (w, h) in eye UV; values past 1 clamp to a full eye.
	 */
	inline Crop PlaceCrop(const Vec2& centre, const Vec2& size)
	{
		Crop crop;
		crop.w = std::isfinite(size.x) ? std::clamp(size.x, 0.0f, 1.0f) : 1.0f;
		crop.h = std::isfinite(size.y) ? std::clamp(size.y, 0.0f, 1.0f) : 1.0f;
		const float centreX = std::isfinite(centre.x) ? centre.x : 0.5f;
		const float centreY = std::isfinite(centre.y) ? centre.y : 0.5f;
		crop.x = std::clamp(centreX - crop.w * 0.5f, 0.0f, 1.0f - crop.w);
		crop.y = std::clamp(centreY - crop.h * 0.5f, 0.0f, 1.0f - crop.h);
		return crop;
	}

	/**
	 * @brief True when gaze moved more than @p threshold crop widths (x) or heights (y) from the held centre.
	 * @param previousCentre The centre the crop currently follows.
	 * @param gaze The latest gaze centre.
	 * @param size Crop extent (w, h) in eye UV.
	 * @param threshold Movement that triggers a recentre, as a fraction of the crop size.
	 */
	inline bool ShouldRecentre(const Vec2& previousCentre, const Vec2& gaze, const Vec2& size, float threshold)
	{
		constexpr float kMinExtent = 0.01f;
		const float width = std::max(std::isfinite(size.x) ? size.x : 0.0f, kMinExtent);
		const float height = std::max(std::isfinite(size.y) ? size.y : 0.0f, kMinExtent);
		const float limit = std::isfinite(threshold) ? std::max(threshold, 0.0f) : 0.0f;
		return std::fabs(gaze.x - previousCentre.x) / width > limit ||
		       std::fabs(gaze.y - previousCentre.y) / height > limit;
	}

	/**
	 * @brief Parses one OSC datagram, following #bundle nesting.
	 * @param data Datagram bytes.
	 * @param size Byte count of @p data.
	 * @param out Receives parsed messages; an unrecognised address is reported with kind kUnknown and no gaze.
	 * @return Number of messages written to @p out.
	 */
	inline std::size_t ParseOscDatagram(const std::uint8_t* data, std::size_t size,
		std::array<OscMessage, kMaxMessagesPerDatagram>& out)
	{
		std::size_t count = 0;
		if (data && size > 0)
			detail::ParsePacket(data, size, 0, out, count);
		return count;
	}
}
