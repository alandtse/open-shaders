#include "Common/GBuffer.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<float>  srcWorkingDepth    : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float4> srcMotionVectors   : register(t2);
Texture2D<float4> srcSpecularAlbedo  : register(t3);  // Open Shaders REFLECTANCE: split-sum specular lobe weight
Texture2D<float4> srcDiffuseAlbedo   : register(t4);
Texture2D<float>  srcPrevLinearDepth : register(t5);

RWTexture2D<float>  outLinearDepth     : register(u0);
RWTexture2D<float4> outMotionVectors    : register(u1);
RWTexture2D<float4> outNormalRoughness  : register(u2);
RWTexture2D<float4> outSpecularAlbedo   : register(u3);
RWTexture2D<float4> outDiffuseAlbedo    : register(u4);

[numthreads(8, 8, 1)]
void main(uint2 dtid : SV_DispatchThreadID)
{
    if (any(dtid >= uint2(OUT_FRAME_DIM)))
        return;

    const float2 frameScale = FrameDim * RcpTexDim;
    const float2 uv = (dtid + 0.5) * RCP_OUT_FRAME_DIM;

    // SSGI working depth is already signed view-space linear depth.
    const float depth = srcWorkingDepth.Load(uint3(dtid, 0));
    outLinearDepth[dtid] = depth;

    // Skyrim/Open Shaders motion XY is PreviousUV - CurrentUV, matching RR.
    // Reproject into the previous RR-depth history to obtain the third component.
    const float2 mv = FULLRES_LOAD(srcMotionVectors, dtid, uv * frameScale, samplerLinearClamp).xy;
    const float2 prevUv = uv + mv;
    float prevDepth = depth;
    if (RRHistoryValid != 0 && all(prevUv >= 0.0) && all(prevUv <= 1.0))
        prevDepth = srcPrevLinearDepth.SampleLevel(samplerLinearClamp, prevUv * OUT_FRAME_SCALE, 0);
    outMotionVectors[dtid] = float4(mv, prevDepth - depth, 0.0);

    const float4 nr = FULLRES_LOAD(srcNormalRoughness, dtid, uv * frameScale, samplerLinearClamp);

    // Open Shaders deliberately uses the opposite octahedral sign convention from
    // AMD RR (GBuffer::EncodeNormal starts by negating N).  Decode to the actual
    // view-space normal, rotate it to world space like AMD's Denoiser sample, then
    // encode with RR's standard octahedral convention.  Passing nr.xy through
    // directly would give RR an inverted, view-space normal.
    float3 rrNormal = normalize(ViewToWorldVector(GBuffer::DecodeNormal(nr.xy), FrameBuffer::CameraViewInverse[Stereo::GetEyeIndexFromTexCoord(uv)]));
    rrNormal /= max(abs(rrNormal.x) + abs(rrNormal.y) + abs(rrNormal.z), 1e-6);
    float2 rrNormalOct = rrNormal.xy;
    if (rrNormal.z < 0.0)
        rrNormalOct = (1.0 - abs(rrNormalOct.yx)) * (rrNormalOct.xy >= 0.0 ? 1.0 : -1.0);
    rrNormalOct = rrNormalOct * 0.5 + 0.5;

    // nr.z is glossiness, so 1-z is linear/perceptual roughness. Material type 0
    // is AMD's recommended starting value for integrations without a dedicated ID.
    outNormalRoughness[dtid] = float4(rrNormalOct, saturate(1.0 - nr.z), 0.0);

    // REFLECTANCE is already the quantity AMD calls specular albedo: Open Shaders
    // builds it as F0 * EnvBRDF.x + EnvBRDF.y (plus applicable coating/wetness
    // lobes). ALBEDO is the closest persistent diffuse-albedo feature available.
    // Both are linear, so the dispatch sets FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO.
    outSpecularAlbedo[dtid] = float4(saturate(FULLRES_LOAD(srcSpecularAlbedo, dtid, uv * frameScale, samplerLinearClamp).rgb), 1.0);
    outDiffuseAlbedo[dtid]  = float4(saturate(FULLRES_LOAD(srcDiffuseAlbedo, dtid, uv * frameScale, samplerLinearClamp).rgb), 1.0);
}
