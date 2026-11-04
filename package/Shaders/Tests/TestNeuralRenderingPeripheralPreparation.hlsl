#include "NeuralRenderingPeripheralPreparationCases.hlsli"

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestPeripheralSubcellsShareExactColorAndGuideCoverage() {
	CheckPeripheralFootprintExcludesCornerContamination();
}

	/// @tags neural-rendering
	[numthreads(1, 1, 1)] void TestPeripheralForwardDepthKeepsTheValidGuideDonor()
{
	CheckPeripheralGuideKeepsDepthMotionAndAnchorTogether();
}
