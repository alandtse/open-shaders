#pragma once

struct Upscaling;

namespace DlssEnhancerImpl
{
	class Preprocess
	{
	public:
		// Mirrors Upscaling::EncodeUpscalingTextures (with a DLSS-specific
		// shader define) so the DlssEnhancer route can prepare reactive +
		// transparency masks without touching dev's path.
		static bool EncodeUpscalingTextures(Upscaling& upscaling);
	};
}
