// This file is derived from the FidelityFX SDK.
//
// Source:
// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/upscalers/include/ffx_upscale.h
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include "../../api/include/ffx_api.h"
#include "../../api/include/ffx_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compatibility glue for the older local ffx_api.h snapshot used by this tree.
#ifndef FFX_API_EFFECT_ID_UPSCALE
#	define FFX_API_EFFECT_ID_UPSCALE 0x00010000u
#endif

#ifndef FFX_API_MAKE_EFFECT_SUB_ID
#	define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion) ((effectId & FFX_API_EFFECT_MASK) | (subversion & ~FFX_API_EFFECT_MASK))
#endif

#define FFX_UPSCALER_VERSION_MAJOR 4
#define FFX_UPSCALER_VERSION_MINOR 1
#define FFX_UPSCALER_VERSION_PATCH 0

#define FFX_UPSCALER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_UPSCALER_VERSION FFX_UPSCALER_MAKE_VERSION(FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH)

enum FfxApiCreateContextUpscaleFlags
{
	FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = (1 << 0),
	FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1 << 1),
	FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),
	FFX_UPSCALE_ENABLE_DEPTH_INVERTED = (1 << 3),
	FFX_UPSCALE_ENABLE_DEPTH_INFINITE = (1 << 4),
	FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = (1 << 5),
	FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION = (1 << 6),
	FFX_UPSCALE_ENABLE_DEBUG_CHECKING = (1 << 7),
	FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE = (1 << 8),
	FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION = (1 << 9),
};

enum FfxApiDispatchFsrUpscaleFlags
{
	FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW = (1 << 0),
	FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = (1 << 1),
	FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ = (1 << 2),
};

enum FfxApiConfigureUpscaleKey
{
	FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR = 0,
	FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE = 1,
	FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE = 2,
	FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME = 3,
	FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION = 4,
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)
struct ffxCreateContextDescUpscale
{
	ffxCreateContextDescHeader header;
	uint32_t flags;
	struct FfxApiDimensions2D maxRenderSize;
	struct FfxApiDimensions2D maxUpscaleSize;
	ffxApiMessage fpMessage;
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)
struct ffxDispatchDescUpscale
{
	ffxDispatchDescHeader header;
	void* commandList;
	struct FfxApiResource color;
	struct FfxApiResource depth;
	struct FfxApiResource motionVectors;
	struct FfxApiResource exposure;
	struct FfxApiResource reactive;
	struct FfxApiResource transparencyAndComposition;
	struct FfxApiResource output;
	struct FfxApiFloatCoords2D jitterOffset;
	struct FfxApiFloatCoords2D motionVectorScale;
	struct FfxApiDimensions2D renderSize;
	struct FfxApiDimensions2D upscaleSize;
	bool enableSharpening;
	float sharpness;
	float frameTimeDelta;
	float preExposure;
	bool reset;
	float cameraNear;
	float cameraFar;
	float cameraFovAngleVertical;
	float viewSpaceToMetersFactor;
	uint32_t flags;
};

#define FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x07)
struct ffxConfigureDescUpscaleKeyValue
{
	ffxConfigureDescHeader header;
	uint64_t key;
	uint64_t u64;
	void* ptr;
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)
struct ffxCreateContextDescUpscaleVersion
{
	ffxCreateContextDescHeader header;
	uint32_t version;
};

#ifdef __cplusplus
}
#endif
