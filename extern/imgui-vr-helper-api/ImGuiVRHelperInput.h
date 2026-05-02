// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// InputCombo / InputDeviceType — public types used by clients to describe
// keybinds (controller buttons, keyboard keys, mouse buttons, gamepad
// buttons) and pass them into IImGuiVRHelperInterface::RegisterCombo. Also
// supports JSON serialization via nlohmann_json so clients can persist
// user-bound combos in their own settings files.
//
// This file is derived in shape from Skyrim Community Shaders'
// src/Utils/Input.h. The packed device-and-key encoding lets a vector of
// combos be JSON-serialized either as a single integer (single bind, for
// backward compatibility with simpler save formats) or as an array of
// integers (multi-key combo).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ImGuiVRHelperPluginAPI
{
	/// Identifies which physical input device a key/button belongs to.
	enum class InputDeviceType : uint32_t
	{
		Primary = 0,    ///< VR: dominant-hand controller
		Secondary = 1,  ///< VR: non-dominant-hand controller
		Both = 2,       ///< VR: both controllers simultaneously
		Keyboard = 3,
		Mouse = 4,
		Gamepad = 5,
	};

	constexpr const char* ToString(InputDeviceType d) noexcept
	{
		switch (d) {
		case InputDeviceType::Primary:
			return "Primary";
		case InputDeviceType::Secondary:
			return "Secondary";
		case InputDeviceType::Both:
			return "Both";
		case InputDeviceType::Keyboard:
			return "Keyboard";
		case InputDeviceType::Mouse:
			return "Mouse";
		case InputDeviceType::Gamepad:
			return "Gamepad";
		}
		return "Unknown";
	}

	constexpr bool IsValidDevice(InputDeviceType d) noexcept
	{
		return d >= InputDeviceType::Primary && d <= InputDeviceType::Gamepad;
	}

	/// One device + one key/button, packed into a single uint32_t with the
	/// device in the upper 16 bits. Multiple `InputCombo`s in a vector form
	/// a chord (e.g. Shift+A or Grip+Trigger). The 16-bit key field is
	/// large enough for VK_* keyboard codes and OpenVR button identifiers.
	struct InputCombo
	{
	private:
		uint32_t deviceAndKey{ 0 };

		friend struct ComboList;

	public:
		InputCombo() = default;
		InputCombo(InputDeviceType d, uint32_t key) noexcept :
			deviceAndKey((static_cast<uint32_t>(d) << 16) | (key & 0xFFFFu))
		{}

		static InputCombo Primary(uint32_t key) { return InputCombo(InputDeviceType::Primary, key); }
		static InputCombo Secondary(uint32_t key) { return InputCombo(InputDeviceType::Secondary, key); }
		static InputCombo Both(uint32_t key) { return InputCombo(InputDeviceType::Both, key); }
		static InputCombo Keyboard(uint32_t key) { return InputCombo(InputDeviceType::Keyboard, key); }
		static InputCombo Mouse(uint32_t key) { return InputCombo(InputDeviceType::Mouse, key); }
		static InputCombo Gamepad(uint32_t key) { return InputCombo(InputDeviceType::Gamepad, key); }

		[[nodiscard]] InputDeviceType GetDevice() const noexcept
		{
			return static_cast<InputDeviceType>(deviceAndKey >> 16);
		}

		[[nodiscard]] uint32_t GetKey() const noexcept { return deviceAndKey & 0xFFFFu; }

		[[nodiscard]] bool IsValid() const noexcept
		{
			return IsValidDevice(GetDevice()) && GetKey() != 0;
		}

		[[nodiscard]] uint32_t Packed() const noexcept { return deviceAndKey; }

		bool operator==(const InputCombo& o) const noexcept { return deviceAndKey == o.deviceAndKey; }
		bool operator<(const InputCombo& o) const noexcept { return deviceAndKey < o.deviceAndKey; }

		friend void to_json(nlohmann::json& j, const InputCombo& c) { j = c.deviceAndKey; }
		friend void from_json(const nlohmann::json& j, InputCombo& c)
		{
			c.deviceAndKey = j.get<uint32_t>();
		}
	};

	/// Helper namespace for serializing std::vector<InputCombo> with
	/// backward-compatible single-int / array-of-int forms.
	struct ComboList
	{
		static void to_json(nlohmann::json& j, const std::vector<InputCombo>& combos)
		{
			if (combos.empty()) {
				j = 0;
				return;
			}
			if (combos.size() == 1) {
				if (combos[0].GetDevice() == InputDeviceType::Keyboard) {
					j = combos[0].GetKey();
				} else {
					j = combos[0].deviceAndKey;
				}
				return;
			}
			std::vector<uint32_t> packed;
			packed.reserve(combos.size());
			for (const auto& c : combos) {
				packed.push_back(
					c.GetDevice() == InputDeviceType::Keyboard ? c.GetKey() : c.deviceAndKey);
			}
			j = packed;
		}

		static void from_json(const nlohmann::json& j, std::vector<InputCombo>& combos)
		{
			combos.clear();
			if (j.is_array()) {
				for (const auto& item : j) {
					if (item.is_number_integer())
						parseAndAdd(item.get<uint32_t>(), combos);
				}
			} else if (j.is_number_integer()) {
				parseAndAdd(j.get<uint32_t>(), combos);
			}
		}

	private:
		static void parseAndAdd(uint32_t val, std::vector<InputCombo>& combos)
		{
			if (val == 0)
				return;
			if (val < 0x10000u) {
				combos.push_back(InputCombo::Keyboard(val));
			} else {
				InputCombo c;
				c.deviceAndKey = val;
				combos.push_back(c);
			}
		}
	};
}  // namespace ImGuiVRHelperPluginAPI

// ADL-discoverable JSON serialization for std::vector<InputCombo>.
namespace nlohmann
{
	inline void to_json(nlohmann::json& j, const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& v)
	{
		ImGuiVRHelperPluginAPI::ComboList::to_json(j, v);
	}
	inline void from_json(const nlohmann::json& j, std::vector<ImGuiVRHelperPluginAPI::InputCombo>& v)
	{
		ImGuiVRHelperPluginAPI::ComboList::from_json(j, v);
	}
}
