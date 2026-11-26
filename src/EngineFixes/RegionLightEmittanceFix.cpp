#include "RegionLightEmittanceFix.h"

void RegionLightEmittanceFix::Install()
{
	const auto updateManagedNodes = REL::RelocationID(18464, 18895).address();
	const auto previousWeatherArg = updateManagedNodes + REL::Relocate(0x3DC, 0x3CA, 0x3DC);
	constexpr auto original = REL::make_pattern<"45 33 C9 4D 8B C6 48 8D 55 E7 48 8B CF E8">();
	// The region resolves one weather, so both blend inputs need it or transitioning lights fade to black.
	constexpr std::uint8_t movR9R14[]{ 0x4D, 0x8B, 0xCE };
	if (!REL::safe_write(previousWeatherArg, movR9R14, sizeof(movR9R14), original))
		logger::warn("[Engine Fixes] {} skipped: unexpected regional emittance blend arguments", GetName());
}
