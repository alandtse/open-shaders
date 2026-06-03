#include "Features/LightLimitFix/ParticleLights.h"

#include "Utils/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace
{
	// User-editable INI values feed light radius/color math; reject non-finite and
	// clamp to a sane range so a malformed entry can't destabilize the pipeline.
	float SanitizeIniFloat(double a_value, float a_min, float a_max, float a_default)
	{
		const float f = static_cast<float>(a_value);
		if (!std::isfinite(f))
			return a_default;
		return std::clamp(f, a_min, a_max);
	}
}

void ParticleLights::GetConfigs()
{
	++configVersion;
	particleLightConfigs.clear();
	particleLightGradientConfigs.clear();

	particleLightConfigs["default"] = Config{};

	if (std::filesystem::exists("Data\\ParticleLights")) {
		logger::info("[LLF] Loading particle lights configs");

		auto configs = clib_util::distribution::get_configs("Data\\ParticleLights", "", ".ini");
		// get_configs order is filesystem-dependent; sort so the "first wins"
		// duplicate policy below is deterministic across installs.
		std::sort(configs.begin(), configs.end());

		if (configs.empty()) {
			logger::warn("[LLF] No .ini files were found within the Data\\ParticleLights folder, aborting...");
		} else {
			logger::info("[LLF] {} matching inis found", configs.size());

			for (auto& path : configs) {
				logger::info("[LLF] loading ini : {}", path);

				CSimpleIniA ini;
				ini.SetUnicode();
				ini.SetMultiKey();

				if (const auto rc = ini.LoadFile(path.c_str()); rc < 0) {
					logger::error("\t\t[LLF] couldn't read INI");
					continue;
				}

				Config data{};

				data.cull = ini.GetBoolValue("Light", "Cull", false);
				data.colorMult.red = SanitizeIniFloat(ini.GetDoubleValue("Light", "ColorMultRed", 1.0), 0.0f, 16.0f, 1.0f);
				data.colorMult.green = SanitizeIniFloat(ini.GetDoubleValue("Light", "ColorMultGreen", 1.0), 0.0f, 16.0f, 1.0f);
				data.colorMult.blue = SanitizeIniFloat(ini.GetDoubleValue("Light", "ColorMultBlue", 1.0), 0.0f, 16.0f, 1.0f);
				data.radiusMult = SanitizeIniFloat(ini.GetDoubleValue("Light", "RadiusMult", 1.0), 0.0f, 16.0f, 1.0f);

				const auto filename = Util::GetLowercaseStem(path, ".ini");
				if (!filename) {
					continue;
				}

				// Legacy first-win policy: keep behavior compatible with older particle light packs.
				if (auto it = particleLightConfigs.find(*filename); it != particleLightConfigs.end()) {
					logger::warn("[LLF] Duplicate particle config '{}'; keeping first entry, ignoring {}", *filename, path);
					continue;
				}

				logger::debug("[LLF] Inserting {}", *filename);
				particleLightConfigs.emplace(*filename, data);
			}
		}
	}

	if (std::filesystem::exists("Data\\ParticleLights\\Gradients")) {
		logger::info("[LLF] Loading particle lights gradients configs");

		auto configs = clib_util::distribution::get_configs("Data\\ParticleLights\\Gradients", "", ".ini");
		// Deterministic "first wins" across installs (see above).
		std::sort(configs.begin(), configs.end());

		if (configs.empty()) {
			logger::warn("[LLF] No .ini files were found within the Data\\ParticleLights\\Gradients folder, aborting...");
			return;
		}

		logger::info("[LLF] {} matching inis found", configs.size());

		for (auto& path : configs) {
			logger::info("[LLF] loading ini : {}", path);

			CSimpleIniA ini;
			ini.SetUnicode();
			ini.SetMultiKey();

			if (const auto rc = ini.LoadFile(path.c_str()); rc < 0) {
				logger::error("\t\t[LLF] couldn't read INI");
				continue;
			}

			GradientConfig data{};
			constexpr std::string_view prefix1 = "0x";
			constexpr std::string_view prefix2 = "#";
			constexpr std::string_view cset = "0123456789ABCDEFabcdef";

			const char* value = ini.GetValue("Gradient", "Color");
			if (value && strcmp(value, "") != 0) {
				std::string_view str = value;

				if (str.starts_with(prefix1))
					str.remove_prefix(prefix1.size());
				if (str.starts_with(prefix2))
					str.remove_prefix(prefix2.size());

				// Require exactly 6 (RRGGBB) or 8 (AARRGGBB) hex digits so malformed
				// widths like "#1" / "#12345" fail closed instead of packing a garbage color.
				// find_first_not_of is bounded to the view; strspn would rely on
				// str.data() being NUL-terminated, which string_view doesn't guarantee.
				bool matches = (str.size() == 6 || str.size() == 8) &&
				               str.find_first_not_of(cset) == std::string_view::npos;

				if (matches) {
					try {
						uint32_t color = static_cast<uint32_t>(std::stoul(std::string(str), nullptr, 16));
						data.color = color;
					} catch (const std::exception&) {
						logger::error("[LLF] invalid color");
						continue;
					}
				} else {
					logger::error("[LLF] invalid color");
					continue;
				}
			} else {
				logger::error("[LLF] missing color");
				continue;
			}

			const auto filename = Util::GetLowercaseStem(path, ".ini");
			if (!filename) {
				continue;
			}

			if (auto it = particleLightGradientConfigs.find(*filename); it != particleLightGradientConfigs.end()) {
				logger::warn("[LLF] Duplicate particle gradient config '{}'; keeping first entry, ignoring {}", *filename, path);
				continue;
			}

			logger::debug("[LLF] Inserting {}", *filename);
			particleLightGradientConfigs.emplace(*filename, data);
		}
	}
}
