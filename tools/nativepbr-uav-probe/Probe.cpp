#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr UINT kSize = 4;
	constexpr UINT kUavSlot = 8;
	constexpr uint32_t kExpectedIdentity = 0x13572468;
	constexpr std::array<DXGI_FORMAT, 8> kMrtFormats{
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_R16G16_FLOAT,
		DXGI_FORMAT_R10G10B10A2_UNORM,
		DXGI_FORMAT_R10G10B10A2_UNORM,
		DXGI_FORMAT_R11G11B10_FLOAT,
		DXGI_FORMAT_R11G11B10_FLOAT,
		DXGI_FORMAT_R11G11B10_FLOAT,
		DXGI_FORMAT_R16G16_UNORM
	};

	constexpr char kShader[] = R"(
cbuffer ProbeConstants : register(b0)
{
    uint DrawIdentity;
    float VertexDepth;
    float2 Padding;
};
RWTexture2D<uint> IdentityOutput : register(u8);

struct VSOutput { float4 position : SV_Position; };
VSOutput vs(uint vertex : SV_VertexID)
{
    float2 points[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOutput result;
    result.position = float4(points[vertex], VertexDepth, 1);
    return result;
}
struct PSOutput
{
    float4 main : SV_Target0;
    float4 motion : SV_Target1;
    float4 normal : SV_Target2;
    float4 albedo : SV_Target3;
    float4 specular : SV_Target4;
    float4 reflectance : SV_Target5;
    float4 masks : SV_Target6;
    float4 masks2 : SV_Target7;
};
PSOutput ps(VSOutput input)
{
    IdentityOutput[uint2(input.position.xy)] = DrawIdentity;
    PSOutput result;
    result.main = float4(0.25, 0.5, 0.75, 1);
    result.motion = float4(0.125, 0.5, 0, 1);
    result.normal = float4(0.5, 0.5, 1, 1);
    result.albedo = float4(0.25, 0.5, 0.75, 1);
    result.specular = float4(0.1, 0.2, 0.3, 1);
    result.reflectance = float4(0.1, 0.2, 0.3, 1);
    result.masks = float4(0.0625, 0.125, 0.25, 1);
    result.masks2 = float4(0.25, 0.5, 0, 1);
    return result;
}
)";

	bool Report(const char* a_label, HRESULT a_result)
	{
		std::printf("%-29s %s (0x%08X)\n", a_label, SUCCEEDED(a_result) ? "PASS" : "FAIL",
			static_cast<unsigned int>(a_result));
		return SUCCEEDED(a_result);
	}

	bool Compile(const char* a_entry, const char* a_profile, ComPtr<ID3DBlob>& a_shader)
	{
		ComPtr<ID3DBlob> errors;
		const HRESULT result = D3DCompile(kShader, std::strlen(kShader), "NativePBRUavProbe", nullptr, nullptr,
			a_entry, a_profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, a_shader.GetAddressOf(), errors.GetAddressOf());
		if (errors)
			std::printf("%s: %.*s\n", a_entry, static_cast<int>(errors->GetBufferSize()),
				static_cast<const char*>(errors->GetBufferPointer()));
		return Report(a_profile, result);
	}

	struct Constants
	{
		uint32_t identity;
		float depth;
		float padding[2];
	};

	int Run()
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		D3D_FEATURE_LEVEL level{};
		constexpr D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			levels, 2, D3D11_SDK_VERSION, device.GetAddressOf(), &level, context.GetAddressOf());
		if (result == E_INVALIDARG)
			result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
				levels + 1, 1, D3D11_SDK_VERSION, device.GetAddressOf(), &level, context.GetAddressOf());
		if (!Report("D3D11 hardware device", result))
			return 1;
		std::printf("Feature level: 0x%04X\n", static_cast<unsigned int>(level));
		ComPtr<IDXGIDevice> dxgiDevice;
		if (SUCCEEDED(device.As(&dxgiDevice))) {
			ComPtr<IDXGIAdapter> adapter;
			if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
				DXGI_ADAPTER_DESC description{};
				if (SUCCEEDED(adapter->GetDesc(&description)))
					std::printf("Adapter: %ls (vendor %04X, device %04X)\n", description.Description,
						static_cast<unsigned int>(description.VendorId), static_cast<unsigned int>(description.DeviceId));
			}
		}
		ComPtr<ID3D11Device1> device1;
		const bool runtime11_1 = SUCCEEDED(device.As(&device1));
		std::printf("D3D11.1 device interface: %s\n", runtime11_1 ? "YES" : "NO");
		ComPtr<ID3D11DeviceContext1> context1;
		std::printf("D3D11.1 context interface: %s\n", SUCCEEDED(context.As(&context1)) ? "YES" : "NO");
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		Report("D3D11 options query", device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)));
		UINT uavFormatSupport = 0;
		if (Report("R32_UINT format query", device->CheckFormatSupport(DXGI_FORMAT_R32_UINT, &uavFormatSupport)))
			std::printf("R32_UINT typed UAV support: %s\n",
				(uavFormatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0 ? "YES" : "NO");

		ComPtr<ID3DBlob> vsBlob, psBlob;
		if (!Compile("vs", "vs_5_0", vsBlob) || !Compile("ps", "ps_5_0", psBlob))
			return 2;
		ComPtr<ID3D11VertexShader> vs;
		ComPtr<ID3D11PixelShader> ps;
		if (!Report("Create vertex shader", device->CreateVertexShader(vsBlob->GetBufferPointer(),
			vsBlob->GetBufferSize(), nullptr, vs.GetAddressOf())) ||
			!Report("Create pixel shader", device->CreatePixelShader(psBlob->GetBufferPointer(),
				psBlob->GetBufferSize(), nullptr, ps.GetAddressOf())))
			return 3;

		std::array<ComPtr<ID3D11Texture2D>, 8> textures;
		std::array<ComPtr<ID3D11RenderTargetView>, 8> rtvs;
		std::array<ID3D11RenderTargetView*, 8> rtvRaw{};
		D3D11_TEXTURE2D_DESC tex{};
		tex.Width = tex.Height = kSize;
		tex.MipLevels = tex.ArraySize = 1;
		tex.SampleDesc.Count = 1;
		tex.Usage = D3D11_USAGE_DEFAULT;
		tex.BindFlags = D3D11_BIND_RENDER_TARGET;
		for (size_t index = 0; index < rtvs.size(); ++index) {
			tex.Format = kMrtFormats[index];
			if (!Report("Create MRT texture", device->CreateTexture2D(&tex, nullptr, textures[index].GetAddressOf())) ||
				!Report("Create MRT view", device->CreateRenderTargetView(textures[index].Get(), nullptr, rtvs[index].GetAddressOf())))
				return 4;
			rtvRaw[index] = rtvs[index].Get();
		}
		tex.Format = DXGI_FORMAT_R32_UINT;
		tex.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		ComPtr<ID3D11Texture2D> identity;
		ComPtr<ID3D11UnorderedAccessView> uav;
		if (!Report("Create identity texture", device->CreateTexture2D(&tex, nullptr, identity.GetAddressOf())) ||
			!Report("Create identity UAV", device->CreateUnorderedAccessView(identity.Get(), nullptr, uav.GetAddressOf())))
			return 5;
		tex.Usage = D3D11_USAGE_STAGING;
		tex.BindFlags = 0;
		tex.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> readback;
		if (!Report("Create readback texture", device->CreateTexture2D(&tex, nullptr, readback.GetAddressOf())))
			return 6;
		std::array<ComPtr<ID3D11Texture2D>, 8> mrtReadbacks;
		for (size_t index = 0; index < mrtReadbacks.size(); ++index) {
			tex.Format = kMrtFormats[index];
			if (!Report("Create MRT readback", device->CreateTexture2D(&tex, nullptr, mrtReadbacks[index].GetAddressOf())))
				return 6;
		}

		D3D11_TEXTURE2D_DESC depthDesc{};
		depthDesc.Width = depthDesc.Height = kSize;
		depthDesc.MipLevels = depthDesc.ArraySize = 1;
		depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		ComPtr<ID3D11Texture2D> depth;
		ComPtr<ID3D11DepthStencilView> dsv;
		if (!Report("Create depth texture", device->CreateTexture2D(&depthDesc, nullptr, depth.GetAddressOf())) ||
			!Report("Create depth view", device->CreateDepthStencilView(depth.Get(), nullptr, dsv.GetAddressOf())))
			return 7;
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		ComPtr<ID3D11DepthStencilState> ds;
		if (!Report("Create depth state", device->CreateDepthStencilState(&dsDesc, ds.GetAddressOf())))
			return 8;

		D3D11_BUFFER_DESC constantDesc{};
		constantDesc.ByteWidth = sizeof(Constants);
		constantDesc.Usage = D3D11_USAGE_DEFAULT;
		constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> constants;
		if (!Report("Create constants", device->CreateBuffer(&constantDesc, nullptr, constants.GetAddressOf())))
			return 9;

		D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->OMSetDepthStencilState(ds.Get(), 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vs.Get(), nullptr, 0);
		context->PSSetShader(ps.Get(), nullptr, 0);
		ID3D11Buffer* buffer = constants.Get();
		context->VSSetConstantBuffers(0, 1, &buffer);
		context->PSSetConstantBuffers(0, 1, &buffer);
		const Constants values{ kExpectedIdentity, 0.5f, {} };
		context->UpdateSubresource(constants.Get(), 0, nullptr, &values, 0, 0);
		const UINT clearIdentity[4]{};
		context->ClearUnorderedAccessViewUint(uav.Get(), clearIdentity);
		context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		const float clearMrt[4]{};
		for (const auto& view : rtvs)
			context->ClearRenderTargetView(view.Get(), clearMrt);

		ID3D11UnorderedAccessView* rawUav = uav.Get();
		context->OMSetRenderTargetsAndUnorderedAccessViews(8, rtvRaw.data(), dsv.Get(), kUavSlot, 1, &rawUav, nullptr);
		std::array<ID3D11RenderTargetView*, 8> observedRtvs{};
		ID3D11DepthStencilView* observedDepth = nullptr;
		ID3D11UnorderedAccessView* observedUav = nullptr;
		context->OMGetRenderTargetsAndUnorderedAccessViews(8, observedRtvs.data(), &observedDepth,
			kUavSlot, 1, &observedUav);
		bool bindingValid = observedDepth == dsv.Get() && observedUav == uav.Get();
		for (size_t index = 0; index < rtvs.size(); ++index) {
			bindingValid &= observedRtvs[index] == rtvRaw[index];
			if (observedRtvs[index]) observedRtvs[index]->Release();
		}
		if (observedDepth) observedDepth->Release();
		if (observedUav) observedUav->Release();
		std::printf("%-29s %s\n", "Eight MRT + u8 binding", bindingValid ? "PASS" : "FAIL");
		if (!bindingValid)
			return 10;

		context->Draw(3, 0);
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->OMSetRenderTargetsAndUnorderedAccessViews(8, rtvRaw.data(), dsv.Get(), kUavSlot, 1, &nullUav, nullptr);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		context->CopyResource(readback.Get(), identity.Get());
		for (size_t index = 0; index < textures.size(); ++index)
			context->CopyResource(mrtReadbacks[index].Get(), textures[index].Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (!Report("Identity readback map", context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return 11;
		bool allPixelsMatch = true;
		for (UINT y = 0; y < kSize; ++y) {
			const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
			for (UINT x = 0; x < kSize; ++x)
				allPixelsMatch &= row[x] == kExpectedIdentity;
		}
		context->Unmap(readback.Get(), 0);
		std::printf("%-29s %s\n", "Identity all 16 pixels", allPixelsMatch ? "PASS" : "FAIL");
		bool allTargetsWritten = true;
		for (size_t index = 0; index < mrtReadbacks.size(); ++index) {
			D3D11_MAPPED_SUBRESOURCE mrtMapped{};
			if (!Report("MRT readback map", context->Map(mrtReadbacks[index].Get(), 0, D3D11_MAP_READ, 0, &mrtMapped)))
				return 13;
			const size_t bytesPerPixel = index == 0 ? 8 : 4;
			bool targetWritten = true;
			for (UINT y = 0; y < kSize; ++y) {
				const auto* row = static_cast<const uint8_t*>(mrtMapped.pData) + y * mrtMapped.RowPitch;
				for (UINT x = 0; x < kSize; ++x) {
					const auto* pixel = row + x * bytesPerPixel;
					targetWritten &= std::any_of(pixel, pixel + bytesPerPixel, [](uint8_t value) { return value != 0; });
				}
			}
			context->Unmap(mrtReadbacks[index].Get(), 0);
			std::printf("MRT %zu 16-pixel readback %s\n", index, targetWritten ? "PASS" : "FAIL");
			allTargetsWritten &= targetWritten;
		}
		std::puts("Depth rejection: NOT TESTED | DX12 interop: NOT TESTED | GPU performance: NOT TESTED");
		return allPixelsMatch && allTargetsWritten ? 0 : 12;
	}
}

int main()
{
	return Run();
}
