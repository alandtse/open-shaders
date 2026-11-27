#include "NRD/Eye.hlsli"
Texture2D<float> PackedDepth : register(t31);
RWTexture2D<float> EyeDepth : register(u7);
[numthreads(8,8,1)] void main(uint2 pixel : SV_DispatchThreadID) {
 uint w,h; EyeDepth.GetDimensions(w,h);
 if(any(pixel >= uint2(w,h))) return;
 uint2 local = min(pixel,uint2(NRDPortActiveWidth,NRDPortActiveHeight)-1);
 EyeDepth[pixel]=PackedDepth[local+uint2(NRDPortOffsetX,0)];
}
