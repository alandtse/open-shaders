#pragma once

#include "OpenWind/Types.h"

#include <cstdint>

namespace OpenWind
{
	enum class WindFieldDebugView : uint32_t
	{
		Blended,
		Current,
		Previous,
		Both,
		Comparison,
		Spring,
		TransientImpulses,
		TerrainShelter,
		ObstacleShelter,
		CombinedShelter,
		Shapes,
		TreeSpringAmbient,
		TreeSpringTransient,
		TreeSpringVelocity,
		TreeSpringCoverage,
		TreeSpringHeight
	};

	struct RuntimeWindTest
	{
		bool enabled = false;
		float speed = 1.0f;
		float gustScale = 2048.0f;
		float gustAmplitude = 0.35f;
		float gustAdvectionMultiplier = 1.0f;
	};

	struct WindRuntimeState
	{
		bool visualizeWindField = false;
		bool windFieldUseRealSpeed = true;
		bool windFieldUseRealDirection = true;
		float windFieldOverrideSpeed = 1.0f;
		float windFieldPendingDirectionDegrees = 0.0f;
		float windFieldAppliedDirectionDegrees = 0.0f;
		WindFieldDebugView windFieldDebugView = WindFieldDebugView::Blended;
		RuntimeWindTest treeWindTest;
	};
}
