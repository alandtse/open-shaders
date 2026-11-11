#include "/Shaders/Common/NeuralRenderingPeripheral.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

NeuralRenderingPeripheral::Mapping MappingForPercent(float percent)
{
	NeuralRenderingPeripheral::Mapping map;
	map.protectedFraction = 1.0 - percent * 0.01;
	map.boundaryScale = (1.0 + map.protectedFraction) * 0.5;
	map.edgeSlope = 0.25;
	map.padding = 0;
	return map;
}

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestPeripheralRectanglePreservesCornersAndCentre() {
	const NeuralRenderingPeripheral::Mapping map = MappingForPercent(75.0);
	ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Forward(float2(0.0, 0.0), map)) < 1e-6));
	ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Forward(float2(1.0, 1.0), map) - 1.0) < 1e-6));
	ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Forward(float2(0.5, 0.5), map) - 0.5) < 1e-6));
}

	/// @tags neural-rendering
	[numthreads(1, 1, 1)] void TestPeripheralInverseReturnsAllSourceSamples()
{
	for (uint percent = 0; percent <= 100; percent += 20) {
		const NeuralRenderingPeripheral::Mapping map = MappingForPercent(float(percent));
		for (uint x = 0; x <= 10; ++x)
			for (uint y = 0; y <= 10; ++y) {
				const float2 source = float2(x, y) * 0.1;
				const float2 model = NeuralRenderingPeripheral::Forward(source, map);
				ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Inverse(model, map) - source) < 2e-6));
			}
	}
}

/// @tags neural-rendering
[numthreads(1, 1, 1)] void TestPeripheralMotionTransformsEndpointsWithoutClipping() {
	const NeuralRenderingPeripheral::Mapping map = MappingForPercent(80.0);
	const float2 anchor = float2(0.98, 0.25), velocity = float2(0.1, -0.4);
	const float2 current = NeuralRenderingPeripheral::Forward(anchor, map);
	const float2 motion = NeuralRenderingPeripheral::Motion(anchor, velocity, map);
	ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Inverse(current + motion, map) - anchor - velocity) < 2e-6));
	ASSERT(IsTrue, all(NeuralRenderingPeripheral::Motion(anchor, 0.0, map) == 0.0));
	ASSERT(IsTrue, all(NeuralRenderingPeripheral::Motion(anchor, asfloat(0x7FC00000u).xx, map) == 0.0));
}

	/// @tags neural-rendering
	[numthreads(1, 1, 1)] void TestPeripheralAreaFootprintCoversAxisCrossings()
{
	const NeuralRenderingPeripheral::Mapping map = MappingForPercent(100.0);
	const float2 first = float2(0.49, 0.39), end = float2(0.51, 0.41);
	float2 lower, upper;
	NeuralRenderingPeripheral::SourceBounds(first, end, map, lower, upper);
	for (uint y = 0; y <= 10; ++y)
		for (uint x = 0; x <= 10; ++x) {
			const float2 sample = NeuralRenderingPeripheral::Inverse(lerp(first, end, float2(x, y) * 0.1), map);
			ASSERT(IsTrue, all(sample >= lower - 1e-6) && all(sample <= upper + 1e-6));
		}
}
