#ifndef __BLUR_DITHER_DEPENDENCY_HLSL__
#define __BLUR_DITHER_DEPENDENCY_HLSL__

namespace BlurDither
{
	static const float kTwoPi = 6.28318530718f;
	static const int kSampleCount = 4;

	float2 Hash22(float2 a_pixelPos)
	{
		float3 p3 = frac(float3(a_pixelPos.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
		p3 += dot(p3, p3.yzx + 33.33f);
		return frac((p3.xx + p3.yz) * p3.zy);
	}

	float2 GetOffset(float2 a_pixelPos, int a_sampleIndex)
	{
		static const float2 offsets[kSampleCount] = {
			float2(-0.25f, -0.25f),
			float2(0.25f, -0.25f),
			float2(-0.25f, 0.25f),
			float2(0.25f, 0.25f)
		};

		float angle = Hash22(a_pixelPos).x * kTwoPi;
		float s, c;
		sincos(angle, s, c);
		return mul(float2x2(c, -s, s, c), offsets[a_sampleIndex]);
	}
}

#endif  // __BLUR_DITHER_DEPENDENCY_HLSL__
