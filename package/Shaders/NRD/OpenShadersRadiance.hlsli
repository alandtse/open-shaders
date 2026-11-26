#ifndef NRD_PORT_RADIANCE_HLSLI
#define NRD_PORT_RADIANCE_HLSLI
#include "Common/Color.hlsli"
// Forward main is in the scene blending domain, while albedo and environment
// use the target's irradiance/working domain. No donor pre-exposure.
namespace NRDPort {
float3 SceneToLinear(float3 color) {
    return ENABLE_LL ? Color::SceneGammaToLinear(color) : Color::IrradianceToLinear(color);
}
float3 LinearToScene(float3 color) { return Color::SceneLinearToGamma(color); }
}
#endif
