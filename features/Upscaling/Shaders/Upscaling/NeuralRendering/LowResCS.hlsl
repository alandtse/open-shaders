#include "Common/NeuralRenderingFinite.hlsli"
#include "Upscaling/NeuralRendering/ColorTransfer.hlsli"
#ifdef NR_PERIPHERAL_COMPRESSION
#	include "Common/NeuralRenderingPeripheral.hlsli"
#	include "Common/ReverseZ.hlsli"
cbuffer PeripheralData : register(b2)
{
	NeuralRenderingPeripheral::Mapping Peripheral;
	uint4 PeripheralRegion;
};
#endif

Texture2D<float4> ProxyInput : register(t0);
Texture2D<float4> LowInput : register(t1);
Texture2D<float4> LowOutput : register(t2);
Texture2D<float> DepthInput : register(t3);
Texture2D<float2> MotionInput : register(t4);
SamplerState LinearClamp : register(s0);

RWTexture2D<float4> LowColor : register(u0);
RWTexture2D<float> LowDepth : register(u1);
RWTexture2D<float2> LowMotion : register(u2);
RWTexture2D<float4> ExpandedProxy : register(u3);

cbuffer LowRes : register(b1)
{
	uint LowWidth;
	uint LowHeight;
	uint GuideWidth;
	uint GuideHeight;
	float GuideSigma;
	uint Guided;
	uint2 LowResPad;
};

#ifdef NR_PERIPHERAL_COMPRESSION
void PreparePeripheral(uint2 pixel)
{
	const float2 grid = float2(LowWidth, LowHeight);
	const float2 sourceSize = float2(GuideWidth, GuideHeight);
	float2 lower, upper;
	NeuralRenderingPeripheral::SourceBounds(float2(pixel) / grid, (float2(pixel) + 1.0) / grid, Peripheral, lower, upper);
	lower = clamp(lower * sourceSize, 0.0, sourceSize);
	upper = clamp(upper * sourceSize, 0.0, sourceSize);
	bool2 empty = upper <= lower;
	lower = float2(empty.x ? min(lower.x, sourceSize.x - 1.0) : lower.x, empty.y ? min(lower.y, sourceSize.y - 1.0) : lower.y);
	upper = float2(empty.x ? lower.x + 1.0 : upper.x, empty.y ? lower.y + 1.0 : upper.y);
	const uint2 first = uint2(floor(lower)), end = uint2(ceil(upper));
	float4 color = 0.0;
	float area = 0.0, nearest = FrameBuffer::FarPlaneDepth();
	float2 motion = 0.0;
	bool foundDepth = false;
	[loop] for (uint y = first.y; y < end.y; ++y)
	{
		[loop] for (uint x = first.x; x < end.x; ++x)
		{
			const uint2 source = min(uint2(x, y), uint2(GuideWidth - 1, GuideHeight - 1));
			const float2 overlap = max(0.0, min(upper, float2(source + 1u)) - max(lower, float2(source)));
			const float weight = overlap.x * overlap.y;
			if (weight <= 0.0)
				continue;
			const float4 value = ProxyInput.Load(int3(source, 0));
			color += (all(NeuralRenderingFinite::IsFinite(value)) ? value : 0.0) * weight;
			area += weight;
			const float depth = DepthInput.Load(int3(source, 0));
			const float2 velocity = MotionInput.Load(int3(source, 0));
			if (NeuralRenderingFinite::IsFinite(depth) && depth >= 0.0 && depth <= 1.0 && all(NeuralRenderingFinite::IsFinite(velocity)) && (!foundDepth || FrameBuffer::IsNearerDepth(depth, nearest))) {
				nearest = depth;
				motion = velocity;
				foundDepth = true;
			}
		}
	}
	LowColor[pixel] = area > 0.0 ? color / area : 0.0;
	LowDepth[pixel] = nearest;
	const float2 anchor = NeuralRenderingPeripheral::Inverse((float2(pixel) + 0.5) / grid, Peripheral);
	LowMotion[pixel] = NeuralRenderingPeripheral::Motion(anchor, motion, Peripheral);
}
#endif

// The model sees an area-averaged proxy; depth and motion take the footprint's centre, since an average of either invents a surface.
[numthreads(8, 8, 1)] void Downsample(uint3 id : SV_DispatchThreadID) {
	if (id.x >= LowWidth || id.y >= LowHeight)
		return;
#ifdef NR_PERIPHERAL_COMPRESSION
	PreparePeripheral(id.xy);
#else
	const float2 uv = (float2(id.xy) + 0.5) / float2(LowWidth, LowHeight);
	const float2 quarter = 0.25 / float2(LowWidth, LowHeight);
	float4 sum = 0.0;
	[unroll] for (int i = 0; i < 4; ++i)
		sum += ProxyInput.SampleLevel(LinearClamp, uv + quarter * float2((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0), 0);
	LowColor[id.xy] = sum * 0.25;
	const int3 guide = int3(min(uint2(uv * float2(GuideWidth, GuideHeight)), uint2(GuideWidth - 1, GuideHeight - 1)), 0);
	LowDepth[id.xy] = DepthInput.Load(guide);
	LowMotion[id.xy] = MotionInput.Load(guide);
#endif
}

float LowLogLuma(float3 proxy)
{
	return log2(max(Color::RGBToLuminance(ProxySrgbToLinear(proxy), Luma), kLumaEpsilon));
}

// The composite takes only the model's luminance ratio, so the low-resolution gain is all that has to cross
// back: the full-resolution proxy scaled by it reads, to the composite, as a model frame of its own size.
[numthreads(8, 8, 1)] void ExpandGain(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
#ifdef NR_PERIPHERAL_COMPRESSION
	if (RegionWidth != 0 && (any(id.xy < uint2(RegionBaseX, RegionBaseY)) || any(id.xy >= uint2(RegionBaseX + RegionWidth, RegionBaseY + RegionHeight)))) {
		ExpandedProxy[id.xy] = ProxyInput[id.xy];
		return;
	}
#endif
	const float3 currentLinear = ProxySrgbToLinear(ProxyInput[id.xy].rgb);
	const float currentLogLuma = log2(max(Color::RGBToLuminance(currentLinear, Luma), kLumaEpsilon));
	float2 sourceUV = (float2(id.xy) + 0.5) / float2(Width, Height);
#ifdef NR_PERIPHERAL_COMPRESSION
	sourceUV = NeuralRenderingPeripheral::Forward(sourceUV, Peripheral);
#endif
	const float2 lowPosition = sourceUV * float2(LowWidth, LowHeight) - 0.5;
	const int2 base = int2(floor(lowPosition));
	const float2 fraction = lowPosition - float2(base);
#ifdef NR_PERIPHERAL_COMPRESSION
	const int2 start = int2(PeripheralRegion.xy), limit = int2(PeripheralRegion.xy + PeripheralRegion.zw - 1u);
#else
	const int2 start = int2(0, 0), limit = int2(LowWidth - 1, LowHeight - 1);
#endif
	float gainSum = 0.0, weightSum = 0.0;
#ifdef NR_PERIPHERAL_COMPRESSION
	bool valid = all(NeuralRenderingFinite::IsFinite(ProxyInput[id.xy]));
#endif
	[unroll] for (int i = 0; i < 4; ++i)
	{
		const int2 tap = clamp(base + int2(i & 1, i >> 1), start, limit);
		const float2 axis = float2((i & 1) ? fraction.x : 1.0 - fraction.x, (i & 2) ? fraction.y : 1.0 - fraction.y);
		const float inputLog = LowLogLuma(LowInput[tap].rgb);
		const float gain = LowLogLuma(LowOutput[tap].rgb) - inputLog;
#ifdef NR_PERIPHERAL_COMPRESSION
		valid = valid && all(NeuralRenderingFinite::IsFinite(LowInput[tap])) && all(NeuralRenderingFinite::IsFinite(LowOutput[tap])) && NeuralRenderingFinite::IsFinite(gain);
#endif
		const float guide = Guided != 0 ? exp(-abs(currentLogLuma - inputLog) / max(GuideSigma, 1e-3)) : 1.0;
		const float weight = axis.x * axis.y * (guide + 1e-3);
		gainSum += gain * weight;
		weightSum += weight;
	}
	const float3 expanded = ProxyLinearToSrgb(currentLinear * exp2(gainSum / max(weightSum, 1e-6)));
#ifdef NR_PERIPHERAL_COMPRESSION
	ExpandedProxy[id.xy] = valid && all(NeuralRenderingFinite::IsFinite(expanded)) ? float4(expanded, 1.0) : ProxyInput[id.xy];
#else
	ExpandedProxy[id.xy] = float4(expanded, 1.0);
#endif
}
