// Unit tests for Util::UIBlend (src/Utils/UIBlendCoverage.h).
//
// Blends real draws on a WARP device with the patched state and applies the
// premultiplied composite used by HDROutputCS: ui.rgb + scene * (1 - ui.a).

#include "Utils/UIBlendCoverage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <d3dcompiler.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

using Catch::Approx;

namespace
{
	D3D11_BLEND_DESC MakeDesc(D3D11_BLEND src, D3D11_BLEND dest, D3D11_BLEND_OP op = D3D11_BLEND_OP_ADD)
	{
		D3D11_BLEND_DESC desc{};
		auto& rt = desc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = src;
		rt.DestBlend = dest;
		rt.BlendOp = op;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ZERO;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		return desc;
	}

	using Pixel = std::array<float, 4>;

	// Draws one fullscreen layer per colour onto a cleared 1x1 UI target through the patched blend state.
	class UILayer
	{
	public:
		UILayer()
		{
			const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
			REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
				D3D11_SDK_VERSION, device.put(), nullptr, context.put())));

			constexpr std::string_view shader = R"(
				cbuffer C : register(b0) { float4 color; };
				float4 VS(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
				float4 PS() : SV_Target { return color; })";
			const auto vsBlob = Compile(shader, "VS", "vs_5_0");
			const auto psBlob = Compile(shader, "PS", "ps_5_0");
			REQUIRE(SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, vs.put())));
			REQUIRE(SUCCEEDED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, ps.put())));

			D3D11_TEXTURE2D_DESC texDesc{ 1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
			REQUIRE(SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, target.put())));
			REQUIRE(SUCCEEDED(device->CreateRenderTargetView(target.get(), nullptr, rtv.put())));
			texDesc.Usage = D3D11_USAGE_STAGING;
			texDesc.BindFlags = 0;
			texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			REQUIRE(SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, staging.put())));

			D3D11_BUFFER_DESC cbDesc{ sizeof(Pixel), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
			REQUIRE(SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, constants.put())));

			const float clear[4]{};
			context->ClearRenderTargetView(rtv.get(), clear);
		}

		void Draw(D3D11_BLEND_DESC desc, const Pixel& color)
		{
			Util::UIBlend::PatchCoverageAlpha(desc);
			winrt::com_ptr<ID3D11BlendState> blend;
			REQUIRE(SUCCEEDED(device->CreateBlendState(&desc, blend.put())));

			context->UpdateSubresource(constants.get(), 0, nullptr, color.data(), 0, 0);
			ID3D11Buffer* cb = constants.get();
			ID3D11RenderTargetView* view = rtv.get();
			const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(vs.get(), nullptr, 0);
			context->PSSetShader(ps.get(), nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &cb);
			context->RSSetViewports(1, &viewport);
			context->OMSetRenderTargets(1, &view, nullptr);
			context->OMSetBlendState(blend.get(), nullptr, 0xFFFFFFFF);
			context->Draw(3, 0);
		}

		Pixel Read()
		{
			context->CopyResource(staging.get(), target.get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			REQUIRE(SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
			Pixel pixel{};
			std::memcpy(pixel.data(), mapped.pData, sizeof(pixel));
			context->Unmap(staging.get(), 0);
			return pixel;
		}

		float CompositeRed(float scene)
		{
			const auto ui = Read();
			return ui[0] + scene * (1.0f - ui[3]);
		}

	private:
		static winrt::com_ptr<ID3DBlob> Compile(std::string_view source, const char* entry, const char* profile)
		{
			winrt::com_ptr<ID3DBlob> code;
			REQUIRE(SUCCEEDED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile, 0, 0, code.put(), nullptr)));
			return code;
		}

		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11PixelShader> ps;
		winrt::com_ptr<ID3D11Texture2D> target;
		winrt::com_ptr<ID3D11Texture2D> staging;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11Buffer> constants;
	};
}

TEST_CASE("UI blend: alpha blends accumulate coverage", "[ui-blend]")
{
	auto desc = MakeDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
	CHECK(Util::UIBlend::PatchCoverageAlpha(desc));
	CHECK(desc.RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_ONE);
	CHECK(desc.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA);
	CHECK_FALSE(Util::UIBlend::PatchCoverageAlpha(desc));
}

TEST_CASE("UI blend: additive blends keep destination coverage", "[ui-blend]")
{
	for (const auto src : { D3D11_BLEND_ONE, D3D11_BLEND_SRC_ALPHA }) {
		auto desc = MakeDesc(src, D3D11_BLEND_ONE);
		CHECK(Util::UIBlend::PatchCoverageAlpha(desc));
		CHECK(desc.RenderTarget[0].SrcBlendAlpha == D3D11_BLEND_ZERO);
		CHECK(desc.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_ONE);
		CHECK(desc.RenderTarget[0].BlendOpAlpha == D3D11_BLEND_OP_ADD);
		CHECK_FALSE(Util::UIBlend::PatchCoverageAlpha(desc));
	}
}

TEST_CASE("UI blend: additive state that already accumulates coverage is corrected", "[ui-blend]")
{
	auto desc = MakeDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE);
	desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	CHECK(Util::UIBlend::PatchCoverageAlpha(desc));
	CHECK(desc.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_ONE);
}

TEST_CASE("UI blend: disabled and destination-only targets are untouched", "[ui-blend]")
{
	auto disabled = MakeDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);
	disabled.RenderTarget[0].BlendEnable = FALSE;
	CHECK_FALSE(Util::UIBlend::PatchCoverageAlpha(disabled));

	auto destinationOnly = MakeDesc(D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA);
	CHECK_FALSE(Util::UIBlend::PatchCoverageAlpha(destinationOnly));
}

TEST_CASE("UI blend: independent targets are patched per slot", "[ui-blend]")
{
	auto desc = MakeDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
	desc.IndependentBlendEnable = TRUE;
	desc.RenderTarget[1] = MakeDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE).RenderTarget[0];
	CHECK(Util::UIBlend::PatchCoverageAlpha(desc));
	CHECK(desc.RenderTarget[0].DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA);
	CHECK(desc.RenderTarget[1].DestBlendAlpha == D3D11_BLEND_ONE);
	CHECK(desc.RenderTarget[2].DestBlendAlpha == D3D11_BLEND{});
}

TEST_CASE("UI blend: an additive glow brightens the scene without darkening it", "[ui-blend][warp]")
{
	UILayer ui;
	ui.Draw(MakeDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE), { 0.4f, 0.0f, 0.0f, 0.5f });

	CHECK(ui.Read()[3] == Approx(0.0f));
	CHECK(ui.CompositeRed(0.6f) == Approx(0.8f));
}

TEST_CASE("UI blend: an additive glow over a panel keeps the panel's coverage", "[ui-blend][warp]")
{
	UILayer ui;
	ui.Draw(MakeDesc(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA), { 0.2f, 0.0f, 0.0f, 0.5f });
	ui.Draw(MakeDesc(D3D11_BLEND_ONE, D3D11_BLEND_ONE), { 0.3f, 0.0f, 0.0f, 0.8f });

	CHECK(ui.Read()[3] == Approx(0.5f));
	CHECK(ui.CompositeRed(0.6f) == Approx(0.7f));
}
