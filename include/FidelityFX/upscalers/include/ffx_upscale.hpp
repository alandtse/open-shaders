// This file is derived from the FidelityFX SDK.
//
// Source:
// https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/upscalers/include/ffx_upscale.hpp
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

#include "../../api/include/ffx_api.hpp"
#include "ffx_upscale.h"

namespace ffx
{

template<>
struct struct_type<ffxCreateContextDescUpscale> : std::integral_constant<uint64_t, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE>
{};

struct CreateContextDescUpscale : public InitHelper<ffxCreateContextDescUpscale>
{};

template<>
struct struct_type<ffxDispatchDescUpscale> : std::integral_constant<uint64_t, FFX_API_DISPATCH_DESC_TYPE_UPSCALE>
{};

struct DispatchDescUpscale : public InitHelper<ffxDispatchDescUpscale>
{};

template<>
struct struct_type<ffxConfigureDescUpscaleKeyValue> : std::integral_constant<uint64_t, FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE>
{};

struct ConfigureDescUpscaleKeyValue : public InitHelper<ffxConfigureDescUpscaleKeyValue>
{};

template<>
struct struct_type<ffxCreateContextDescUpscaleVersion> : std::integral_constant<uint64_t, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION>
{};

struct CreateContextDescUpscaleVersion : public InitHelper<ffxCreateContextDescUpscaleVersion>
{};

}
