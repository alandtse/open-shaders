#pragma once

namespace DX12
{
	/** @brief Only a bounded, non-truncated whole-interior result can seed subdivided requests. */
	constexpr bool CanReuseWholeInterior(bool attempted, bool consolidated, bool validated, bool enumerated,
		bool budgetExceeded, bool captureBudgetExceeded, bool rejectedLights, bool allEligibleExtracted)
	{
		return attempted && !consolidated && validated && enumerated && !budgetExceeded &&
			!captureBudgetExceeded && !rejectedLights && allEligibleExtracted;
	}

	enum class TilePreparationOutcome
	{
		Retry,
		Ready,
		Partial,
		Empty,
		EmptyPartial
	};

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
