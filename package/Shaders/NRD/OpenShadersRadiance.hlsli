#ifndef NRD_PORT_RADIANCE_HLSLI
#define NRD_PORT_RADIANCE_HLSLI
#include "Common/Color.hlsli"
// Forward main is in the scene blending domain, while albedo and environment
// use the target's irradiance/working domain. No donor pre-exposure.
namespace NRDPort
{
	float3 SceneToLinear(float3 color)
	{
		return ENABLE_LL ? Color::SceneGammaToLinear(color) : Color::IrradianceToLinear(color);
	}
	float3 LinearToScene(float3 color) { return Color::SceneLinearToGamma(color); }

#if defined(NRD_PORT_RECONSTRUCT_AMBIENT)
	// Masks.z is the target's measured ambient luminance, not the donor's IL gain.
	// Reconstruct chroma from native lighting and match luminance before conversion.
	// The same estimate must be removed from the traced source and the final composite.
	float3 RemoveAmbient(float3 linearScene, uint2 pixel, float depth, float2 encodedNormal, float3 albedo, float ambientLuminance)
	{
		if (ambientLuminance <= 0.0 || abs(depth - FrameBuffer::FarPlaneDepth()) <= 1e-6)
			return linearScene;
		float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[NRDPortEye], float4(GBuffer::DecodeNormal(encodedNormal), 0)).xyz);
		float3 ambient = Color::Ambient(max(0, SharedData::GetAmbient(normalWS)));
#	if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
#		if defined(SKYLIGHTING)
			float2 uv = (float2(pixel) + 0.5) / float2(NRDPortActiveWidth, NRDPortActiveHeight);
			float4 pos = mul(FrameBuffer::CameraViewProjInverse[NRDPortEye], float4(2 * float2(uv.x, 1 - uv.y) - 1, depth, 1));
			float3 positionMS = pos.xyz / pos.w;
#			if defined(VR)
			positionMS += FrameBuffer::CameraPosAdjust[NRDPortEye].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#			endif
			float visibility = Skylighting::EvaluateDiffuse(Skylighting::Sample(positionMS, normalWS), normalWS, Skylighting::GetFadeOutFactor(positionMS));
			ambient = ImageBasedLighting::GetDiffuseIBLOccluded(ambient, -normalWS, visibility);
#		else
			ambient = ImageBasedLighting::GetDiffuseIBL(ambient, -normalWS);
#		endif
		}
#	endif
		ambient = max(0.0, ambient * albedo);
		const float estimateLuminance = Color::RGBToYCoCg(ambient).x;
		ambient = estimateLuminance > EPSILON_DIVISION ? ambient * (ambientLuminance / estimateLuminance) : ambientLuminance.xxx;
		return max(0.0, linearScene - Color::IrradianceToLinear(ambient));
	}
#endif
}
#endif
