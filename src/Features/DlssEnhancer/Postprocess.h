#pragma once

struct Upscaling;

namespace DlssEnhancer
{
	class Postprocess
	{
	public:
		// Sharpening pass for the DlssEnhancer route. Mirrors what
		// Upscaling::ApplySharpening does on dev's path but is invoked from
		// Main_PostProcessing only when the DlssEnhancer route is active.
		// MVP-B: only the kRCAS sharpen mode is wired (no DLSSperf-aware
		// path — that's PR-2 + PR-3 integration).
		static bool ApplyDlssSharpening(Upscaling& upscaling);
	};
}
