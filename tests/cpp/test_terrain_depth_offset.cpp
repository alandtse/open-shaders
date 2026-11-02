#include "Utils/TerrainDepthOffset.h"

#include <catch2/catch_test_macros.hpp>
#include <d3dcompiler.h>
#include <limits>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

TEST_CASE("Terrain depth metadata follows the shader object", "[terrain-depth]")
{
	ComPtr<ID3D11Device> device;
	REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
	constexpr char source[] = "float4 main(float4 p : POSITION) : SV_POSITION { return p; }";
	ComPtr<ID3DBlob> bytecode;
	REQUIRE(SUCCEEDED(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr,
		"main", "vs_5_0", 0, 0, &bytecode, nullptr)));
	ComPtr<ID3D11VertexShader> shader;
	REQUIRE(SUCCEEDED(device->CreateVertexShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader)));

	CSPluginAPI::TerrainDepthOffset result{ 99.0f, 1 };
	CHECK_FALSE(Util::TerrainDepth::Query(nullptr, &result));
	CHECK(result.standardOffset == 0.0f);
	CHECK(result.reverseZ == 0);
	CHECK_FALSE(Util::TerrainDepth::Query(shader.Get(), nullptr));
	CHECK_FALSE(Util::TerrainDepth::Query(shader.Get(), &result));
	CHECK_FALSE(Util::TerrainDepth::Publish(nullptr, {}));

	for (const auto reverseZ : { 0u, 1u }) {
		REQUIRE(Util::TerrainDepth::Publish(shader.Get(), { Util::TerrainDepth::StandardOffset, reverseZ }));
		REQUIRE(Util::TerrainDepth::Query(shader.Get(), &result));
		CHECK(result.standardOffset == Util::TerrainDepth::StandardOffset);
		CHECK(result.reverseZ == reverseZ);
	}

	ComPtr<ID3D11VertexShader> retained = shader;
	shader.Reset();
	REQUIRE(Util::TerrainDepth::Query(retained.Get(), &result));
	CHECK(result.standardOffset == Util::TerrainDepth::StandardOffset);
	CHECK_FALSE(Util::TerrainDepth::Publish(retained.Get(), { std::numeric_limits<float>::infinity(), 0 }));
	CHECK_FALSE(Util::TerrainDepth::Publish(retained.Get(), { 5.0f, 2 }));

	const float truncated = 5.0f;
	REQUIRE(SUCCEEDED(retained->SetPrivateData(Util::TerrainDepth::MetadataId, sizeof(truncated), &truncated)));
	CHECK_FALSE(Util::TerrainDepth::Query(retained.Get(), &result));
	CHECK(result.standardOffset == 0.0f);
	CHECK(result.reverseZ == 0);

	const CSPluginAPI::TerrainDepthOffset invalid{ std::numeric_limits<float>::quiet_NaN(), 0 };
	REQUIRE(SUCCEEDED(retained->SetPrivateData(Util::TerrainDepth::MetadataId, sizeof(invalid), &invalid)));
	CHECK_FALSE(Util::TerrainDepth::Query(retained.Get(), &result));
	CHECK(result.standardOffset == 0.0f);
}
