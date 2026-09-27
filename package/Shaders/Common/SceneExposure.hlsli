#ifndef __SCENE_EXPOSURE_DEPENDENCY_HLSL__
#define __SCENE_EXPOSURE_DEPENDENCY_HLSL__

namespace SceneExposure
{
	static const float kMiddleGrey = 0.18f;

	float Evaluate(float adaptedLuminance, float2 luminanceRange, float compensationScale)
	{
		return kMiddleGrey * compensationScale / clamp(adaptedLuminance, luminanceRange.x, luminanceRange.y);
	}
}

#endif  // __SCENE_EXPOSURE_DEPENDENCY_HLSL__
