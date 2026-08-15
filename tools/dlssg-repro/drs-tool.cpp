// Minimal NVAPI DRS tool for the DLSS-G driver-profile investigation.
// Usage:
//   drs-tool dump                     - print the DLSSG keys on the Skyrim profile
//   drs-tool set <hexId> <hexValue>   - set a DWORD key on the Skyrim profile
//   drs-tool del <hexId>              - remove a key from the Skyrim profile
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Utils/NvApiDrs.h"

namespace
{
	const uint32_t kKeys[] = { 0x104D6667, 0x10E41DF1, Util::NvApiDrs::kKeyDLSSGDisable };

	bool ParseHex(const char* a_text, uint32_t& a_out)
	{
		char* end{};
		unsigned long value = strtoul(a_text, &end, 16);
		if (end == a_text || *end != '\0')
			return false;
		a_out = (uint32_t)value;
		return true;
	}
}

int main(int argc, char** argv)
{
	Util::NvApiDrs::Api drs{};
	if (!drs.Load()) {
		printf("FATAL: nvapi64 DRS entry points unavailable\n");
		return 1;
	}

	Util::NvApiDrs::SessionHandle session{};
	if (drs.CreateSession(&session) != 0 || drs.LoadSettings(session) != 0) {
		printf("FATAL: DRS session/load failed\n");
		return 1;
	}

	uint16_t profileName[2048]{};
	Util::NvApiDrs::Api::CopyProfileName(Util::NvApiDrs::kSkyrimSEProfileName, profileName);
	Util::NvApiDrs::ProfileHandle profile{};
	int findResult = drs.FindProfileByName(session, profileName, &profile);
	if (findResult != 0) {
		printf("FATAL: FindProfileByName failed (%d)\n", findResult);
		return 1;
	}
	printf("Profile found: %ls\n", Util::NvApiDrs::kSkyrimSEProfileName);

	std::string cmd = argc > 1 ? argv[1] : "dump";

	if (cmd == "dump") {
		for (uint32_t id : kKeys) {
			Util::NvApiDrs::Setting setting{};
			setting.version = Util::NvApiDrs::kSettingVersion;
			int result = drs.GetSetting(session, profile, id, &setting);
			if (result == 0)
				printf("key %08x: current=%08x predefined=%08x type=%u isCurrentPredefined=%u\n",
					id, setting.u32CurrentValue, setting.u32PredefinedValue, setting.settingType, setting.isCurrentPredefined);
			else
				printf("key %08x: GetSetting failed (%d)\n", id, result);
		}
	} else if (cmd == "set" && argc == 4) {
		uint32_t id{};
		uint32_t value{};
		if (!ParseHex(argv[2], id) || !ParseHex(argv[3], value)) {
			printf("invalid hex argument\n");
			return 1;
		}
		Util::NvApiDrs::Setting setting{};
		setting.version = Util::NvApiDrs::kSettingVersion;
		setting.settingId = id;
		setting.settingType = 0;  // DWORD
		setting.u32CurrentValue = value;
		if (drs.SetSetting(session, profile, &setting) != 0) {
			printf("SetSetting %08x=%08x failed\n", id, value);
			return 1;
		}
		if (drs.SaveSettings(session) != 0) {
			printf("SaveSettings failed\n");
			return 1;
		}
		printf("OK: key %08x set to %08x and saved\n", id, value);
	} else if (cmd == "del" && argc == 3) {
		uint32_t id{};
		if (!ParseHex(argv[2], id)) {
			printf("invalid hex argument\n");
			return 1;
		}
		if (drs.DeleteProfileSetting(session, profile, id) != 0) {
			printf("DeleteProfileSetting %08x failed\n", id);
			return 1;
		}
		if (drs.SaveSettings(session) != 0) {
			printf("SaveSettings failed\n");
			return 1;
		}
		printf("OK: key %08x deleted (reverts to driver predefined) and saved\n", id);
	} else {
		printf("usage: drs-tool dump | set <hexId> <hexValue> | del <hexId>\n");
	}

	drs.DestroySession(session);
	return 0;
}
