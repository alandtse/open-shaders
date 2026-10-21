#include "Common/GBuffer.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<float>  srcWorkingDepth      : register(t0);
Texture2D<float4> srcNormalRoughness   : register(t1);
Texture2D<float4> srcDiffuseAlbedo     : register(t2);

// Raw current-frame SSGI radiance used as the NRC training target.
Texture2D<float4> srcTrainingRadiance  : register(t3);

struct RadianceCacheInput
{
    float3 position;
    float2 normal;
    float2 viewDir;
    float3 diffuseAlbedo;
    float  roughness;
};

struct RadianceCacheOutput
{
    float3 radiance;
};

RWStructuredBuffer<RadianceCacheInput>  outRCInput          : register(u0);
RWStructuredBuffer<RadianceCacheOutput> outRCTrainingTarget : register(u1);

static const float PI = 3.14159265358979323846;

// FidelityFX Radiance Cache spherical direction encoding.
float2 RCEncodeDirection(float3 v)
{
    v = normalize(v);

    float theta = acos(clamp(v.z, -1.0, 1.0)) / (PI * 0.5);
    theta = (theta < 1.0)
        ? sqrt(theta)
        : 2.0 - sqrt(2.0 - theta);

    return float2(
        theta * 0.5,
        atan2(v.y, v.x) / (2.0 * PI) + 0.5);
}

static const uint RC_CAPTURE_MAX_GRID_SIZE = 64u;

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint gridSize = clamp(RadianceCacheGridSize, 1u, RC_CAPTURE_MAX_GRID_SIZE);

    if (dtid.x >= gridSize || dtid.y >= gridSize)
        return;

    uint2 frameDim = max(uint2(1u, 1u), uint2(OUT_FRAME_DIM));

    // Deterministic screen-space query grid selected by the active NRC quality preset.
    // Each thread samples the center of one grid cell.
    float2 gridUV = (float2(dtid.xy) + 0.5) / float(gridSize);
    uint2 pixel = min(uint2(gridUV * float2(frameDim)), frameDim - 1u);

    float2 uv = (float2(pixel) + 0.5) * RCP_OUT_FRAME_DIM;
    uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
    float2 screenPos = Stereo::ConvertFromStereoUV(uv, eyeIndex);

    float depth = srcWorkingDepth.Load(uint3(pixel, 0));
    float3 viewPos = ScreenToViewPosition(screenPos, depth, eyeIndex);

    float4 nr = FULLRES_LOAD(
        srcNormalRoughness,
        pixel,
        uv * (FrameDim * RcpTexDim),
        samplerLinearClamp);

    float3 viewNormal = normalize(GBuffer::DecodeNormal(nr.xy));

    // Direction from the shaded point toward the camera.
    float3 viewDirection = normalize(-viewPos);

    // Transform current-frame geometry into Skyrim world space.
    float3 worldPos = ViewToWorldPosition(viewPos, InvViewMat[eyeIndex]);
    float3 worldNormal = normalize(ViewToWorldVector(viewNormal, InvViewMat[eyeIndex]));
    float3 worldViewDirection = normalize(ViewToWorldVector(viewDirection, InvViewMat[eyeIndex]));

    float3 albedo = saturate(FULLRES_LOAD(
        srcDiffuseAlbedo,
        pixel,
        uv * (FrameDim * RcpTexDim),
        samplerLinearClamp).rgb);

    RadianceCacheInput result;
    // Normalize world-space positions against the active NRC domain.
    // Do not clamp: out-of-volume samples must remain detectable.
    float3 rcLocalPos = worldPos - RadianceCacheVolumeCenter.xyz;
    result.position = rcLocalPos / (2.0 * RadianceCacheVolumeExtent.xyz) + 0.5;

    result.normal = RCEncodeDirection(worldNormal);
    result.viewDir = RCEncodeDirection(worldViewDirection);
    result.diffuseAlbedo = albedo;
    result.roughness = saturate(1.0 - nr.z);

    uint outputIndex = dtid.y * gridSize + dtid.x;
    outRCInput[outputIndex] = result;

    // Same query pixel and same physical capture index as the NRC input.
    // texRRSpecularInput.a is RR hit-distance metadata, so only RGB is supervised.
    RadianceCacheOutput trainingTarget;
    trainingTarget.radiance = srcTrainingRadiance.Load(uint3(pixel, 0)).rgb;
    outRCTrainingTarget[outputIndex] = trainingTarget;
}
