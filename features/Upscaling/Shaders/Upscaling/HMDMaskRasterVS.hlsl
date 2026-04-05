// Vertex shader for rasterizing the OpenVR hidden area mesh into an R8 mask texture.
// Vertices are stored as a StructuredBuffer<float2> in NDC space (no input layout needed).
// The CPU converts OpenVR UV [0,1] -> NDC [-1,1] before uploading.

StructuredBuffer<float2> MeshVerts : register(t0);

float4 main(uint vertexId : SV_VertexID) : SV_POSITION
{
	return float4(MeshVerts[vertexId], 0.0, 1.0);
}
