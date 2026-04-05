// Pixel shader for rasterizing the OpenVR hidden area mesh.
// Outputs 1.0 into an R8_UNORM render target to mark hidden pixels.

float4 main() : SV_Target
{
	return float4(1.0, 0.0, 0.0, 0.0);
}
