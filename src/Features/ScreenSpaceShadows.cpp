#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"
#include <algorithm>

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	VRBaseSamplesAtReference,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast,
	DistanceFadeStart,
	DistanceFadeEnd,
	EnableDistanceFade)

namespace
{
	bool TryGetDepthSrvDimensions(ID3D11ShaderResourceView* a_depthSrv, uint32_t& o_width, uint32_t& o_height)
	{
		o_width = 0;
		o_height = 0;
		if (!a_depthSrv)
			return false;

		ID3D11Resource* resource = nullptr;
		a_depthSrv->GetResource(&resource);
		if (!resource)
			return false;

		ID3D11Texture2D* texture = nullptr;
		HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
		resource->Release();

		if (FAILED(hr) || !texture)
			return false;

		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		texture->Release();

		if (desc.Width == 0 || desc.Height == 0)
			return false;

		o_width = desc.Width;
		o_height = desc.Height;
		return true;
	}
}

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable", (bool*)&bendSettings.Enable);
		ImGui::SliderInt("Sample Count Multiplier", (int*)&bendSettings.SampleCount, 1, 4);
		if (globals::game::isVR) {
			ImGui::SliderFloat("VR Baseline Samples", &bendSettings.VRBaseSamplesAtReference, 16.0f, 96.0f, "%.0f");
			bool enableDistanceFade = bendSettings.EnableDistanceFade != 0;
			if (ImGui::Checkbox("Enable VR Distance Fade", &enableDistanceFade))
				bendSettings.EnableDistanceFade = enableDistanceFade ? 1u : 0u;

			ImGui::BeginDisabled(!enableDistanceFade);
			ImGui::SliderFloat("VR Fade Start", &bendSettings.DistanceFadeStart, 0.0f, 1.0f, "%.3f");
			ImGui::SliderFloat("VR Fade End", &bendSettings.DistanceFadeEnd, 0.0f, 1.0f, "%.3f");
			ImGui::EndDisabled();
			ImGui::TextDisabled("Fade controls are remapped to linear-distance range.");

			// Clamp to a safe normalized range and enforce a minimum gap.
			bendSettings.DistanceFadeStart = std::min(std::max(bendSettings.DistanceFadeStart, 0.0f), 0.98f);
			bendSettings.DistanceFadeEnd = std::min(std::max(bendSettings.DistanceFadeEnd, bendSettings.DistanceFadeStart + 0.01f), 1.0f);
		}
		ImGui::SliderFloat("Surface Thickness", &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		ImGui::SliderFloat("Bilinear Threshold", &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		ImGui::SliderFloat("Shadow Contrast", &bendSettings.ShadowContrast, 0.0f, 4.0f);

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
	if (raymarchRightCS) {
		raymarchRightCS->Release();
		raymarchRightCS = nullptr;
	}
	compiledSampleCount = 0;
	compiledSampleCountRight = 0;
}

uint ScreenSpaceShadows::GetScaledSampleCount(bool a_dynamic)
{
	auto screenSize = globals::state->screenSize;

	if (a_dynamic)
		screenSize = Util::ConvertToDynamic(globals::state->screenSize);

	if (globals::game::isVR) {
		// Per-eye VR scaling against a 4.5 MP reference eye.
		constexpr float kReferencePerEyeArea = 4'500'000.0f;
		float currentArea = (screenSize.x * 0.5f) * screenSize.y;
		float areaScale = std::sqrt(currentArea / kReferencePerEyeArea);
		float baseSamples = std::max(1.0f, bendSettings.VRBaseSamplesAtReference);
		uint scaledSampleCount = static_cast<uint>(std::round(bendSettings.SampleCount * baseSamples * areaScale));
		return std::max(1u, scaledSampleCount);
	}

	// Scale sample count based on both dimensions relative to 1920x1080 reference

	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = screenSize.x * screenSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(bendSettings.SampleCount * 60 * areaScale));

	return scaledSampleCount;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	const uint scaledSampleCount = GetScaledSampleCount(false);
	if (raymarchCS && compiledSampleCount != scaledSampleCount) {
		raymarchCS->Release();
		raymarchCS = nullptr;
		compiledSampleCount = 0;
	}

	if (!raymarchCS) {
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", { { "SAMPLE_COUNT", std::format("{}", scaledSampleCount).c_str() } }, "cs_5_0");
		compiledSampleCount = scaledSampleCount;
	}
	return raymarchCS;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarchRight()
{
	const uint scaledSampleCount = GetScaledSampleCount(false);
	if (raymarchRightCS && compiledSampleCountRight != scaledSampleCount) {
		raymarchRightCS->Release();
		raymarchRightCS = nullptr;
		compiledSampleCountRight = 0;
	}

	if (!raymarchRightCS) {
		raymarchRightCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", { { "SAMPLE_COUNT", std::format("{}", scaledSampleCount).c_str() }, { "RIGHT", "" } }, "cs_5_0");
		compiledSampleCountRight = scaledSampleCount;
	}
	return raymarchRightCS;
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScoped;
	auto state = globals::state;
	TracyD3D11Zone(state->tracyCtx, "Screen Space Shadows");

	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection for a given eye
	auto CalculateLightProjection = [&](uint32_t eyeIndex = 0) -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProjUnjittered(eyeIndex).Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection(0);

	float2 renderSize = Util::ConvertToDynamic(state->screenSize);
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	if (globals::game::isVR)
		viewportSize[0] /= 2;

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	// Setup common render state
	auto* depthSRV = Util::GetCurrentSceneDepthSRV();
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };
	uint32_t depthWidth = 0;
	uint32_t depthHeight = 0;
	if (TryGetDepthSrvDimensions(depthSRV, depthWidth, depthHeight)) {
		if (globals::game::isVR) {
			// In VR, viewportSize.x is per-eye but depth SRV can be either per-eye or combined.
			dynamicRes.x = (static_cast<float>(viewportSize[0]) * 2.0f) / static_cast<float>(depthWidth);
			dynamicRes.y = static_cast<float>(viewportSize[1]) / static_cast<float>(depthHeight);
		} else {
			dynamicRes.x = static_cast<float>(viewportSize[0]) / static_cast<float>(depthWidth);
			dynamicRes.y = static_cast<float>(viewportSize[1]) / static_cast<float>(depthHeight);
		}
	}

	auto* raymarchLeft = GetComputeRaymarch();
	ID3D11ComputeShader* raymarchRight = globals::game::isVR ? GetComputeRaymarchRight() : nullptr;

	uint maxCompiledSamples = compiledSampleCount > 0 ? compiledSampleCount : GetScaledSampleCount(false);
	if (globals::game::isVR && compiledSampleCountRight > 0)
		maxCompiledSamples = std::min(maxCompiledSamples, compiledSampleCountRight);

	uint dynamicSampleCount = std::min(GetScaledSampleCount(true), maxCompiledSamples);
	dynamicSampleCount = std::max(dynamicSampleCount, 1u);
	uint dynamicReadCount = (dynamicSampleCount / 64 + 2);

	// Shared dispatch logic for both VR and non-VR
	auto DispatchEye = [&](const char* eyeName, ID3D11ComputeShader* shader, const float* lightProj,
						   float invTexSizeX, float invTexSizeY) {
		if (globals::state->frameAnnotations && eyeName) {
			std::string eventName = std::format("SSS - Ray March ({})", eyeName);
			globals::state->BeginPerfEvent(eventName);
		} else if (globals::state->frameAnnotations) {
			globals::state->BeginPerfEvent("SSS - Ray March");
		}

		context->CSSetShader(shader, nullptr, 0);

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			TracyD3D11Zone(globals::state->tracyCtx, "SSS - Ray March");

			auto dispatchData = dispatchList.Dispatch[i];

			RaymarchCB data{};
			data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
			data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
			data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
			data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

			data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
			data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

			data.FarDepthValue = 1.0f;
			data.NearDepthValue = 0.0f;

			data.DynamicRes = dynamicRes;

			data.DynamicSampleCount = dynamicSampleCount;
			data.DynamicReadCount = dynamicReadCount;

			data.InvDepthTextureSize[0] = invTexSizeX;
			data.InvDepthTextureSize[1] = invTexSizeY;

			data.settings = bendSettings;

			raymarchCB->Update(data);

			context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
		}

		if (globals::state->frameAnnotations) {
			globals::state->EndPerfEvent();
		}
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	if (!globals::game::isVR) {
		DispatchEye(nullptr, raymarchLeft, lightProjectionF.data(), InvTexSizeX, InvTexSizeY);
	} else {
		DispatchEye("Left Eye", raymarchLeft, lightProjectionF.data(), InvTexSizeX, InvTexSizeY);

		// Calculate light projection for right eye
		auto lightProjectionRightF = CalculateLightProjection(1);
		DispatchEye("Right Eye", raymarchRight, lightProjectionRightF.data(), InvTexSizeX, InvTexSizeY);
	}

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky)
		if (bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull)
			DrawShadows();

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>());

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = new Texture2D(texDesc);
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);
	}
}
