#ifndef COMMON_UI_COMPOSITION_HLSLI
#define COMMON_UI_COMPOSITION_HLSLI

#include "Common/Color.hlsli"

namespace UIComposition
{
	float3 CompositeSDR(float3 scene, float4 ui, bool sceneIsLinear, bool sceneIsBT2020, float brightness)
	{
		[branch] if (ui.a != 0.0 || any(ui.rgb != 0.0))
		{
			if (sceneIsBT2020)
				scene = Color::BT2020ToBT709(scene);
			if (sceneIsLinear)
				scene = Color::LinearToGammaSafe(scene);
			// SDR-authored coverage must blend before decoding premultiplied gamma RGB.
			scene = ui.rgb * Color::LinearToSrgb(max(0.0, brightness).xxx) + scene * (1.0 - saturate(ui.a));
			if (sceneIsLinear)
				scene = Color::GammaToLinearSafe(scene);
			if (sceneIsBT2020)
				scene = Color::BT709ToBT2020(scene);
		}
		return scene;
	}
}

#endif
