#include "IBL.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "Shadercache.h"
#include "State.h"
#include "WeatherVariableRegistry.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

namespace
{
	constexpr uint32_t kIblPsSrvSlot = 76u;
	constexpr uint32_t kIblPsSrvCount = 4u;

	void SetIblPsSrvs(ID3D11DeviceContext* a_context,
		ID3D11ShaderResourceView* a_diffuse,
		ID3D11ShaderResourceView* a_diffuseSky,
		ID3D11ShaderResourceView* a_staticDiffuse,
		ID3D11ShaderResourceView* a_staticSpecular)
	{
		ID3D11ShaderResourceView* srvs[kIblPsSrvCount] = { a_diffuse, a_diffuseSky, a_staticDiffuse, a_staticSpecular };
		a_context->PSSetShaderResources(kIblPsSrvSlot, kIblPsSrvCount, srvs);
	}

	void ClearIblPsSrvs(ID3D11DeviceContext* a_context)
	{
		SetIblPsSrvs(a_context, nullptr, nullptr, nullptr, nullptr);
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	IBL::Settings,
	EnableIBL,
	EnableDiffuseIBL,
	PreserveFogLuminance,
	UseStaticIBL,
	EnableInterior,
	DiffuseIBLScale,
	DALCAmount,
	IBLSaturation,
	FogAmount)

void IBL::DrawSettings()
{
	bool enableIBL = settings.EnableIBL != 0;
	if (ImGui::Checkbox("Enable IBL", &enableIBL)) {
		settings.EnableIBL = enableIBL ? 1u : 0u;
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Turns Image Based Lighting on or off.");
	}

	ImGui::BeginDisabled(settings.EnableIBL == 0);
	Util::WeatherUI::Checkbox("Enable Diffuse IBL", this, "EnableDiffuseIBL", (bool*)&settings.EnableDiffuseIBL);
	Util::WeatherUI::SliderFloat("Diffuse IBL Scale", this, "DiffuseIBLScale", &settings.DiffuseIBLScale, 0.0f, 10.0f, "%.2f");
	Util::WeatherUI::SliderFloat("Diffuse IBL Saturation", this, "IBLSaturation", &settings.IBLSaturation, 0.0f, 2.0f, "%.2f");
	Util::WeatherUI::SliderFloat("DALC Amount", this, "DALCAmount", &settings.DALCAmount, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox("Enable Interior", (bool*)&settings.EnableInterior);
	ImGui::Checkbox("Use Static IBL For Out-of-World Objects", (bool*)&settings.UseStaticIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enables the use of static IBL textures for objects that are not in the world (e.g. inventory items).");
	}
	Util::WeatherUI::SliderFloat("Fog Mix", this, "FogAmount", &settings.FogAmount, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox("Preserve Fog Luminance", (bool*)&settings.PreserveFogLuminance);
	ImGui::EndDisabled();
}

void IBL::LoadSettings(json& o_json)
{
	settings = o_json;
}

void IBL::SaveSettings(json& o_json)
{
	o_json = settings;
}

void IBL::RestoreDefaultSettings()
{
	settings = {};
}

void IBL::RegisterWeatherVariables()
{
	auto* registry = WeatherVariables::GlobalWeatherRegistry::GetSingleton()
	                     ->GetOrCreateFeatureRegistry(GetShortName());
	// Register enable diffuse IBL toggle
	registry->RegisterVariable(std::make_shared<WeatherVariables::WeatherVariable<bool>>(
		"EnableDiffuseIBL",
		"Enable Diffuse IBL",
		"Enable or disable diffuse IBL for this weather",
		(bool*)&settings.EnableDiffuseIBL,
		true,
		[](const bool& from, const bool& to, float factor) {
			return factor > 0.5f ? to : from;  // Switch at transition midpoint
		}));

	// Register diffuse IBL scale - controls the overall intensity of diffuse IBL
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"DiffuseIBLScale",
		"Diffuse IBL Scale",
		"Controls the overall intensity of diffuse IBL lighting",
		&settings.DiffuseIBLScale,
		1.0f,
		0.0f, 10.0f));

	// Register IBL saturation - controls color saturation of IBL
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"IBLSaturation",
		"IBL Saturation",
		"Controls the color saturation of IBL lighting",
		&settings.IBLSaturation,
		1.0f,
		0.0f, 2.0f));

	// Register DALC amount - controls mixing with Directional Ambient Light Color
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"DALCAmount",
		"DALC Amount",
		"Amount of DALC (Directional Ambient Light Color) mixing",
		&settings.DALCAmount,
		0.33f,
		0.0f, 1.0f));

	// Register fog amount - controls fog mixing
	registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(
		"FogAmount",
		"Fog Mix",
		"Amount of fog mixed into IBL",
		&settings.FogAmount,
		0.0f,
		0.0f, 1.0f));
}

void IBL::ReflectionsPrepass()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	if (!IsRuntimeEnabled()) {
		ClearIblPsSrvs(context);
		return;
	}

	SetIblPsSrvs(
		context,
		diffuseIBLTexture ? diffuseIBLTexture->srv.get() : nullptr,
		diffuseSkyIBLTexture ? diffuseSkyIBLTexture->srv.get() : nullptr,
		staticDiffuseIBLTexture ? staticDiffuseIBLTexture->srv.get() : nullptr,
		staticSpecularIBLTexture ? staticSpecularIBLTexture->srv.get() : nullptr
	);
}

void IBL::Prepass()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	// Always clear all IBL SRV slots first to avoid stale bindings.
	ClearIblPsSrvs(context);

	if (!IsRuntimeEnabled())
		return;

	if (!diffuseIBLTexture || !diffuseSkyIBLTexture)
		return;

	auto state = globals::state;

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;

	auto& envTexture = dynamicCubemaps.envTexture;
	auto& envReflectionsTexture = dynamicCubemaps.envReflectionsTexture;

	state->BeginPerfEvent("IBL");
	std::array<ID3D11ShaderResourceView*, 1> srvs = { (dynamicCubemaps.loaded && envTexture) ? envTexture->srv.get() : nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { diffuseIBLTexture->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { Deferred::GetSingleton()->linearSampler };

	// IBL
	{
		samplers[0] = Deferred::GetSingleton()->linearSampler;

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(GetDiffuseIBLCS(), nullptr, 0);
		context->Dispatch(1, 1, 1);
	}

	// IBL with sky
	{
		srvs.at(0) = (dynamicCubemaps.loaded && envReflectionsTexture) ? envReflectionsTexture->srv.get() : nullptr;
		uavs.at(0) = diffuseSkyIBLTexture->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->Dispatch(1, 1, 1);
	}

	// Reset
	{
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		samplers.fill(nullptr);

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();

	// Set PS shader resource
	SetIblPsSrvs(context, diffuseIBLTexture->srv.get(), diffuseSkyIBLTexture->srv.get(), nullptr, nullptr);
}

void IBL::SetupResources()
{
	GetDiffuseIBLCS();

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 3,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		diffuseIBLTexture = new Texture2D(texDesc);
		diffuseIBLTexture->CreateSRV(srvDesc);
		diffuseIBLTexture->CreateUAV(uavDesc);
		diffuseSkyIBLTexture = new Texture2D(texDesc);
		diffuseSkyIBLTexture->CreateSRV(srvDesc);
		diffuseSkyIBLTexture->CreateUAV(uavDesc);
	}

	auto device = globals::d3d::device;

	logger::debug("Loading static Diffuse IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\DiffuseIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticDiffuseIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		staticDiffuseIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticDiffuseIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		staticDiffuseIBLTexture->CreateSRV(srvDesc);
	}

	logger::debug("Loading static Specular IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\SpecIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		staticSpecularIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		staticSpecularIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticSpecularIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 8 }
		};
		staticSpecularIBLTexture->CreateSRV(srvDesc);
	}
}

void IBL::ClearShaderCache()
{
	if (diffuseIBLCS)
		diffuseIBLCS->Release();
	diffuseIBLCS = nullptr;
}

ID3D11ComputeShader* IBL::GetDiffuseIBLCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });
	if (!diffuseIBLCS)
		diffuseIBLCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\IBL\\DiffuseIBLCS.hlsl", defines, "cs_5_0"));
	return diffuseIBLCS;
}

IBL::CommonBufferData IBL::GetCommonBufferData() const
{
	return {
		.EnableDiffuseIBL = (settings.EnableIBL != 0) ? settings.EnableDiffuseIBL : 0u,
		.PreserveFogLuminance = settings.PreserveFogLuminance,
		.UseStaticIBL = settings.UseStaticIBL,
		.EnableInterior = settings.EnableInterior,
		.DiffuseIBLScale = settings.DiffuseIBLScale,
		.DALCAmount = settings.DALCAmount,
		.IBLSaturation = settings.IBLSaturation,
		.FogAmount = settings.FogAmount
	};
}

bool IBL::IsRuntimeEnabled() const
{
	return loaded && settings.EnableIBL != 0;
}
