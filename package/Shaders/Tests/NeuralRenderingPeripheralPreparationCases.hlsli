#include "/Shaders/Common/NeuralRenderingPeripheralPreparation.hlsli"
#include "/Test/STF/ShaderTestFramework.hlsli"

void CheckPeripheralFootprintExcludesCornerContamination()
{
	NeuralRenderingPeripheral::Mapping map;
	map.protectedFraction = 0.0;
	map.boundaryScale = 0.5;
	map.edgeSlope = 0.25;
	map.padding = 0;
	float2 lower, upper;
	const NeuralRenderingPeripheral::Footprint footprint = NeuralRenderingPeripheral::BuildFootprint(uint2(0, 0), 128.0, 1024.0, map, lower, upper);
	float weights[NeuralRenderingPeripheral::kFootprintPatchCount];
	NeuralRenderingPeripheral::RowWeights(footprint, 0u, weights);
	ASSERT(IsTrue, NeuralRenderingPeripheral::SourceWeight(footprint, weights, 0u) > 0.0);
	ASSERT(IsTrue, NeuralRenderingPeripheral::SourceWeight(footprint, weights, 12u) == 0.0);
	float2 wholeLower, wholeUpper;
	NeuralRenderingPeripheral::SourceBounds(0.0, 1.0 / 128.0, map, wholeLower, wholeUpper);
	ASSERT(IsTrue, all(wholeLower <= float2(12.0, 0.0) / 1024.0));
	ASSERT(IsTrue, all(wholeUpper > float2(12.0, 0.0) / 1024.0));
	ASSERT(IsTrue, all(lower >= 0.0) && all(upper <= 1024.0));
	for (uint y = 0; y <= 8; ++y)
		for (uint x = 0; x <= 8; ++x) {
			const float2 mappedPoint = NeuralRenderingPeripheral::Inverse(float2(x, y) / (8.0 * 128.0), map) * 1024.0;
			bool covered = false;
			[unroll] for (uint patch = 0; patch < NeuralRenderingPeripheral::kFootprintPatchCount; ++patch)
				covered = covered || (all(mappedPoint >= footprint.lower[patch] - 1e-3) && all(mappedPoint <= footprint.upper[patch] + 1e-3));
			ASSERT(IsTrue, covered);
		}
}

void CheckPeripheralGuideKeepsDepthMotionAndAnchorTogether()
{
	NeuralRenderingPeripheral::GuideDonor donor = NeuralRenderingPeripheral::EmptyGuideDonor();
	const float farDepth = FrameBuffer::FarPlaneDepth();
	const float nearerDepth = FrameBuffer::ToNativeDepth(0.5);
	ASSERT(IsTrue, !donor.found && donor.depth == farDepth && all(donor.motion == 0.0));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, asfloat(0x7FC00000u)));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, asfloat(0x7F800000u)));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, -0.01));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, 1.01));
	ASSERT(IsTrue, NeuralRenderingPeripheral::CanReplaceGuide(donor, farDepth));
	NeuralRenderingPeripheral::AcceptGuide(donor, farDepth, float2(0.1, -0.2), uint2(5, 7));
	ASSERT(IsTrue, donor.found && donor.depth == farDepth && all(donor.pixel == uint2(5, 7)));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, farDepth));
	ASSERT(IsTrue, NeuralRenderingPeripheral::CanReplaceGuide(donor, nearerDepth));
	NeuralRenderingPeripheral::AcceptGuide(donor, nearerDepth, float2(0.2, asfloat(0x7FC00000u)), uint2(13, 19));
	ASSERT(IsTrue, donor.depth == farDepth && all(donor.motion == float2(0.1, -0.2)) && all(donor.pixel == uint2(5, 7)));
	NeuralRenderingPeripheral::AcceptGuide(donor, nearerDepth, float2(0.2, 0.1), uint2(13, 19));
	ASSERT(IsTrue, donor.depth == nearerDepth && all(donor.motion == float2(0.2, 0.1)) && all(donor.pixel == uint2(13, 19)));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, nearerDepth));
	ASSERT(IsTrue, !NeuralRenderingPeripheral::CanReplaceGuide(donor, farDepth));
	ASSERT(IsTrue, NeuralRenderingPeripheral::CanReplaceGuide(donor, FrameBuffer::NearPlaneDepth()));
	NeuralRenderingPeripheral::Mapping map;
	map.protectedFraction = 0.2;
	map.boundaryScale = 0.6;
	map.edgeSlope = 0.25;
	map.padding = 0;
	const float2 anchor = (float2(13, 19) + 0.5) / 128.0;
	const float2 modelPosition = NeuralRenderingPeripheral::Forward(anchor, map);
	const float2 motion = NeuralRenderingPeripheral::GuideMotion(donor, 128.0, map);
	ASSERT(IsTrue, all(abs(NeuralRenderingPeripheral::Inverse(modelPosition + motion, map) - anchor - donor.motion) < 2e-6));
	const NeuralRenderingPeripheral::GuideDonor empty = NeuralRenderingPeripheral::EmptyGuideDonor();
	ASSERT(IsTrue, all(NeuralRenderingPeripheral::GuideMotion(empty, 128.0, map) == 0.0));
}
