#pragma once

namespace DX12
{
	enum class TilePreparationOutcome { Retry, Ready, Partial, Empty, EmptyPartial };

	/** @brief A finished empty enumeration is terminal, but only complete coverage proves emptiness. */
	constexpr TilePreparationOutcome ClassifyTilePreparation(bool validated, bool enumerated, bool complete, bool hasContent)
	{
		if (!validated)
			return TilePreparationOutcome::Retry;
		if (hasContent)
			return complete ? TilePreparationOutcome::Ready : TilePreparationOutcome::Partial;
		if (!enumerated)
			return TilePreparationOutcome::Retry;
		return complete ? TilePreparationOutcome::Empty : TilePreparationOutcome::EmptyPartial;
	}
}
