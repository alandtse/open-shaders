cbuffer PeripheryTAAAuxClearCB : register(b0)
{
	uint2 OutputDim;
	uint2 OutputOffset;
	uint2 DispatchDim;
	uint2 Padding;
};

RWTexture2D<float2> OutVelocity : register(u0);
RWTexture2D<float> OutLock : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
	if (dispatchID.x >= DispatchDim.x || dispatchID.y >= DispatchDim.y)
		return;

	const uint2 outputPos = dispatchID.xy + OutputOffset;
	if (outputPos.x >= OutputDim.x || outputPos.y >= OutputDim.y)
		return;

	OutVelocity[outputPos] = 0.0.xx;
	OutLock[outputPos] = 0.0;
}
