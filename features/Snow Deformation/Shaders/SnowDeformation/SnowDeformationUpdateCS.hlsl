#include "SnowDeformation/UpdateParams.hlsli"
#include "SnowDeformation/Coverage.hlsli"

#define STAMP_WORDS 3
#define FLAG_FRESH 1
#define FLAG_CLEAR 2
#define FLAG_SEED 4

StructuredBuffer<int4> TileList : register(t3);
Texture2D<float> BlanketRaise : register(t4);
Texture2D<float> FieldBefore : register(t5);
Texture2D<float> CoarseField : register(t6);
SamplerState FieldSampler : register(s1);

RWTexture2D<float> Field : register(u0);

float BeforeAt(int2 texel)
{
	return FieldBefore.Load(int3(texel & (FIELD_TEXELS - 1), 0));
}

float RaiseAt(float2 worldPosition)
{
	const float2 cell = worldPosition * (1.0 / BLANKET_CELL);
	const int2 inside = (int2)floor(cell) - BlanketCell.xy;
	float raise = 0.0;
	[branch] if (all(inside >= 0) && all(inside < BLANKET_TEXELS - 1))
		raise = BlanketRaise.SampleLevel(FieldSampler, (cell + 0.5) * (1.0 / BLANKET_TEXELS), 0);
	return raise;
}

groupshared uint gsHits[STAMP_WORDS];

uint HashCell(int2 cell)
{
	uint2 q = uint2(cell) * uint2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	n ^= n >> 15;
	return n * 2246822519u;
}

float ValueNoise(float2 position)
{
	float2 base = floor(position);
	float2 f = position - base;
	f = f * f * (3.0 - 2.0 * f);
	int2 cell = int2(base);
	float a = HashCell(cell) * (1.0 / 4294967296.0);
	float b = HashCell(cell + int2(1, 0)) * (1.0 / 4294967296.0);
	float c = HashCell(cell + int2(0, 1)) * (1.0 / 4294967296.0);
	float d = HashCell(cell + int2(1, 1)) * (1.0 / 4294967296.0);
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float FractalNoise(float2 worldPosition, float wavelength)
{
	const float2x2 turn = float2x2(0.7986, -0.6018, 0.6018, 0.7986);
	float2 position = worldPosition / wavelength;
	float noise = ValueNoise(position);
	position = mul(turn, position) * 2.03;
	noise += 0.5 * ValueNoise(position);
	return noise * (2.0 / 1.5) - 1.0;
}

float ChurnNoise(float2 worldPosition)
{
	float noise = FractalNoise(worldPosition, 12.0);
	noise *= abs(noise);
	return noise * (0.65 + 0.35 * FractalNoise(worldPosition, 70.0));
}

float2 SweptDelta(float2 position, float2 centre, float2 motion)
{
	float2 delta = position - centre;
	float lengthSquared = dot(motion, motion);
	float t = lengthSquared > 1e-4 ? saturate(dot(delta + motion, motion) / max(lengthSquared, 1e-4)) : 1.0;
	return delta + motion * (1.0 - t);
}

float StampDistance(float2 delta, float4 stamp, float4 shape)
{
	float halfLength = max(stamp.z, 1e-3);
	float2 forward = shape.z > 0.0 ? normalize(shape.xy) : float2(1.0, 0.0);
	float2 right = float2(-forward.y, forward.x);
	float halfWidth = shape.z > 0.0 ? max(shape.z, 1e-3) : halfLength;
	float2 normalized = float2(dot(delta, forward) / halfLength, dot(delta, right) / halfWidth);
	return length(normalized) * halfLength;
}

[numthreads(8, 8, 1)] void main(uint3 groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex) {
	const int4 entry = TileList[groupID.y];
	const uint2 slot = uint2(entry.x & 0xFF, (entry.x >> 8) & 0xFF);
	const uint flags = ((uint)entry.x >> 16) & 0xFF;
	const bool clear = (flags & FLAG_CLEAR) != 0;
	const bool fresh = (flags & FLAG_FRESH) != 0;
	const uint2 block = uint2(groupID.x & 7, groupID.x >> 3);
	const uint2 inTile = block * 8 + groupThreadID.xy;
	const uint2 texel = slot * TILE_TEXELS + inTile;

	const int2 tileCell = entry.yz * TILE_TEXELS;
	const int2 cell = tileCell + int2(inTile);
	const float cellSize = Control.z;
	const float2 windowPosition = float2(cell - OriginCell.xy) * cellSize;
	const float2 worldPosition = float2(cell) * cellSize;

	if (groupIndex < STAMP_WORDS)
		gsHits[groupIndex] = 0;
	GroupMemoryBarrierWithGroupSync();

	const float2 groupLow = float2(tileCell + int2(block * 8) - OriginCell.xy - 1) * cellSize;
	const float2 groupHigh = groupLow + 10.0 * cellSize;
	const uint count = clear ? 0 : min((uint)Control.y, (uint)MAX_STAMPS);
	for (uint candidate = groupIndex; candidate < count; candidate += 64) {
		const float4 bounds = StampBounds[candidate];
		if (all(groupHigh >= bounds.xy) && all(groupLow <= bounds.zw))
			InterlockedOr(gsHits[candidate >> 5], 1u << (candidate & 31));
	}
	GroupMemoryBarrierWithGroupSync();

	float height = 0.0;
	[branch] if (!clear)
	{
		[branch] if (!fresh)
			height = BeforeAt(int2(texel)) * Control.x;
		else if ((flags & FLAG_SEED) != 0 && Options.z > 0.5)
		{
			const float2 coarseTexel = ((windowPosition + Coarse.xy) * Coarse.z + float2(OriginCell.zw) + 0.5) * (1.0 / FIELD_TEXELS);
			height = CoarseField.SampleLevel(FieldSampler, coarseTexel, 0);
		}
	}

	float target = 0.0;
	float snowCover = -1.0;
	float raise = -1.0;
	float churn = 0.0;
	float rimNoise = 0.0;
	float melt = 0.0;
	float meltFloor = 0.0;
	bool noiseReady = false;

	uint word = 0;
	uint hits = gsHits[0];
	[loop] for (;;)
	{
		[loop] while (hits == 0 && word + 1 < STAMP_WORDS)
			hits = gsHits[++word];
		if (hits == 0)
			break;
		const uint i = word * 32 + firstbitlow(hits);
		hits &= hits - 1;

		const float4 bounds = StampBounds[i];
		if (any(windowPosition < bounds.xy) || any(windowPosition > bounds.zw))
			continue;

		const float4 stamp = Stamps[i];
		const float4 shape = StampShape[i];
		const float4 motion = StampMotion[i];
		const float4 surface = StampSurface[i];

		const float halfLength = max(stamp.z, 1e-3);
		const float distance = StampDistance(SweptDelta(windowPosition, stamp.xy, motion.xy), stamp, shape);
		const float press = 1.0 - smoothstep(halfLength * Control.w, halfLength, distance);

		[branch] if (surface.z > 0.5)
		{
			const float meltHere = stamp.w * press;
			if (meltHere > melt) {
				melt = meltHere;
				meltFloor = shape.w * press;
			}
			continue;
		}

		float rim = 0.0;
		const float rimSpan = halfLength * Shape.x;
		[branch] if (shape.w > 0.0 && rimSpan > 0.0)
		{
			const float u = (distance - halfLength) / (distance >= halfLength ? 2.0 * rimSpan : 0.5 * rimSpan);
			const float bump = saturate(1.0 - u * u);
			rim = shape.w * bump * bump;
		}

		if (press <= 0.0 && rim <= 0.0)
			continue;

		if (!noiseReady) {
			noiseReady = true;
			churn = Shape.z > 0.0 ? ChurnNoise(worldPosition) : 0.0;
			rimNoise = Shape.y > 0.0 ? FractalNoise(worldPosition, 9.0) : 0.0;
		}
		if (snowCover < 0.0)
			snowCover = SnowCoverAt(worldPosition, windowPosition, surface.x, surface.y);

		const float snow = max(motion.z, snowCover);
		float carve = motion.w;
		[branch] if (surface.w > 0.5)
		{
			if (raise < 0.0)
				raise = RaiseAt(worldPosition);
			carve = max(raise - (surface.w - 1.0), 0.0);
		}
		const float profile = (rim * max(1.0 + Shape.y * rimNoise, 0.0) - stamp.w * press * max(1.0 + Shape.z * churn, 0.0)) * snow - carve * press;
		if (abs(profile) > abs(target))
			target = profile;
	}

	if (target < 0.0 && target < height)
		height = target;
	else if (target > 0.0 && target > height && height >= 0.0)
		height = target;

	[branch] if (melt > 0.0 && Coarse.w > 0.0)
	{
		const float remains = pow(max(1.0 - melt, 0.0), Coarse.w);
		height = meltFloor + (height - meltFloor) * remains;
	}

	[branch] if (!clear && !fresh && target == 0.0 && Options.w > 0.0)
	{
		const float maxStep = cellSize * Shape.w;
		const float n0 = BeforeAt(int2(texel) + int2(-1, 0));
		const float n1 = BeforeAt(int2(texel) + int2(1, 0));
		const float n2 = BeforeAt(int2(texel) + int2(0, -1));
		const float n3 = BeforeAt(int2(texel) + int2(0, 1));
		float pull = sign(n0 - height) * max(abs(n0 - height) - maxStep, 0.0);
		pull += sign(n1 - height) * max(abs(n1 - height) - maxStep, 0.0);
		pull += sign(n2 - height) * max(abs(n2 - height) - maxStep, 0.0);
		pull += sign(n3 - height) * max(abs(n3 - height) - maxStep, 0.0);
		height += pull * 0.25 * Options.w;
	}

	Field[texel] = clear ? 0.0 : height;
}
