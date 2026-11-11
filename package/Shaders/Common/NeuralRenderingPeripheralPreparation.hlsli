#ifndef NEURAL_RENDERING_PERIPHERAL_PREPARATION_HLSLI
#define NEURAL_RENDERING_PERIPHERAL_PREPARATION_HLSLI

#include "NeuralRenderingPeripheral.hlsli"
#include "ReverseZ.hlsli"

namespace NeuralRenderingPeripheral
{
	static const uint kFootprintSubdivisions = 2u;
	static const uint kFootprintPatchCount = kFootprintSubdivisions * kFootprintSubdivisions;

	/** @brief Conservative source rectangles for one warped model cell. */
	struct Footprint
	{
		float2 lower[kFootprintPatchCount];
		float2 upper[kFootprintPatchCount];
	};

	/** @brief Bounds the four inverse subcells for one shared colour and guide traversal. */
	Footprint BuildFootprint(uint2 pixel, float2 grid, float2 sourceSize, Mapping map, out float2 lower, out float2 upper)
	{
		Footprint footprint;
		lower = sourceSize;
		upper = 0.0;
		[unroll] for (uint patch = 0; patch < kFootprintPatchCount; ++patch)
		{
			const float2 position = float2(pixel * kFootprintSubdivisions + uint2(patch % kFootprintSubdivisions, patch / kFootprintSubdivisions));
			const float2 patchGrid = grid * kFootprintSubdivisions;
			float2 patchLower, patchUpper;
			SourceBounds(position / patchGrid, (position + 1.0) / patchGrid, map, patchLower, patchUpper);
			patchLower = clamp(patchLower * sourceSize, 0.0, sourceSize);
			patchUpper = clamp(patchUpper * sourceSize, 0.0, sourceSize);
			const bool2 empty = patchUpper <= patchLower;
			patchLower = float2(empty.x ? min(patchLower.x, sourceSize.x - 1.0) : patchLower.x, empty.y ? min(patchLower.y, sourceSize.y - 1.0) : patchLower.y);
			patchUpper = float2(empty.x ? patchLower.x + 1.0 : patchUpper.x, empty.y ? patchLower.y + 1.0 : patchUpper.y);
			footprint.lower[patch] = patchLower;
			footprint.upper[patch] = patchUpper;
			lower = min(lower, patchLower);
			upper = max(upper, patchUpper);
		}
		return footprint;
	}

	/** @brief Reuses vertical overlap for every source sample in a row. */
	void RowWeights(Footprint footprint, uint y, out float weights[kFootprintPatchCount])
	{
		[unroll] for (uint patch = 0; patch < kFootprintPatchCount; ++patch)
			weights[patch] = max(0.0, min(footprint.upper[patch].y, float(y + 1u)) - max(footprint.lower[patch].y, float(y)));
	}

	/** @brief Uses identical subcell coverage for colour and nearest-surface guides. */
	float SourceWeight(Footprint footprint, float rowWeights[kFootprintPatchCount], uint x)
	{
		float weight = 0.0;
		[unroll] for (uint patch = 0; patch < kFootprintPatchCount; ++patch)
			weight += rowWeights[patch] * max(0.0, min(footprint.upper[patch].x, float(x + 1u)) - max(footprint.lower[patch].x, float(x)));
		return weight;
	}

	/** @brief One accepted source surface and its matching motion sample. */
	struct GuideDonor
	{
		float depth;
		float2 motion;
		uint2 pixel;
		bool found;
	};

	/** @brief Initializes the guide fallback in the current depth convention. */
	GuideDonor EmptyGuideDonor()
	{
		GuideDonor donor;
		donor.depth = FrameBuffer::FarPlaneDepth();
		donor.motion = 0.0;
		donor.pixel = 0u;
		donor.found = false;
		return donor;
	}

	/** @brief Rejects losing depths before their motion needs to be fetched. */
	bool CanReplaceGuide(GuideDonor donor, float depth)
	{
		return NeuralRenderingFinite::IsFinite(depth) && depth >= 0.0 && depth <= 1.0 && (!donor.found || FrameBuffer::IsNearerDepth(depth, donor.depth));
	}

	/** @brief Commits a qualified depth and its finite velocity as one donor. */
	void AcceptGuide(inout GuideDonor donor, float depth, float2 motion, uint2 pixel)
	{
		if (all(NeuralRenderingFinite::IsFinite(motion))) {
			donor.depth = depth;
			donor.motion = motion;
			donor.pixel = pixel;
			donor.found = true;
		}
	}

	/** @brief Anchors nonlinear motion at the same source texel that supplied depth. */
	float2 GuideMotion(GuideDonor donor, float2 sourceSize, Mapping map)
	{
		return Motion((float2(donor.pixel) + 0.5) / sourceSize, donor.motion, map);
	}
}

#endif
