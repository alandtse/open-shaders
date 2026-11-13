#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace DX12ResourceService
{
	/** Source parity against open-RXRT/tools/redstone-scene-prep. */
	struct ParityStatus
	{
		bool winningRecordsResolved = false;
		bool referenceSweepComplete = false;
		bool nifProvenanceVerified = false;
		bool compiledStaticGeometry = false;
		bool composedTransforms = false;
		bool compiledShadingAttributes = false;
		bool collectedStaticLights = false;
		bool outputValidated = false;
		uint32_t placedReferences = 0, compiledShapes = 0;
		uint64_t compiledTriangles = 0;

		[[nodiscard]] bool Complete() const
		{
			return winningRecordsResolved && referenceSweepComplete && nifProvenanceVerified &&
			       compiledStaticGeometry && composedTransforms && compiledShadingAttributes &&
			       collectedStaticLights && outputValidated && placedReferences > 0 &&
			       compiledShapes > 0 && compiledTriangles > 0;
		}
		[[nodiscard]] nlohmann::json ToJson() const
		{
			return {
				{ "winningRecordsResolved", winningRecordsResolved },
				{ "referenceSweepComplete", referenceSweepComplete },
				{ "nifProvenanceVerified", nifProvenanceVerified },
				{ "compiledStaticGeometry", compiledStaticGeometry },
				{ "composedTransforms", composedTransforms },
				{ "compiledShadingAttributes", compiledShadingAttributes },
				{ "collectedStaticLights", collectedStaticLights },
				{ "outputValidated", outputValidated },
				{ "placedReferences", placedReferences }, { "compiledShapes", compiledShapes },
				{ "compiledTriangles", compiledTriangles }, { "parityComplete", Complete() }
			};
		}
	};
}
