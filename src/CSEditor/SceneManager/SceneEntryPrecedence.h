#pragma once

#include <array>

namespace SceneEntryPrecedence
{
	enum class Rank
	{
		User,
		Preset,
		Suppression
	};

	inline constexpr std::array kApplyOrder{ Rank::User, Rank::Preset, Rank::Suppression };

	/// An explicit user suppression still hides preset values at the same address.
	constexpr Rank GetRank(bool fromPreset, bool deleted)
	{
		return fromPreset ? Rank::Preset : deleted ? Rank::Suppression :
		                                             Rank::User;
	}
}
