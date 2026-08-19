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
	};
}
