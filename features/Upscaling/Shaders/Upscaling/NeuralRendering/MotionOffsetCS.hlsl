Texture2D<float2> MotionInput : register(t0);
RWTexture2D<float2> MotionOutput : register(u0);

cbuffer MotionOffset : register(b0)
{
	uint2 MotionSize;
	float2 MotionShift;
};

[numthreads(8, 8, 1)] void Offset(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= MotionSize))
		return;
	MotionOutput[id.xy] = MotionInput[id.xy] + MotionShift;
}
