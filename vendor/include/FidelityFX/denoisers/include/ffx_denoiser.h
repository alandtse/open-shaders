#pragma once
// Minimal FidelityFX Denoiser 1.2 ABI declarations used by Open Shaders.
// Layout and descriptor IDs follow AMD FidelityFX SDK 2.3.0 ffx_denoiser.h.
#include "../../api/include/ffx_api.h"
#include "../../api/include/ffx_api_types.h"
#include <stdint.h>

#define FFX_DENOISER_VERSION_MAJOR 1
#define FFX_DENOISER_VERSION_MINOR 2
#define FFX_DENOISER_VERSION_PATCH 0
#define FFX_DENOISER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_DENOISER_VERSION FFX_DENOISER_MAKE_VERSION(1, 2, 0)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x01)
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x41)
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x48)

typedef enum FfxApiDenoiserSignalFlags {
    FFX_DENOISER_SIGNAL_NONE = 0,
    FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR = (1 << 5),
} FfxApiDenoiserSignalFlags;
typedef enum FfxApiDispatchDenoiserFlags {
    FFX_DENOISER_DISPATCH_RESET = (1 << 0),
    FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO = (1 << 1),
} FfxApiDispatchDenoiserFlags;

typedef struct ffxCreateContextDescDenoiser {
    ffxCreateContextDescHeader header;
    uint32_t version;
    struct FfxApiDimensions2D maxRenderSize;
    uint32_t signalFlags;
    uint32_t checkerboardSignalFlags;
    uint32_t flags;
} ffxCreateContextDescDenoiser;

typedef struct FfxApiDenoiserSignal {
    struct FfxApiResource input;
    struct FfxApiResource output;
    uint32_t checkerboardOrigin;
} FfxApiDenoiserSignal;

typedef struct ffxDispatchDescDenoiser {
    ffxDispatchDescHeader header;
    void* commandList;
    struct FfxApiResource linearDepth;
    struct FfxApiResource motionVectors;
    struct FfxApiResource normals;
    struct FfxApiResource specularAlbedo;
    struct FfxApiResource diffuseAlbedo;
    struct FfxApiFloatCoords3D motionVectorScale;
    struct FfxApiFloatCoords2D jitterOffsets;
    struct FfxApiFloatCoords3D cameraPositionDelta;
    FfxApiMatrix4x4 view;
    FfxApiMatrix4x4 projection;
    FfxApiFloatBounds linearDepthBounds;
    struct FfxApiDimensions2D renderSize;
    uint32_t frameIndex;
    uint32_t flags;
} ffxDispatchDescDenoiser;

typedef struct ffxDispatchDescDenoiserIndirectSpecular {
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserIndirectSpecular;
