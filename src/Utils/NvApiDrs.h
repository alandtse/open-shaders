#pragma once

#include <windows.h>

#include <cstdint>

// Minimal NVAPI driver-settings (DRS) access via nvapi64 QueryInterface; the public
// NVAPI SDK is not vendored. Shared by the runtime DLSS-G driver-profile reset and
// the standalone drs-tool diagnostic.
namespace Util::NvApiDrs
{
	using SessionHandle = void*;
	using ProfileHandle = void*;

#pragma pack(push, 8)
	struct Setting
	{
		uint32_t version;
		uint16_t settingName[2048];
		uint32_t settingId;
		uint32_t settingType;
		uint32_t settingLocation;
		uint32_t isCurrentPredefined;
		uint32_t isPredefinedValid;
		union
		{
			uint32_t u32PredefinedValue;
			uint8_t binaryPredefinedValue[4100];
		};
		union
		{
			uint32_t u32CurrentValue;
			uint8_t binaryCurrentValue[4100];
		};
	};
#pragma pack(pop)

	inline constexpr uint32_t kSettingVersion = static_cast<uint32_t>(sizeof(Setting)) | (1u << 16);

	// DRS key on the game's driver profile that makes sl.dlss_g silently disable
	// interpolation while every API still returns eOk (written by the NVIDIA App's
	// DLSS-override panel; driver default is 0).
	inline constexpr uint32_t kKeyDLSSGDisable = 0x10308298;
	// DRS master switch for the driver's own frame-generation layer (NVIDIA App's
	// "Smooth Motion" panel; NvPresent64.dll). Driver default is 0 (off); 1 = on.
	inline constexpr uint32_t kKeySmoothMotionEnable = 0xB0D384C0;
	inline constexpr wchar_t kSkyrimSEProfileName[] = L"The Elder Scrolls V: Skyrim Special Edition";

	struct Api
	{
		int(__cdecl* Initialize)(){};
		int(__cdecl* CreateSession)(SessionHandle*){};
		int(__cdecl* DestroySession)(SessionHandle){};
		int(__cdecl* LoadSettings)(SessionHandle){};
		int(__cdecl* SaveSettings)(SessionHandle){};
		int(__cdecl* FindProfileByName)(SessionHandle, uint16_t*, ProfileHandle*){};
		int(__cdecl* GetSetting)(SessionHandle, ProfileHandle, uint32_t, Setting*){};
		int(__cdecl* SetSetting)(SessionHandle, ProfileHandle, Setting*){};
		int(__cdecl* DeleteProfileSetting)(SessionHandle, ProfileHandle, uint32_t){};

		/** @brief Loads nvapi64 and resolves the DRS entry points; false when unavailable. */
		bool Load()
		{
			HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
			if (!nvapi)
				return false;
			using PQueryInterface = void*(__cdecl*)(uint32_t);
			auto queryInterface = (PQueryInterface)GetProcAddress(nvapi, "nvapi_QueryInterface");
			if (!queryInterface)
				return false;
			*(void**)&Initialize = queryInterface(0x0150E828);
			*(void**)&CreateSession = queryInterface(0x0694D52E);
			*(void**)&DestroySession = queryInterface(0xDAD9CFF8);
			*(void**)&LoadSettings = queryInterface(0x375DBD6B);
			*(void**)&SaveSettings = queryInterface(0xFCBC7E14);
			*(void**)&FindProfileByName = queryInterface(0x7E4A9A0B);
			*(void**)&GetSetting = queryInterface(0x73BF8338);
			*(void**)&SetSetting = queryInterface(0x577DD202);
			*(void**)&DeleteProfileSetting = queryInterface(0xE4A26362);
			return Initialize && CreateSession && DestroySession && LoadSettings && SaveSettings &&
			       FindProfileByName && GetSetting && SetSetting && DeleteProfileSetting &&
			       Initialize() == 0;
		}

		/** @brief Fills a_profileName from a null-terminated source, always terminating. */
		static void CopyProfileName(const wchar_t* a_source, uint16_t (&a_profileName)[2048])
		{
			size_t i = 0;
			for (; a_source[i] && i < 2047; i++)
				a_profileName[i] = static_cast<uint16_t>(a_source[i]);
			a_profileName[i] = 0;
		}

		/**
		 * @brief Loads the driver, opens a session, and finds the Skyrim SE profile
		 * within it. On success a_session is left open (caller must DestroySession);
		 * on failure any partially-opened session is already closed.
		 */
		bool TryOpenSkyrimProfile(SessionHandle& a_session, ProfileHandle& a_profile)
		{
			if (!Load() || CreateSession(&a_session) != 0)
				return false;
			if (LoadSettings(a_session) != 0) {
				DestroySession(a_session);
				return false;
			}

			uint16_t profileName[2048]{};
			CopyProfileName(kSkyrimSEProfileName, profileName);
			if (FindProfileByName(a_session, profileName, &a_profile) == 0)
				return true;

			DestroySession(a_session);
			return false;
		}

		/**
		 * @brief Reads a_settingId from the Skyrim SE driver profile. Missing
		 * profile/setting is not an error: a_outValue is left unset and false is
		 * returned, matching driver defaults (no profile override).
		 */
		bool TryGetSkyrimSetting(uint32_t a_settingId, uint32_t& a_outValue)
		{
			SessionHandle session{};
			ProfileHandle profile{};
			if (!TryOpenSkyrimProfile(session, profile))
				return false;

			Setting setting{};
			setting.version = kSettingVersion;
			bool found = GetSetting(session, profile, a_settingId, &setting) == 0;
			if (found)
				a_outValue = setting.u32CurrentValue;

			DestroySession(session);
			return found;
		}
	};
}
