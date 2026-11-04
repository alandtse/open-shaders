#define REVERSE_Z
#include "NeuralRenderingPeripheralPreparationCases.hlsli"

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestPeripheralReverseDepthKeepsTheValidGuideDonor() {
	CheckPeripheralGuideKeepsDepthMotionAndAnchorTogether();
}
