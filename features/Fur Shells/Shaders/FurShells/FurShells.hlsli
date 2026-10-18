#ifndef __FUR_SHELLS_HLSLI__
#define __FUR_SHELLS_HLSLI__

namespace FurShells
{
	// VS and PS bind the same buffer at stage-specific slots; b13 is the VR constant buffer and the other slots are taken by Lighting.
#if defined(VSHADER)
	cbuffer FurShellData : register(b7)
#else
	cbuffer FurShellData : register(b10)
#endif
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float RootTest;
	};

#if defined(PSHADER)
	Texture2D<float4> TexShell : register(t124);
	Texture2D<float4> TexDepth : register(t125);

	bool IsRootCovered(float4 rootPosition, float rootSlope, uint eyeIndex)
	{
		float2 uv = rootPosition.xy / rootPosition.w * float2(0.5, -0.5) + 0.5;
		float sceneDepth = SharedData::GetScreenDepth(TexDepth.Load(SharedData::ConvertUVToSampleCoord(uv, eyeIndex)).x);
		float bias = 0.03 + rootSlope + 2e-8 * rootPosition.w * rootPosition.w;
		return RootTest > 0.0 && rootPosition.w > 0.0 && all(uv == saturate(uv)) && sceneDepth < rootPosition.w - bias;
	}

	static const float CoverageSpread = 0.75;
	static const float CoverageLod = 3.0;

	float GetCoverage(float alpha, float threshold, float2 uv)
	{
		uint width, height, levels;
		TexShell.GetDimensions(0, width, height, levels);
		float2 dx = ddx(uv) * float2(width, height);
		float2 dy = ddy(uv) * float2(width, height);
		float lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8)) + SharedData::MipBias;
		float spread = CoverageSpread * saturate(lod / CoverageLod) * alpha;
		return saturate((alpha + spread - threshold) / max(2.0 * spread, 1e-4));
	}
#endif

#if defined(VSHADER) && defined(MODELSPACENORMALS)
	Texture2D<float4> TexModelNormal : register(t126);

	float3 GetModelNormal(float2 uv)
	{
		uint width, height, levels;
		TexModelNormal.GetDimensions(0, width, height, levels);
		uint mip = min(uint(max(int(firstbithigh(max(width, height))), 9) - 9), max(levels, 1u) - 1u);
		uint2 size = max(uint2(width, height) >> mip, 1u);
		uint2 texel = min(uint2(frac(uv) * float2(size)), size - 1u);
		float3 normal = TexModelNormal.Load(int3(texel, mip)).xzy * 2.0 - 1.0;
		return normal * rsqrt(max(dot(normal, normal), 1e-6));
	}
#endif

#if defined(VSHADER)
	/** @brief Shell layer of an instance; VR packs both eyes into consecutive instances, so the layer is the pair index. */
	uint GetShellIndex(uint instanceID)
	{
#	if defined(VR)
		return instanceID >> (StereoEnabled != 0.0 ? 1u : 0u);
#	else
		return instanceID;
#	endif
	}
#endif

	float GetShell(uint shellIndex)
	{
		return saturate((ShellCount - float(shellIndex)) / max(ShellCount, 1.0));
	}

	float GetDarkening(float shell, float facing)
	{
		return lerp(1.0, lerp(RootDarkening, 1.0, shell), smoothstep(0.0, 0.5, facing));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
