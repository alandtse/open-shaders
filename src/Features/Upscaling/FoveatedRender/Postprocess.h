#pragma once

struct Upscaling;

namespace FoveatedRenderImpl
{
	class Postprocess
	{
	public:
		// Sharpening pass for the FoveatedRender route. Mirrors what
		// Upscaling::ApplySharpening does but is invoked from
		// Main_PostProcessing only when the FoveatedRender route is active.
		// Only the kRCAS path is wired.
		static bool ApplyDlssSharpening(Upscaling& upscaling);
	};
}
