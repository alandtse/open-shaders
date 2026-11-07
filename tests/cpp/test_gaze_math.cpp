// Unit tests for Util::Gaze (src/Utils/GazeMath.h).
//
// Covers OSC datagram parsing, the VRChat pitch/yaw and Unity-vector
// conventions, direction projection through GetProjectionRaw tangents, and
// the crop placement / hysteresis maths used by the gaze-following subrect.

#include "Utils/GazeMath.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

using Catch::Approx;
using Util::Gaze::Direction;
using Util::Gaze::kFallbackProjection;
using Util::Gaze::kMaxMessagesPerDatagram;
using Util::Gaze::OscAddress;
using Util::Gaze::OscMessage;
using Util::Gaze::ProjectionRaw;
using Util::Gaze::Vec2;

namespace
{
	void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t value)
	{
		out.push_back(static_cast<std::uint8_t>(value >> 24));
		out.push_back(static_cast<std::uint8_t>(value >> 16));
		out.push_back(static_cast<std::uint8_t>(value >> 8));
		out.push_back(static_cast<std::uint8_t>(value));
	}

	void AppendFloat(std::vector<std::uint8_t>& out, float value)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		AppendU32(out, bits);
	}

	void AppendPaddedString(std::vector<std::uint8_t>& out, std::string_view text)
	{
		out.insert(out.end(), text.begin(), text.end());
		out.push_back(0);
		while (out.size() % 4 != 0)
			out.push_back(0);
	}

	std::vector<std::uint8_t> MakeMessage(std::string_view address, std::string_view tags)
	{
		std::vector<std::uint8_t> out;
		AppendPaddedString(out, address);
		std::string tagString = ",";
		tagString += std::string(tags);
		AppendPaddedString(out, tagString);
		return out;
	}

	std::vector<std::uint8_t> Bundle(const std::vector<std::vector<std::uint8_t>>& messages)
	{
		std::vector<std::uint8_t> out;
		AppendPaddedString(out, "#bundle");
		for (int i = 0; i < 8; ++i)
			out.push_back(0);
		for (const auto& message : messages) {
			AppendU32(out, static_cast<std::uint32_t>(message.size()));
			out.insert(out.end(), message.begin(), message.end());
		}
		return out;
	}

	std::size_t Parse(const std::vector<std::uint8_t>& packet, std::array<OscMessage, kMaxMessagesPerDatagram>& messages)
	{
		messages = {};
		return Util::Gaze::ParseOscDatagram(packet.data(), packet.size(), messages);
	}
}

TEST_CASE("OSC parses LeftRightPitchYaw floats into per-eye directions", "[gaze][osc]")
{
	auto packet = MakeMessage("/tracking/eye/LeftRightPitchYaw", "ffff");
	AppendFloat(packet, 0.0f);    // left pitch
	AppendFloat(packet, 30.0f);   // left yaw, looks right
	AppendFloat(packet, 0.0f);    // right pitch
	AppendFloat(packet, -30.0f);  // right yaw, looks left

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 1);
	REQUIRE(messages[0].kind == OscAddress::kLeftRightPitchYaw);
	REQUIRE(messages[0].hasGaze);
	CHECK(messages[0].left[0] > 0.0f);
	CHECK(messages[0].left[2] < 0.0f);
	CHECK(messages[0].right[0] < 0.0f);
	CHECK(messages[0].right[2] < 0.0f);
}

TEST_CASE("OSC parses integer arguments as floats", "[gaze][osc]")
{
	auto packet = MakeMessage("/tracking/eye/CenterPitchYaw", "ii");
	AppendU32(packet, 20u);  // positive pitch looks down
	AppendU32(packet, 0u);

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 1);
	REQUIRE(messages[0].kind == OscAddress::kCenterPitchYaw);
	REQUIRE(messages[0].hasGaze);
	CHECK(messages[0].left[1] < 0.0f);
	CHECK(messages[0].left == messages[0].right);
}

TEST_CASE("OSC flips Unity vector z into our forward-is-negative-z convention", "[gaze][osc]")
{
	auto centreVec = MakeMessage("/tracking/eye/CenterVec", "fff");
	AppendFloat(centreVec, 0.0f);
	AppendFloat(centreVec, 0.0f);
	AppendFloat(centreVec, 1.0f);

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(centreVec, messages) == 1);
	REQUIRE(messages[0].hasGaze);
	CHECK(messages[0].left[0] == Approx(0.0f));
	CHECK(messages[0].left[1] == Approx(0.0f));
	CHECK(messages[0].left[2] == Approx(-1.0f));

	auto leftRightVec = MakeMessage("/tracking/eye/LeftRightVec", "ffffff");
	AppendFloat(leftRightVec, 1.0f);
	AppendFloat(leftRightVec, 0.0f);
	AppendFloat(leftRightVec, 0.0f);
	AppendFloat(leftRightVec, 0.0f);
	AppendFloat(leftRightVec, 1.0f);
	AppendFloat(leftRightVec, 0.0f);
	REQUIRE(Parse(leftRightVec, messages) == 1);
	REQUIRE(messages[0].hasGaze);
	CHECK(messages[0].left[0] == Approx(1.0f));
	CHECK(messages[0].right[1] == Approx(1.0f));
}

TEST_CASE("OSC parses CenterVecFull with the same convention as CenterVec", "[gaze][osc]")
{
	auto packet = MakeMessage("/tracking/eye/CenterVecFull", "fff");
	AppendFloat(packet, -1.0f);
	AppendFloat(packet, 0.0f);
	AppendFloat(packet, 0.0f);

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 1);
	REQUIRE(messages[0].kind == OscAddress::kCenterVecFull);
	REQUIRE(messages[0].hasGaze);
	CHECK(messages[0].left[0] == Approx(-1.0f));
	CHECK(messages[0].left == messages[0].right);
}

TEST_CASE("OSC follows a bundle with two messages", "[gaze][osc][bundle]")
{
	auto pitchYaw = MakeMessage("/tracking/eye/CenterPitchYaw", "ff");
	AppendFloat(pitchYaw, 0.0f);
	AppendFloat(pitchYaw, 0.0f);
	auto vectors = MakeMessage("/tracking/eye/CenterVec", "fff");
	AppendFloat(vectors, 0.0f);
	AppendFloat(vectors, 0.0f);
	AppendFloat(vectors, 1.0f);

	const auto packet = Bundle({ pitchYaw, vectors });

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 2);
	CHECK(messages[0].hasGaze);
	CHECK(messages[1].hasGaze);
	CHECK(messages[0].kind == OscAddress::kCenterPitchYaw);
	CHECK(messages[1].kind == OscAddress::kCenterVec);
}

TEST_CASE("OSC returns nothing for garbage and truncated buffers", "[gaze][osc][bounds]")
{
	std::array<OscMessage, kMaxMessagesPerDatagram> messages;

	const std::vector<std::uint8_t> garbage{ 0xff, 0x00, 0x12, 0x34 };
	CHECK(Parse(garbage, messages) == 0);

	const std::vector<std::uint8_t> unterminatedAddress{ '/', 't', 'e', 's' };
	CHECK(Parse(unterminatedAddress, messages) == 0);

	std::vector<std::uint8_t> oversizedElement;
	AppendPaddedString(oversizedElement, "#bundle");
	for (int i = 0; i < 8; ++i)
		oversizedElement.push_back(0);
	AppendU32(oversizedElement, 9999u);
	CHECK(Parse(oversizedElement, messages) == 0);

	auto truncatedArgs = MakeMessage("/tracking/eye/CenterPitchYaw", "ff");
	truncatedArgs.push_back(0x3f);
	truncatedArgs.push_back(0x80);
	REQUIRE(Parse(truncatedArgs, messages) == 1);
	CHECK(messages[0].kind == OscAddress::kCenterPitchYaw);
	CHECK_FALSE(messages[0].hasGaze);
}

TEST_CASE("OSC reports an unknown address without producing gaze", "[gaze][osc]")
{
	auto packet = MakeMessage("/tracking/eye/SomethingElse", "ff");
	AppendFloat(packet, 0.0f);
	AppendFloat(packet, 0.0f);

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 1);
	CHECK(messages[0].kind == OscAddress::kUnknown);
	CHECK_FALSE(messages[0].hasGaze);
}

TEST_CASE("OSC rejects NaN arguments", "[gaze][osc][finite]")
{
	auto packet = MakeMessage("/tracking/eye/CenterPitchYaw", "ff");
	AppendFloat(packet, std::numeric_limits<float>::quiet_NaN());
	AppendFloat(packet, 0.0f);

	std::array<OscMessage, kMaxMessagesPerDatagram> messages;
	REQUIRE(Parse(packet, messages) == 1);
	CHECK_FALSE(messages[0].hasGaze);

	auto infinity = MakeMessage("/tracking/eye/CenterVec", "fff");
	AppendFloat(infinity, std::numeric_limits<float>::infinity());
	AppendFloat(infinity, 0.0f);
	AppendFloat(infinity, 0.0f);
	REQUIRE(Parse(infinity, messages) == 1);
	CHECK_FALSE(messages[0].hasGaze);
}

TEST_CASE("Pitch and yaw follow the VRChat sign convention", "[gaze][direction]")
{
	const Direction down = Util::Gaze::PitchYawToDirection(30.0f, 0.0f);
	CHECK(down[1] < 0.0f);
	CHECK(down[2] < 0.0f);

	const Direction up = Util::Gaze::PitchYawToDirection(-30.0f, 0.0f);
	CHECK(up[1] > 0.0f);

	const Direction right = Util::Gaze::PitchYawToDirection(0.0f, 90.0f);
	CHECK(right[0] == Approx(1.0f).margin(1e-5));
	CHECK(right[2] == Approx(0.0f).margin(1e-5));

	const Direction forward = Util::Gaze::PitchYawToDirection(0.0f, 0.0f);
	CHECK(forward[0] == Approx(0.0f).margin(1e-5));
	CHECK(forward[2] == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("Unity vectors normalise and flip z", "[gaze][direction]")
{
	const Direction up = Util::Gaze::UnityVectorToDirection(0.0f, 2.0f, 0.0f);
	CHECK(up[1] == Approx(1.0f));
	CHECK(up[2] == Approx(0.0f));

	const Direction zero = Util::Gaze::UnityVectorToDirection(0.0f, 0.0f, 0.0f);
	CHECK(zero[2] == Approx(-1.0f));
}

TEST_CASE("ProjectDirection rejects sideways and backwards gaze", "[gaze][projection]")
{
	Vec2 uv;
	CHECK_FALSE(Util::Gaze::ProjectDirection(0, Direction{ 1.0f, 0.0f, 0.0f }, kFallbackProjection, uv));
	CHECK_FALSE(Util::Gaze::ProjectDirection(0, Direction{ 0.0f, 0.0f, 1.0f }, kFallbackProjection, uv));
	CHECK(Util::Gaze::ProjectDirection(0, Direction{ 0.0f, 0.0f, -1.0f }, kFallbackProjection, uv));
	CHECK(uv.x == Approx(0.5f));
	CHECK(uv.y == Approx(0.5f));
}

TEST_CASE("ProjectDirection falls back to the 1.19 tangents for a degenerate projection", "[gaze][projection][fallback]")
{
	const std::array<ProjectionRaw, 2> degenerate{ ProjectionRaw{ 0.0f, 0.0f, 0.0f, 0.0f },
		ProjectionRaw{ 0.0f, 0.0f, 0.0f, 0.0f } };

	Vec2 uv;
	// tan(x / -z) = 1.19 lands on the right edge of the fallback view.
	REQUIRE(Util::Gaze::ProjectDirection(0, Util::Gaze::UnityVectorToDirection(1.19f, 0.0f, 1.0f), degenerate, uv));
	CHECK(uv.x == Approx(1.0f).margin(1e-5));

	// tan = 3 clamps to the same edge instead of overshooting.
	REQUIRE(Util::Gaze::ProjectDirection(1, Util::Gaze::UnityVectorToDirection(3.0f, 0.0f, 1.0f), degenerate, uv));
	CHECK(uv.x == Approx(1.0f));
}

TEST_CASE("ProjectDirection uses the supplied per-eye tangents", "[gaze][projection]")
{
	const std::array<ProjectionRaw, 2> projection{ ProjectionRaw{ -0.5f, 0.5f, -0.5f, 0.5f },
		ProjectionRaw{ -0.5f, 0.5f, -0.5f, 0.5f } };

	Vec2 uv;
	REQUIRE(Util::Gaze::ProjectDirection(0, Direction{ 0.5f, 0.0f, -1.0f }, projection, uv));
	CHECK(uv.x == Approx(1.0f).margin(1e-5));
	CHECK(uv.y == Approx(0.5f).margin(1e-5));

	// Looking down is a negative y direction and maps toward the bottom (uv.y = 1).
	REQUIRE(Util::Gaze::ProjectDirection(0, Util::Gaze::UnityVectorToDirection(0.0f, -0.5f, 1.0f), projection, uv));
	CHECK(uv.y == Approx(1.0f).margin(1e-5));
}

TEST_CASE("NdcToEyeUV converts OpenVR NDC to y-down eye UV", "[gaze][ndc]")
{
	const Vec2 centre = Util::Gaze::NdcToEyeUV({ 0.0f, 0.0f });
	CHECK(centre.x == Approx(0.5f));
	CHECK(centre.y == Approx(0.5f));

	const Vec2 topRight = Util::Gaze::NdcToEyeUV({ 1.0f, 1.0f });
	CHECK(topRight.x == Approx(1.0f));
	CHECK(topRight.y == Approx(0.0f));

	const Vec2 bottomLeft = Util::Gaze::NdcToEyeUV({ -1.0f, -1.0f });
	CHECK(bottomLeft.x == Approx(0.0f));
	CHECK(bottomLeft.y == Approx(1.0f));

	const Vec2 nonFinite = Util::Gaze::NdcToEyeUV({ std::numeric_limits<float>::quiet_NaN(), 0.0f });
	CHECK(nonFinite.x == Approx(0.5f));
	CHECK(nonFinite.y == Approx(0.5f));
}

TEST_CASE("PlaceCrop clamps the crop inside every edge", "[gaze][crop]")
{
	const Vec2 size{ 0.4f, 0.2f };

	const auto topLeft = Util::Gaze::PlaceCrop({ 0.0f, 0.0f }, size);
	CHECK(topLeft.x == Approx(0.0f));
	CHECK(topLeft.y == Approx(0.0f));
	CHECK(topLeft.w == Approx(size.x));
	CHECK(topLeft.h == Approx(size.y));

	const auto bottomRight = Util::Gaze::PlaceCrop({ 1.0f, 1.0f }, size);
	CHECK(bottomRight.x == Approx(1.0f - size.x));
	CHECK(bottomRight.y == Approx(1.0f - size.y));

	const auto rightEdge = Util::Gaze::PlaceCrop({ 0.9f, 0.5f }, Vec2{ 0.8f, 0.5f });
	CHECK(rightEdge.x == Approx(0.2f));
	CHECK(rightEdge.y == Approx(0.25f));

	const auto bottomEdge = Util::Gaze::PlaceCrop({ 0.5f, 0.9f }, Vec2{ 0.6f, 0.4f });
	CHECK(bottomEdge.y == Approx(0.6f));

	const auto oversized = Util::Gaze::PlaceCrop({ 0.5f, 0.5f }, Vec2{ 1.5f, 2.0f });
	CHECK(oversized.w == Approx(1.0f));
	CHECK(oversized.h == Approx(1.0f));
	CHECK(oversized.x == Approx(0.0f));
	CHECK(oversized.y == Approx(0.0f));
}

TEST_CASE("ShouldRecentre triggers just past the threshold", "[gaze][hysteresis]")
{
	const Vec2 size{ 0.5f, 0.5f };
	const Vec2 previous{ 0.5f, 0.5f };
	constexpr float threshold = 0.2f;

	// threshold * size = 0.1; movement up to and including it stays put.
	CHECK_FALSE(Util::Gaze::ShouldRecentre(previous, Vec2{ 0.59f, 0.5f }, size, threshold));
	CHECK(Util::Gaze::ShouldRecentre(previous, Vec2{ 0.61f, 0.5f }, size, threshold));
	CHECK_FALSE(Util::Gaze::ShouldRecentre(previous, Vec2{ 0.5f, 0.41f }, size, threshold));
	CHECK(Util::Gaze::ShouldRecentre(previous, Vec2{ 0.5f, 0.61f }, size, threshold));
}
