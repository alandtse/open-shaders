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
		float Density;
		float TipWeight;
		float4 Wind;
		float WetStrength;
		float3 Inertia;
		float4 TouchEye[2];
		float TouchCount;
		float TouchStrength;
		float TouchDebug;
		float4 TouchA[12];
		float4 TouchB[12];
		float4 TouchMove[12];
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
	static const float TouchBend = 0.5;
	static const float TouchMarginScale = 0.25;
	static const float TouchMinMargin = 0.3;
	static const float TouchDrag = 0.8;
	static const float TouchMaxPush = 3.0;
	static const float TouchPress = 0.7;
	static const float TouchStableRadius = 0.5;
	static const float WindMaxBend = 0.6;
	static const float WindLeeShare = 0.25;
	static const float InertiaWetCling = 0.6;
	static const float WindInertiaMaxBend = 1.0;

	/**
	 * @brief World-space push of a shell layer away from, and along, the capsules touching the mesh.
	 * @details Each capsule bends fur within one fur length of its surface and pushes fur that is inside it back out. Capsule movement drags the fur along, so a stroke combs it.
	 *          The push grows with layer height squared so the root stays put, and is capped at TouchMaxPush. Near a capsule's axis, where the push direction is unstable, the sideways push fades out
	 *          and the layers flatten toward the root along the surface normal instead, so a deep press squashes the fur. Remembered dents arrive as capsules, flagged by TouchB.w, whose strength fades as the fur recovers.
	 * @param a_absolutePosition World position of the layer, including the camera offset.
	 * @param a_worldNormal World-space surface normal the layer grows along.
	 * @param a_length The fur's current length, wet fur included, which sets the width of the zone before contact.
	 * @param a_debug Receives the strongest live capsule influence in x, 1 in y when the layer is inside a capsule, and the strongest remembered dent's strength in z.
	 */
	float3 GetTouchDisplacement(float3 a_absolutePosition, float3 a_worldNormal, float a_shell, float a_length, out float3 a_debug)
	{
		float3 total = 0.0;
		float press = 0.0;
		a_debug = 0.0;
		float margin = max(a_length * TouchMarginScale, TouchMinMargin);
		[loop] for (uint index = 0; index < uint(TouchCount); ++index)
		{
			float3 pointA = TouchA[index].xyz;
			float3 axis = TouchB[index].xyz - pointA;
			float t = saturate(dot(a_absolutePosition - pointA, axis) / max(dot(axis, axis), 1e-4));
			float3 offset = a_absolutePosition - (pointA + axis * t);
			float distance = length(offset);
			float3 direction = offset / max(distance, 1e-4);
			float radius = TouchA[index].w;
			float strength = TouchMove[index].w;
			float reach = saturate((radius + margin - distance) / margin);
			float influence = reach * strength;
			float penetration = max(radius - distance, 0.0) * strength;
			bool remembered = TouchB[index].w > 0.5;
			a_debug = float3(remembered ? a_debug.x : max(a_debug.x, influence), remembered ? a_debug.y : max(a_debug.y, step(0.0, radius - distance)), remembered && reach > 0.0 ? max(a_debug.z, strength) : a_debug.z);
			float stable = saturate(distance / max(radius * TouchStableRadius, 1e-3));
			total += direction * ((influence * a_length * TouchBend + penetration) * stable) + TouchMove[index].xyz * (influence * TouchDrag);
			press = max(press, influence);
		}
		total *= TouchStrength;
		float magnitude = length(total);
		float3 push = total * (min(magnitude, TouchMaxPush) / max(magnitude, 1e-4)) * (a_shell * a_shell);
		return push - a_worldNormal * (a_length * a_shell * press * TouchPress * saturate(TouchStrength));
	}

	/**
	 * @brief World-space push of a shell layer by the wind sampled for the whole mesh.
	 * @details Bends quadratically with layer height, scales with wind speed squared, and is capped at WindMaxBend of the fur length so layers never separate.
	 *          Surfaces facing away from the wind keep WindLeeShare of the push.
	 */
	float3 GetWindDisplacement(float3 a_worldNormal, float a_shell)
	{
		float3 velocity = Wind.xyz;
		float speed = length(velocity);
		float3 direction = velocity / max(speed, 1e-4);
		float exposure = lerp(WindLeeShare, 1.0, saturate(-dot(a_worldNormal, direction)));
		float bend = saturate(speed * speed) * exposure * WindMaxBend;
		return direction * (bend * a_shell * a_shell * Length);
	}

	/** @brief World-space lag of the fur tips behind the mesh's motion, bending quadratically with layer height; wet fur clings and lags less. */
	float3 GetInertiaDisplacement(float a_shell, float a_wet)
	{
		return Inertia * (a_shell * a_shell * (1.0 - InertiaWetCling * saturate(a_wet)));
	}

	/** @brief Limits the combined wind and inertia push of a layer to WindInertiaMaxBend of the layer's own wet-shortened height in world units, so layers never separate. */
	float3 LimitBend(float3 a_bend, float a_shell, float a_wetLength, float a_worldScale)
	{
		float limit = WindInertiaMaxBend * Length * a_wetLength * a_worldScale * a_shell * a_shell;
		float bendLength = length(a_bend);
		return bendLength > limit ? a_bend * (limit / bendLength) : a_bend;
	}

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

	static const float WetClumpCells = 48.0;
	static const float WetClumpPull = 0.7;
	static const float WetDarkening = 0.5;
	static const float WetDroopGain = 1.5;
	static const float WetCollapse = 0.35;
	static const float TipShell = 0.999;

	/** @brief How wet the fur is, from the character's retained wetness, scaled by Wet Fur Strength. */
	float GetWetFactor(float a_retainedWetness, bool a_characterSurface)
	{
		return a_characterSurface ? saturate(a_retainedWetness) * WetStrength : 0.0;
	}

	/** @brief Centre of the clump a texture coordinate falls in; clumps are jittered cells of the shell texture. */
	float2 GetClumpCentre(float2 a_uv)
	{
		float2 cell = floor(a_uv * WetClumpCells);
		float2 hash = frac(sin(float2(dot(cell, float2(127.1, 311.7)), dot(cell, float2(269.5, 183.3)))) * 43758.5453);
		return (cell + lerp(0.2, 0.8, hash)) / WetClumpCells;
	}

	/** @brief Shell texture coordinate pulled toward its clump centre, more at the tip than at the root, so wet strands gather into wisps. */
	float2 GetClumpedUv(float2 a_uv, float a_shell, float a_wet)
	{
		return lerp(a_uv, GetClumpCentre(a_uv), saturate(a_wet * WetClumpPull * a_shell));
	}

	/** @brief Fur length scale of wet fur, which loses volume as it mats down. */
	float GetWetLength(float a_wet)
	{
		return 1.0 - WetCollapse * a_wet;
	}

	/** @brief Darkening of wet fur. */
	float GetWetDarkening(float a_wet)
	{
		return 1.0 - WetDarkening * a_wet;
	}

	/** @brief Droop scale of wet fur, which hangs heavier. */
	float GetWetDroop(float a_wet)
	{
		return 1.0 + WetDroopGain * a_wet;
	}

	static const float TouchDebugEdge = 0.12;

	static const float TouchMemoryHueSpan = 0.66;

	/**
	 * @brief Debug tint for touched fur: white inside a live capsule, magenta along the edge of the touched zone, cyan to yellow with growing influence inside it.
	 *        Fur still remembering a press is a heat map from red for a fresh press through green to blue as it recovers. The alpha is the tint strength.
	 */
	float4 GetTouchDebugColor(float3 a_debug)
	{
		if (a_debug.y > 0.5)
			return float4(1.0, 1.0, 1.0, 0.85);
		if (a_debug.z > 0.0) {
			float hue = (1.0 - saturate(a_debug.z)) * TouchMemoryHueSpan;
			return float4(saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0), 0.85);
		}
		if (a_debug.x <= 0.0)
			return 0.0;
		if (a_debug.x < TouchDebugEdge)
			return float4(1.0, 0.0, 1.0, 0.9);
		return float4(lerp(float3(0.0, 1.0, 1.0), float3(1.0, 1.0, 0.0), a_debug.x), 0.6);
	}

	float GetShell(uint shellIndex)
	{
		return saturate((ShellCount - float(shellIndex)) / max(ShellCount, 1.0));
	}

#if defined(PSHADER)
	/** @brief Strand survival weight: distance and size thinning for every layer, and the fractional share of the tip layer. */
	float GetLayerWeight(float shell)
	{
		return Density * (shell >= 0.999 ? TipWeight : 1.0);
	}
#endif

	float GetDarkening(float shell, float facing)
	{
		return lerp(1.0, lerp(RootDarkening, 1.0, shell), smoothstep(0.0, 0.5, facing));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
