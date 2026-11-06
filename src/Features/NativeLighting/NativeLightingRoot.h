#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d12.h>

namespace NativeLighting
{
	inline constexpr uint32_t kMaximumInstances = 8192;
	inline constexpr uint32_t kMaximumTexturePools = 256;

	/** @brief Creates the direct-lighting root signature shared by the runtime and validation. */
	inline HRESULT CreateRootSignature(ID3D12Device* a_device, ID3D12RootSignature** a_root)
	{
		if (!a_device || !a_root) return E_INVALIDARG;
		D3D12_DESCRIPTOR_RANGE ranges[6]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumInstances, 0, 1, 0 };
		ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumInstances, 0, 2, 0 };
		ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaximumTexturePools, 0, 3, 0 };
		ranges[4] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 0, 0 };
		ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, 0 };
		D3D12_ROOT_PARAMETER params[12]{};
		constexpr std::array rootSrvRegisters{ 0u, 2u, 3u, 4u, 5u };
		constexpr std::array rootSrvIndices{ 0u, 2u, 3u, 5u, 6u };
		for (size_t i = 0; i < rootSrvIndices.size(); ++i) {
			params[rootSrvIndices[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			params[rootSrvIndices[i]].Descriptor.ShaderRegister = rootSrvRegisters[i];
		}
		constexpr std::array tableIndices{ 1u, 4u, 7u, 8u, 9u, 10u };
		for (size_t i = 0; i < tableIndices.size(); ++i) {
			params[tableIndices[i]].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[tableIndices[i]].DescriptorTable = { 1, &ranges[i] };
		}
		params[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[11].Descriptor.ShaderRegister = 0;
		D3D12_STATIC_SAMPLER_DESC samplers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[i].AddressU = (i & 2) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressV = (i & 1) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samplers[i].MaxAnisotropy = 1;
			samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			samplers[i].ShaderRegister = i;
		}
		const D3D12_ROOT_SIGNATURE_DESC desc{ uint32_t(std::size(params)), params, 4, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ID3DBlob* serialized = nullptr;
		ID3DBlob* errors = nullptr;
		auto result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
		if (errors) errors->Release();
		if (FAILED(result)) { if (serialized) serialized->Release(); return result; }
		result = a_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(a_root));
		serialized->Release();
		return result;
	}
}
