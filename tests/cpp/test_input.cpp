// Unit tests for the input-combo abstraction (src/Utils/Input.h).
//
// Header-only; depends on magic_enum + nlohmann_json. <Windows.h> is needed
// because Input.h's MatchesKeyboardCombo references VK_* / GetAsyncKeyState
// (that function itself isn't tested — it needs live key state).

#include <Windows.h>

#include "Utils/Input.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

TEST_CASE("InputCombo packs device and key, with 16-bit key truncation", "[input]")
{
	const auto kb = InputCombo::Keyboard(0x41);  // 'A'
	REQUIRE(kb.GetDevice() == InputDeviceType::Keyboard);
	REQUIRE(kb.GetKey() == 0x41u);

	// Key is masked to the low 16 bits.
	const InputCombo big(InputDeviceType::Mouse, 0x12345);
	REQUIRE(big.GetDevice() == InputDeviceType::Mouse);
	REQUIRE(big.GetKey() == 0x2345u);
}

TEST_CASE("Device factories select the right device", "[input]")
{
	REQUIRE(InputCombo::Primary(1).GetDevice() == InputDeviceType::Primary);
	REQUIRE(InputCombo::Secondary(1).GetDevice() == InputDeviceType::Secondary);
	REQUIRE(InputCombo::Both(1).GetDevice() == InputDeviceType::Both);
	REQUIRE(InputCombo::Keyboard(1).GetDevice() == InputDeviceType::Keyboard);
	REQUIRE(InputCombo::Mouse(1).GetDevice() == InputDeviceType::Mouse);
	REQUIRE(InputCombo::Gamepad(1).GetDevice() == InputDeviceType::Gamepad);
}

TEST_CASE("IsValid requires an in-range device and a non-zero key", "[input]")
{
	REQUIRE(InputCombo::Keyboard(0x41).IsValid());
	REQUIRE_FALSE(InputCombo{}.IsValid());                                        // default: key 0
	REQUIRE_FALSE(InputCombo::Keyboard(0).IsValid());                             // explicit zero key
	REQUIRE_FALSE(InputCombo(static_cast<InputDeviceType>(99), 0x41).IsValid());  // bad device
}

TEST_CASE("Equality and ordering compare the packed device+key", "[input]")
{
	REQUIRE(InputCombo::Keyboard(0x41) == InputCombo::Keyboard(0x41));
	REQUIRE_FALSE(InputCombo::Keyboard(0x41) == InputCombo::Mouse(0x41));
	// Mouse (device 4) sorts after Keyboard (device 3) since device is the high bits.
	REQUIRE(InputCombo::Keyboard(0xFFFF) < InputCombo::Mouse(0x0001));
}

TEST_CASE("ToString and IsValidDevice cover the enum range", "[input]")
{
	REQUIRE(std::string(ToString(InputDeviceType::Keyboard)) == "Keyboard");
	REQUIRE(std::string(ToString(InputDeviceType::Primary)) == "Primary");
	REQUIRE(std::string(ToString(static_cast<InputDeviceType>(99))) == "Unknown");

	REQUIRE(IsValidDevice(InputDeviceType::Primary));
	REQUIRE(IsValidDevice(InputDeviceType::Gamepad));
	REQUIRE_FALSE(IsValidDevice(static_cast<InputDeviceType>(99)));
}

TEST_CASE("InputCombo serializes as its packed integer and round-trips", "[input]")
{
	const auto kb = InputCombo::Keyboard(0x41);
	const json j = kb;
	REQUIRE(j.get<uint32_t>() == 0x30041u);

	const auto back = j.get<InputCombo>();
	REQUIRE(back == kb);
}

TEST_CASE("A vector of InputCombo round-trips through JSON", "[input]")
{
	const std::vector<InputCombo> combo{
		InputCombo::Keyboard(VK_CONTROL),
		InputCombo::Keyboard(0x53),  // 'S'
	};
	const json j = combo;
	const auto back = j.get<std::vector<InputCombo>>();
	REQUIRE(back == combo);
}
