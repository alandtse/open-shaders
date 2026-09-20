#pragma once

#include <array>
#include <string_view>

namespace SpellStormRecords
{
	/** Identifies one vanilla area-of-effect storm spell that SpellShoutWindRouter routes and
	 *  ExplosionWindRouter must not double-process as an unrelated explosion. */
	struct Spell
	{
		RE::FormID localFormID;
		std::string_view editorID;
	};

	inline constexpr std::array<Spell, 3> kSpells{
		Spell{ 0x7A82B, "FireStorm" },
		Spell{ 0x7E8E4, "Blizzard" },
		Spell{ 0x7E8E5, "LightningStorm" }
	};
}
