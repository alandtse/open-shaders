#define REVERSE_Z
#include "/Shaders/Tests/NeuralRenderingPeripheralPreparationCases.hlsli"

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestPeripheralReverseDepthKeepsTheValidGuideDonor() {
	CheckPeripheralGuideKeepsDepthMotionAndAnchorTogether();
}
