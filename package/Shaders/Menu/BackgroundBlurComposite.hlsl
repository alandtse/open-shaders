// Composite Blur Pass Shader with Rounded Rectangle Mask
// Part of the BackgroundBlur system - applies blurred texture with rounded corners

#include "Common/BlurDither.hlsli"

cbuffer WindowBuffer : register(b1)
{
	float4 WindowRect;    // x = minX, y = minY, z = maxX, w = maxY (in pixels)
	float4 WindowParams;  // x = cornerRadius, y = screenWidth, z = screenHeight, w = fullscreen (1.0 = skip SDF)
};

SamplerState LinearSampler : register(s0);
Texture2D InputTexture : register(t0);

static const float DOWNSAMPLE_FACTOR = 8.0f;
static const float CLIP_EPSILON = 0.001f;

struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
};

VS_OUTPUT VS_Main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT output;
	output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
	output.Position = float4(output.TexCoord * 2.0f - 1.0f, 0.0f, 1.0f);
	output.Position.y = -output.Position.y;
	return output;
}

// Soft sampling with blurred dithering - takes 4 samples with jittered offsets
// and averages them to smooth out the noise while still breaking up blocky pixels
float4 SampleWithSoftening(float2 uv, float2 pixelPos, float2 texelSize)
{
	float4 result = 0;
	[unroll] for (int i = 0; i < BlurDither::kSampleCount; i++)
	{
		result += InputTexture.Sample(LinearSampler, uv + BlurDither::GetOffset(pixelPos, i) * texelSize);
	}

	return result / (float)BlurDither::kSampleCount;
}

// Compute signed distance to a rounded rectangle
// Returns negative inside, positive outside
float RoundedRectSDF(float2 pixelPos, float2 rectMin, float2 rectMax, float radius)
{
	// Center of the rectangle
	float2 rectCenter = (rectMin + rectMax) * 0.5f;
	float2 rectHalfSize = (rectMax - rectMin) * 0.5f;

	// Clamp radius to not exceed half the smallest dimension
	radius = min(radius, min(rectHalfSize.x, rectHalfSize.y));

	// Distance from center
	float2 p = abs(pixelPos - rectCenter) - rectHalfSize + radius;

	// SDF for rounded rectangle
	return length(max(p, 0.0f)) + min(max(p.x, p.y), 0.0f) - radius;
}

float4 PS_Main(VS_OUTPUT input) :
	SV_TARGET
{
	// Convert UV to pixel coordinates
	float2 pixelPos = input.TexCoord * float2(WindowParams.y, WindowParams.z);

	// Get window bounds and corner radius
	float2 rectMin = WindowRect.xy;
	float2 rectMax = WindowRect.zw;
	float cornerRadius = WindowParams.x;

	float alpha = 1.0f;
	if (WindowParams.w < 0.5f) {
		// Calculate signed distance to rounded rectangle
		float sdf = RoundedRectSDF(pixelPos, rectMin, rectMax, cornerRadius);

		// Create smooth edge (anti-aliased)
		// Negative = inside, positive outside
		// Use 1.0 pixel transition for smooth edge
		alpha = saturate(-sdf);

		// Early out if completely outside
		if (alpha <= 0.0f) {
			discard;
		}
	}

	float2 blurTexelSize = DOWNSAMPLE_FACTOR / float2(WindowParams.y, WindowParams.z);

	// Sample with soft dithering to hide blocky pixels from the downsampled blur
	float4 blurColor = SampleWithSoftening(input.TexCoord, pixelPos, blurTexelSize);

	// Apply rounded corner mask to alpha
	// The blur strength is applied via blend state, so just use the rounded mask here
	blurColor.a = alpha;

	return blurColor;
}

// Clear shader entry point - outputs transparent black inside rounded rect only
// Used to clear UI buffer (HUD) in the exact same shape as the blur
float4 PS_Clear(VS_OUTPUT input) :
	SV_TARGET
{
	float2 pixelPos = input.TexCoord * float2(WindowParams.y, WindowParams.z);
	float sdf = RoundedRectSDF(pixelPos, WindowRect.xy, WindowRect.zw, WindowParams.x);

	// Discard pixels outside rounded rect to preserve HUD in corners
	clip(-sdf - CLIP_EPSILON);

	return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
