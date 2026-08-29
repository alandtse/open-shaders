#pragma once

namespace NeuralRendering
{
	/** Runs DLSS Neural Rendering on the LDR foveated regions immediately before UI composite. */
	bool ApplyFoveatedLdr();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}