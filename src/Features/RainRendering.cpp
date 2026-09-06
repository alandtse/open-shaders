#include "RainRendering.h"

#include "DynamicCubemaps.h"
#include "Globals.h"
#include "GpuPass.h"
#include "I18n/I18n.h"
#include "InverseSquareLighting.h"
#include "LightLimitFix.h"
#include "Menu.h"
#include "Skylighting.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#define I18N_KEY_PREFIX "feature.rain_rendering."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RainRendering::Settings,
	EnableRainRendering,
	ForceRainRendering,
	EnableRainRoofOcclusion,
	EnableRainCanopyResponse,
	RainDropCount,
	RainOverheadDropCount,
	RainDensity,
	RainFallSpeed,
	RainStreakLength,
	RainVelocityStretch,
	RainStreakWidth,
	RainOpacity,
	RainBrightness,
	RainLightingResponse,
	RainMinimumVisibility,
	RainNearCutoffDistance,
	RainFarDistance,
	RainNearLayerDistance,
	RainMidLayerDistance,
	RainNearBudgetWeight,
	RainMidBudgetWeight,
	RainFarBudgetWeight,
	EnableDistantRain,
	RainDistantDropCount,
	RainDistantDensity,
	RainDistantOpacity,
	RainDistantStreakLength,
	RainDistantStreakWidth,
	RainDensityNoiseScale,
	RainDensityNoiseStrength,
	RainCurtainScale,
	RainCurtainStrength,
	RainCurtainContrast,
	RainCurtainMinDensity,
	RainCurtainMaxDensity,
	RainIntersectionFadeDistance,
	RainDebugMode,
	EnableGlassyRain,
	EnableRainRefraction,
	RainCoreDarkening,
	RainEdgeHighlight,
	RainRefractionStrength,
	RainRefractionDistance,
	RainStreakVariation,
	RainLocalLightResponse,
	EnableTexturedRain,
	RainTextureNormalStrength,
	RainTextureReflectionStrength,
	RainTextureUVWidth,
	RainEnvironmentTransmission,
	RainSceneRefractionMix,
	RainHighlightRoughness,
	RainLightScattering,
	RainRoofOcclusionFadeStart,
	RainRoofOcclusionFadeEnd,
	RainCanopyDensityScale,
	RainCanopySpeedScale)

namespace
{
	constexpr uint32_t kMaximumDropCount = 32768;
	constexpr uint32_t kRainComputeGroupSize = 64;
	constexpr uint32_t kMaximumCompactionGroupCount = kMaximumDropCount / kRainComputeGroupSize;
	constexpr uint32_t kMaximumDistantDropCount = 16384;
	constexpr uint32_t kCanopyProbeWidth = 256;
	constexpr uint32_t kCanopyProbeHeight = 256;
	constexpr uint32_t kCanopyProbeDepth = 128;
	constexpr uint32_t kGridWidth = 96;
	constexpr uint32_t kGridDepth = 96;
	constexpr uint32_t kGridHeight = 4;
	constexpr float kMaximumRainParticleDensity = 3.0f;
	constexpr float kReferenceRainGravity = 675.0f;
	constexpr float kMaximumRefractionPixels = 12.0f;
	constexpr float kMaximumVRRefractionPixels = 4.0f;
	constexpr const char* kRainTexturePath = "Data\\Textures\\CommunityShaders\\RainRendering\\RainDrop.png";

	struct RainPerformancePreset
	{
		uint32_t dropCount;
		float density;
		float farDistance;
	};

	constexpr RainPerformancePreset GetRainPerformancePreset(Feature::PerfProfile a_profile)
	{
		switch (a_profile) {
		case Feature::PerfProfile::Performance:
			return { 8192, 0.8f, 4500.0f };
		case Feature::PerfProfile::Balanced:
			return { 16384, 1.0f, 6000.0f };
		default:
			return { 24576, 1.15f, 9000.0f };
		}
	}

	float ClampFinite(float a_value, float a_minimum, float a_maximum, float a_default)
	{
		return std::clamp(std::isfinite(a_value) ? a_value : a_default, a_minimum, a_maximum);
	}

	float LinearStep(float a_minimum, float a_maximum, float a_value)
	{
		if (a_minimum >= a_maximum)
			return a_value >= a_maximum ? 1.0f : 0.0f;
		return std::clamp((a_value - a_minimum) / (a_maximum - a_minimum), 0.0f, 1.0f);
	}

	bool UsesWaterMaterial(const RainRendering::Settings& a_settings)
	{
		return a_settings.EnableGlassyRain && (a_settings.RainDebugMode == 0 || a_settings.RainDebugMode >= 6);
	}

	bool DrawFlagCheckbox(const char* a_label, uint& a_value)
	{
		bool enabled = a_value != 0;
		if (!ImGui::Checkbox(a_label, &enabled))
			return false;
		a_value = enabled ? 1u : 0u;
		return true;
	}

	template <class T>
	void ReleasePointer(T*& a_pointer)
	{
		if (a_pointer) {
			a_pointer->Release();
			a_pointer = nullptr;
		}
	}

	/** @brief Saves only the D3D11 state modified by the procedural rain draw. */
	class RainPipelineState
	{
	public:
		explicit RainPipelineState(ID3D11DeviceContext* a_context) : context(a_context)
		{
			context->IAGetInputLayout(&inputLayout);
			context->IAGetPrimitiveTopology(&topology);

			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->HSGetShader(&hullShader, nullptr, nullptr);
			context->DSGetShader(&domainShader, nullptr, nullptr);
			context->GSGetShader(&geometryShader, nullptr, nullptr);
			context->VSGetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers.size()), vertexConstantBuffers.data());
			context->VSGetShaderResources(1, 1, &vertexShaderResource);
			context->VSGetShaderResources(39, 1, &visibleIndexResource);

			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->PSGetConstantBuffers(0, 1, &pixelConstantBuffer);
			context->PSGetConstantBuffers(5, static_cast<UINT>(pixelSharedConstantBuffers.size()), pixelSharedConstantBuffers.data());
			context->PSGetShaderResources(0, static_cast<UINT>(pixelShaderResources.size()), pixelShaderResources.data());
			context->PSGetSamplers(0, 1, &pixelSampler);

			context->RSGetState(&rasterizer);
			viewportCount = static_cast<UINT>(viewports.size());
			context->RSGetViewports(&viewportCount, viewports.data());

			context->OMGetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), &depthStencilView);
			context->OMGetBlendState(&blend, blendFactor.data(), &sampleMask);
			context->OMGetDepthStencilState(&depthStencil, &stencilReference);
		}

		RainPipelineState(const RainPipelineState&) = delete;
		RainPipelineState& operator=(const RainPipelineState&) = delete;

		~RainPipelineState()
		{
			std::array<ID3D11ShaderResourceView*, 6> nullResources{};
			context->PSSetShaderResources(0, static_cast<UINT>(nullResources.size()), nullResources.data());
			ID3D11ShaderResourceView* nullVertexResource = nullptr;
			context->VSSetShaderResources(1, 1, &nullVertexResource);
			context->VSSetShaderResources(39, 1, &nullVertexResource);

			context->IASetInputLayout(inputLayout);
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->HSSetShader(hullShader, nullptr, 0);
			context->DSSetShader(domainShader, nullptr, 0);
			context->GSSetShader(geometryShader, nullptr, 0);
			context->VSSetConstantBuffers(0, static_cast<UINT>(vertexConstantBuffers.size()), vertexConstantBuffers.data());
			context->VSSetShaderResources(1, 1, &vertexShaderResource);
			context->VSSetShaderResources(39, 1, &visibleIndexResource);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &pixelConstantBuffer);
			context->PSSetConstantBuffers(5, static_cast<UINT>(pixelSharedConstantBuffers.size()), pixelSharedConstantBuffers.data());
			context->RSSetState(rasterizer);
			context->RSSetViewports(viewportCount, viewports.data());
			context->OMSetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), depthStencilView);
			context->OMSetBlendState(blend, blendFactor.data(), sampleMask);
			context->OMSetDepthStencilState(depthStencil, stencilReference);
			context->PSSetShaderResources(0, static_cast<UINT>(pixelShaderResources.size()), pixelShaderResources.data());
			context->PSSetSamplers(0, 1, &pixelSampler);

			ReleasePointer(inputLayout);
			ReleasePointer(vertexShader);
			ReleasePointer(hullShader);
			ReleasePointer(domainShader);
			ReleasePointer(geometryShader);
			for (auto*& buffer : vertexConstantBuffers)
				ReleasePointer(buffer);
			ReleasePointer(vertexShaderResource);
			ReleasePointer(visibleIndexResource);
			ReleasePointer(pixelShader);
			ReleasePointer(pixelConstantBuffer);
			for (auto*& buffer : pixelSharedConstantBuffers)
				ReleasePointer(buffer);
			for (auto*& resource : pixelShaderResources)
				ReleasePointer(resource);
			ReleasePointer(pixelSampler);
			ReleasePointer(rasterizer);
			for (auto*& target : renderTargets)
				ReleasePointer(target);
			ReleasePointer(depthStencilView);
			ReleasePointer(blend);
			ReleasePointer(depthStencil);
		}

	private:
		ID3D11DeviceContext* context;
		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11HullShader* hullShader = nullptr;
		ID3D11DomainShader* domainShader = nullptr;
		ID3D11GeometryShader* geometryShader = nullptr;
		std::array<ID3D11Buffer*, 13> vertexConstantBuffers{};
		ID3D11ShaderResourceView* vertexShaderResource = nullptr;
		ID3D11ShaderResourceView* visibleIndexResource = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11Buffer* pixelConstantBuffer = nullptr;
		std::array<ID3D11Buffer*, 2> pixelSharedConstantBuffers{};
		std::array<ID3D11ShaderResourceView*, 6> pixelShaderResources{};
		ID3D11SamplerState* pixelSampler = nullptr;
		ID3D11RasterizerState* rasterizer = nullptr;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		UINT viewportCount = 0;
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets{};
		ID3D11DepthStencilView* depthStencilView = nullptr;
		ID3D11BlendState* blend = nullptr;
		std::array<float, 4> blendFactor{};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthStencil = nullptr;
		UINT stencilReference = 0;
	};

	/** @brief Preserves the compute slots used to update the shared rain-drop buffer. */
	class RainComputeState
	{
	public:
		explicit RainComputeState(ID3D11DeviceContext* a_context) : context(a_context)
		{
			context->CSGetShader(&shader, nullptr, nullptr);
			context->CSGetConstantBuffers(0, 1, &rainConstantBuffer);
			context->CSGetConstantBuffers(5, static_cast<UINT>(sharedConstantBuffers.size()), sharedConstantBuffers.data());
			context->CSGetConstantBuffers(12, 1, &frameConstantBuffer);
			context->CSGetShaderResources(0, 1, &occlusionDepthResource);
			context->CSGetShaderResources(1, 1, &dropResource);
			context->CSGetShaderResources(39, static_cast<UINT>(compactionResources.size()), compactionResources.data());
			context->CSGetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
			context->CSGetShaderResources(41, static_cast<UINT>(canopyResources.size()), canopyResources.data());
			context->CSGetSamplers(0, 1, &computeSampler);
			context->CSGetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data());
		}

		RainComputeState(const RainComputeState&) = delete;
		RainComputeState& operator=(const RainComputeState&) = delete;

		~RainComputeState()
		{
			ID3D11ShaderResourceView* nullOcclusionDepth = nullptr;
			context->CSSetShaderResources(0, 1, &nullOcclusionDepth);
			ID3D11ShaderResourceView* nullDropResource = nullptr;
			context->CSSetShaderResources(1, 1, &nullDropResource);
			std::array<ID3D11ShaderResourceView*, 2> nullResources{};
			context->CSSetShaderResources(39, static_cast<UINT>(nullResources.size()), nullResources.data());
			std::array<ID3D11ShaderResourceView*, 2> nullCanopyResources{};
			context->CSSetShaderResources(41, static_cast<UINT>(nullCanopyResources.size()), nullCanopyResources.data());
			std::array<ID3D11UnorderedAccessView*, 5> nullUAVs{};
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr);
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &rainConstantBuffer);
			context->CSSetConstantBuffers(5, static_cast<UINT>(sharedConstantBuffers.size()), sharedConstantBuffers.data());
			context->CSSetConstantBuffers(12, 1, &frameConstantBuffer);
			context->CSSetShaderResources(0, 1, &occlusionDepthResource);
			context->CSSetShaderResources(1, 1, &dropResource);
			context->CSSetShaderResources(39, static_cast<UINT>(compactionResources.size()), compactionResources.data());
			context->CSSetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
			context->CSSetShaderResources(41, static_cast<UINT>(canopyResources.size()), canopyResources.data());
			context->CSSetSamplers(0, 1, &computeSampler);
			context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);

			ReleasePointer(shader);
			ReleasePointer(rainConstantBuffer);
			for (auto*& buffer : sharedConstantBuffers)
				ReleasePointer(buffer);
			ReleasePointer(frameConstantBuffer);
			ReleasePointer(occlusionDepthResource);
			ReleasePointer(dropResource);
			for (auto*& resource : compactionResources)
				ReleasePointer(resource);
			for (auto*& resource : computeResources)
				ReleasePointer(resource);
			for (auto*& resource : canopyResources)
				ReleasePointer(resource);
			ReleasePointer(computeSampler);
			for (auto*& uav : computeUAVs)
				ReleasePointer(uav);
		}

	private:
		ID3D11DeviceContext* context;
		ID3D11ComputeShader* shader = nullptr;
		ID3D11Buffer* rainConstantBuffer = nullptr;
		std::array<ID3D11Buffer*, 2> sharedConstantBuffers{};
		ID3D11Buffer* frameConstantBuffer = nullptr;
		ID3D11ShaderResourceView* occlusionDepthResource = nullptr;
		ID3D11ShaderResourceView* dropResource = nullptr;
		std::array<ID3D11ShaderResourceView*, 2> compactionResources{};
		std::array<ID3D11ShaderResourceView*, 4> computeResources{};
		std::array<ID3D11ShaderResourceView*, 2> canopyResources{};
		ID3D11SamplerState* computeSampler = nullptr;
		std::array<ID3D11UnorderedAccessView*, 5> computeUAVs{};
	};
}

std::pair<std::string, std::vector<std::string>> RainRendering::GetFeatureSummary()
{
	return {
		T("feature.rain_rendering.description",
			"Airborne Rain renders dense world-space precipitation with stereo-stable depth and storm curtains."),
		{ T("feature.rain_rendering.key_feature_1", "The same world-space streak geometry is projected into both VR eyes"),
			T("feature.rain_rendering.key_feature_2", "GPU-generated drops with no per-particle CPU simulation"),
			T("feature.rain_rendering.key_feature_3", "Stable world-space density variation and rain curtains"),
			T("feature.rain_rendering.key_feature_4", "Scene-depth occlusion with distance-layered precipitation") }
	};
}

void RainRendering::SetupResources()
{
	auto* device = globals::d3d::device;
	if (!device)
		return;

	if (!perFrameCB)
		perFrameCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PerFrame>(), "RainRendering::PerFrame");
	if (!dropBuffer) {
		dropBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<DropData>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::Drops");
		dropBuffer->CreateSRV();
		dropBuffer->CreateUAV();
	}
	if (!dropLocalOffsetBuffer) {
		dropLocalOffsetBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::DropLocalOffsets");
		dropLocalOffsetBuffer->CreateSRV();
		dropLocalOffsetBuffer->CreateUAV();
	}
	if (!dropGroupOffsetBuffer) {
		dropGroupOffsetBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumCompactionGroupCount), true, false),
			kMaximumCompactionGroupCount,
			"RainRendering::DropGroupOffsets");
		dropGroupOffsetBuffer->CreateSRV();
		dropGroupOffsetBuffer->CreateUAV();
	}
	if (!visibleDropIndexBuffer) {
		visibleDropIndexBuffer = std::make_unique<StructuredBuffer>(
			StructuredBufferDesc<uint32_t>(static_cast<uint64_t>(kMaximumDropCount), true, false),
			kMaximumDropCount,
			"RainRendering::VisibleDropIndices");
		visibleDropIndexBuffer->CreateSRV();
		visibleDropIndexBuffer->CreateUAV();
	}
	if (!indirectDrawArgsBuffer) {
		D3D11_BUFFER_DESC description{};
		description.ByteWidth = sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS);
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		description.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		indirectDrawArgsBuffer = std::make_unique<Buffer>(description, nullptr, "RainRendering::IndirectDrawArgs");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
		uavDescription.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDescription.Buffer.NumElements = description.ByteWidth / sizeof(uint32_t);
		uavDescription.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		indirectDrawArgsBuffer->CreateUAV(uavDescription);
	}

	if (!blendState) {
		D3D11_BLEND_DESC description{};
		description.RenderTarget[0].BlendEnable = TRUE;
		description.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		description.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		description.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		description.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		description.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		description.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		description.RenderTarget[0].RenderTargetWriteMask =
			D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&description, blendState.put()));
		Util::SetResourceName(blendState.get(), "RainRendering::AlphaBlend");
	}

	if (!rasterizerState) {
		D3D11_RASTERIZER_DESC description{};
		description.FillMode = D3D11_FILL_SOLID;
		description.CullMode = D3D11_CULL_NONE;
		description.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&description, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "RainRendering::Rasterizer");
	}

	if (!depthStencilState) {
		D3D11_DEPTH_STENCIL_DESC description{};
		description.DepthEnable = FALSE;
		description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&description, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "RainRendering::DepthDisabled");
	}

	if (settings.EnableRainCanopyResponse && EnsureCanopyOcclusionResources())
		EnsureCanopyOcclusionShader();

	renderPathReady = perFrameCB && dropBuffer && dropLocalOffsetBuffer && dropGroupOffsetBuffer &&
	                  visibleDropIndexBuffer && indirectDrawArgsBuffer && EnsureShaders();
}

bool RainRendering::EnsureCanopyOcclusionResources()
{
	if (solidCoverOcclusion && canopyClassification && canopyAccumulation)
		return true;
	if (!globals::features::skylighting.loaded)
		return false;

	auto* renderer = globals::game::renderer;
	if (!renderer)
		return false;

	auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
	if (!precipitationOcclusion.texture || !precipitationOcclusion.depthSRV || !precipitationOcclusion.views[0])
		return false;

	D3D11_TEXTURE2D_DESC depthDescription{};
	D3D11_SHADER_RESOURCE_VIEW_DESC depthResourceDescription{};
	D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
	precipitationOcclusion.texture->GetDesc(&depthDescription);
	precipitationOcclusion.depthSRV->GetDesc(&depthResourceDescription);
	precipitationOcclusion.views[0]->GetDesc(&depthViewDescription);

	solidCoverOcclusion = std::make_unique<Texture2D>(depthDescription, "RainRendering::SolidCoverOcclusion");
	solidCoverOcclusion->CreateSRV(depthResourceDescription);
	solidCoverOcclusion->CreateDSV(depthViewDescription);
	globals::d3d::context->ClearDepthStencilView(
		solidCoverOcclusion->dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

	D3D11_TEXTURE3D_DESC fieldDescription{
		.Width = kCanopyProbeWidth,
		.Height = kCanopyProbeHeight,
		.Depth = kCanopyProbeDepth,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R8_UNORM,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.CPUAccessFlags = 0,
		.MiscFlags = 0
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC fieldResourceDescription{
		.Format = fieldDescription.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC fieldViewDescription{
		.Format = fieldDescription.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = fieldDescription.Depth }
	};

	canopyClassification = std::make_unique<Texture3D>(fieldDescription, "RainRendering::CanopyClassification");
	canopyClassification->CreateSRV(fieldResourceDescription);
	canopyClassification->CreateUAV(fieldViewDescription);

	fieldDescription.Format = fieldResourceDescription.Format = fieldViewDescription.Format = DXGI_FORMAT_R8_UINT;
	canopyAccumulation = std::make_unique<Texture3D>(fieldDescription, "RainRendering::CanopyAccumulation");
	canopyAccumulation->CreateSRV(fieldResourceDescription);
	canopyAccumulation->CreateUAV(fieldViewDescription);

	ResetCanopyOcclusion();
	return true;
}

bool RainRendering::EnsureCanopyOcclusionShader()
{
	if (canopyOcclusionCS)
		return true;
	if (canopyOcclusionShaderCompileAttempted)
		return false;
	canopyOcclusionShaderCompileAttempted = true;

	auto* shader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainCanopyOcclusion.hlsl", {}, "cs_5_0", "RainCanopyUpdateCS"));
	if (!shader) {
		logger::warn("[RainRendering] Canopy classification shader unavailable; using conservative roof occlusion");
		return false;
	}

	canopyOcclusionCS.attach(shader);
	Util::SetResourceName(canopyOcclusionCS.get(), "RainRendering::CanopyOcclusionCS");
	return true;
}

void RainRendering::ResetCanopyOcclusion()
{
	if (!canopyClassification || !canopyAccumulation)
		return;

	auto* context = globals::d3d::context;
	const float noCanopy[4]{};
	context->ClearUnorderedAccessViewFloat(canopyClassification->uav.get(), noCanopy);
	const UINT noSamples[4] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(canopyAccumulation->uav.get(), noSamples);
}

void RainRendering::UpdateCanopyOcclusion(
	ID3D11DeviceContext* a_context,
	ID3D11Buffer* a_sharedBuffer,
	ID3D11Buffer* a_frameBuffer)
{
	if (!settings.EnableRainRoofOcclusion || !settings.EnableRainCanopyResponse || !canopyOcclusionCS ||
		!solidCoverOcclusion || !canopyClassification || !canopyAccumulation ||
		!globals::features::skylighting.texOcclusion || !globals::features::skylighting.texOcclusion->srv.get())
		return;

	const bool interior = Util::IsInterior();
	if (!previousCanopyInteriorState || *previousCanopyInteriorState != interior) {
		ResetCanopyOcclusion();
		previousCanopyInteriorState = interior;
	}

	CS_GPU_PASS("RainRendering::CanopyOcclusion");
	a_context->CSSetShader(canopyOcclusionCS.get(), nullptr, 0);
	a_context->CSSetConstantBuffers(5, 1, &a_sharedBuffer);
	a_context->CSSetConstantBuffers(12, 1, &a_frameBuffer);
	ID3D11ShaderResourceView* depthResources[] = {
		globals::features::skylighting.texOcclusion->srv.get(),
		solidCoverOcclusion->srv.get()
	};
	a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(depthResources)), depthResources);
	ID3D11SamplerState* sampler = globals::features::skylighting.comparisonSampler.get();
	a_context->CSSetSamplers(0, 1, &sampler);
	ID3D11UnorderedAccessView* fieldViews[] = {
		canopyClassification->uav.get(),
		canopyAccumulation->uav.get()
	};
	a_context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(fieldViews)), fieldViews, nullptr);
	a_context->Dispatch(
		(kCanopyProbeWidth + 7u) >> 3,
		(kCanopyProbeHeight + 7u) >> 3,
		kCanopyProbeDepth);
	ID3D11ShaderResourceView* nullDepthResources[2]{};
	a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullDepthResources)), nullDepthResources);
	ID3D11UnorderedAccessView* nullFieldViews[2]{};
	a_context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(nullFieldViews)), nullFieldViews, nullptr);
}

bool RainRendering::EnsureShaders()
{
	if (rainUpdateCS && rainCountCS && rainPrefixCS && rainScatterCS && rainVS && rainPS)
		return true;
	if (shaderCompileAttempted)
		return false;
	shaderCompileAttempted = true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::game::isVR)
		defines.emplace_back("VR", "");
	if (globals::features::lightLimitFix.loaded) {
		defines.emplace_back("RAIN_LOCAL_LIGHTS", "");
		if (globals::features::inverseSquareLighting.loaded)
			defines.emplace_back("RAIN_INVERSE_SQUARE", "");
	}
	if (globals::features::skylighting.loaded)
		defines.emplace_back("RAIN_SKYLIGHTING_OCCLUSION", "");

	auto* updateShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainUpdateCS"));
	auto* countShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainCountCS"));
	auto* prefixShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainPrefixCS"));
	auto* scatterShader = static_cast<ID3D11ComputeShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "cs_5_0", "RainScatterCS"));
	auto* vertexShader = static_cast<ID3D11VertexShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "vs_5_0", "RainVS"));
	auto* pixelShader = static_cast<ID3D11PixelShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "ps_5_0", "RainPS"));
	if (!updateShader || !countShader || !prefixShader || !scatterShader || !vertexShader || !pixelShader) {
		if (updateShader)
			updateShader->Release();
		if (countShader)
			countShader->Release();
		if (prefixShader)
			prefixShader->Release();
		if (scatterShader)
			scatterShader->Release();
		if (vertexShader)
			vertexShader->Release();
		if (pixelShader)
			pixelShader->Release();
		logger::error("[RainRendering] Disabling the render path because its runtime shaders failed to compile");
		return false;
	}

	rainUpdateCS.attach(updateShader);
	rainCountCS.attach(countShader);
	rainPrefixCS.attach(prefixShader);
	rainScatterCS.attach(scatterShader);
	rainVS.attach(vertexShader);
	rainPS.attach(pixelShader);
	Util::SetResourceName(rainUpdateCS.get(), "RainRendering::RainUpdateCS");
	Util::SetResourceName(rainCountCS.get(), "RainRendering::RainCountCS");
	Util::SetResourceName(rainPrefixCS.get(), "RainRendering::RainPrefixCS");
	Util::SetResourceName(rainScatterCS.get(), "RainRendering::RainScatterCS");
	Util::SetResourceName(rainVS.get(), "RainRendering::RainVS");
	Util::SetResourceName(rainPS.get(), "RainRendering::RainPS");
	return true;
}

bool RainRendering::EnsureDistantRainShaders()
{
	if (distantRainVS && distantRainPS)
		return true;
	if (distantRainShaderCompileAttempted)
		return false;
	distantRainShaderCompileAttempted = true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::game::isVR)
		defines.emplace_back("VR", "");
	auto* vertexShader = static_cast<ID3D11VertexShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainFarField.hlsl", defines, "vs_5_0", "DistantRainVS"));
	auto* pixelShader = static_cast<ID3D11PixelShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainFarField.hlsl", defines, "ps_5_0", "DistantRainPS"));
	if (!vertexShader || !pixelShader) {
		if (vertexShader)
			vertexShader->Release();
		if (pixelShader)
			pixelShader->Release();
		logger::warn("[RainRendering] Distant rain shaders failed to compile; continuing without the distant field");
		return false;
	}

	distantRainVS.attach(vertexShader);
	distantRainPS.attach(pixelShader);
	Util::SetResourceName(distantRainVS.get(), "RainRendering::DistantRainVS");
	Util::SetResourceName(distantRainPS.get(), "RainRendering::DistantRainPS");
	return true;
}

void RainRendering::ClearShaderCache()
{
	rainUpdateCS = nullptr;
	rainCountCS = nullptr;
	rainPrefixCS = nullptr;
	rainScatterCS = nullptr;
	rainVS = nullptr;
	rainPS = nullptr;
	distantRainVS = nullptr;
	distantRainPS = nullptr;
	sceneColorDownsampleVS = nullptr;
	sceneColorDownsamplePS = nullptr;
	canopyOcclusionCS = nullptr;
	shaderCompileAttempted = false;
	distantRainShaderCompileAttempted = false;
	canopyOcclusionShaderCompileAttempted = false;
	renderPathReady = false;
	rainTextureSRV = nullptr;
	rainTextureLoadAttempted = false;
	sceneColorCopy = nullptr;
	sceneColorRTV = nullptr;
	sceneColorSRV = nullptr;
	sceneDepthCopy = nullptr;
	sceneDepthRTV = nullptr;
	sceneDepthSRV = nullptr;
	sceneColorDescription = {};
	sceneColorViewFormat = DXGI_FORMAT_UNKNOWN;
	sceneColorCopyFailed = false;
}

void RainRendering::RestoreDefaultSettings()
{
	settings = {};
	NormalizeSettings();
}

void RainRendering::LoadSettings(json& o_json)
{
	settings = o_json;
	NormalizeSettings();
}

void RainRendering::SaveSettings(json& o_json)
{
	NormalizeSettings();
	o_json = settings;
}

void RainRendering::NormalizeSettings()
{
	settings.EnableRainRendering = settings.EnableRainRendering ? 1u : 0u;
	settings.ForceRainRendering = settings.ForceRainRendering ? 1u : 0u;
	settings.EnableRainRoofOcclusion = settings.EnableRainRoofOcclusion ? 1u : 0u;
	settings.EnableRainCanopyResponse = settings.EnableRainCanopyResponse ? 1u : 0u;
	settings.EnableDistantRain = settings.EnableDistantRain ? 1u : 0u;
	settings.EnableGlassyRain = settings.EnableGlassyRain ? 1u : 0u;
	settings.EnableRainRefraction = settings.EnableRainRefraction ? 1u : 0u;
	settings.EnableTexturedRain = settings.EnableTexturedRain ? 1u : 0u;

	settings.RainDropCount = std::clamp(settings.RainDropCount, 2048u, kMaximumDropCount);
	settings.RainOverheadDropCount = std::min(settings.RainOverheadDropCount, 64u);
	settings.RainDistantDropCount = std::clamp(settings.RainDistantDropCount, 512u, kMaximumDistantDropCount);
	settings.RainDensity = ClampFinite(settings.RainDensity, 0.0f, 2.0f, 1.0f);
	settings.RainDistantDensity = ClampFinite(settings.RainDistantDensity, 0.0f, 2.0f, 1.0f);
	settings.RainDistantOpacity = ClampFinite(settings.RainDistantOpacity, 0.0f, 1.0f, 0.55f);
	settings.RainDistantStreakLength = ClampFinite(settings.RainDistantStreakLength, 4.0f, 256.0f, 64.0f);
	settings.RainDistantStreakWidth = ClampFinite(settings.RainDistantStreakWidth, 0.2f, 8.0f, 3.0f);
	settings.RainMinimumVisibility = ClampFinite(settings.RainMinimumVisibility, 0.0f, 1.0f, 0.01f);
	settings.RainNearCutoffDistance = ClampFinite(settings.RainNearCutoffDistance, 0.0f, 64.0f, 4.0f);
	settings.RainFarDistance = ClampFinite(settings.RainFarDistance, 2000.0f, 30000.0f, 6000.0f);

	const float nearLayerMaximum = std::min(4000.0f, settings.RainFarDistance * 0.45f);
	settings.RainNearLayerDistance = ClampFinite(settings.RainNearLayerDistance, 200.0f, nearLayerMaximum, 422.0f);
	const float midLayerMinimum = std::max(500.0f, settings.RainNearLayerDistance * 1.25f);
	const float midLayerMaximum = std::min(12000.0f, settings.RainFarDistance * 0.85f);
	settings.RainMidLayerDistance = ClampFinite(settings.RainMidLayerDistance, midLayerMinimum, midLayerMaximum, 2371.0f);

	settings.RainRefractionStrength = ClampFinite(settings.RainRefractionStrength, 0.0f,
		globals::game::isVR ? kMaximumVRRefractionPixels : kMaximumRefractionPixels, 2.68f);

	settings.RainCurtainMinDensity = ClampFinite(settings.RainCurtainMinDensity, 0.0f, 2.0f, 0.28f);
	settings.RainCurtainMaxDensity = ClampFinite(settings.RainCurtainMaxDensity,
		settings.RainCurtainMinDensity, 3.0f, 1.85f);
	settings.RainRoofOcclusionFadeStart = ClampFinite(settings.RainRoofOcclusionFadeStart, 0.0f, 0.99f, 0.20f);
	settings.RainRoofOcclusionFadeEnd = ClampFinite(settings.RainRoofOcclusionFadeEnd,
		settings.RainRoofOcclusionFadeStart + 0.01f, 1.0f, 0.75f);
	settings.RainCanopyDensityScale = ClampFinite(settings.RainCanopyDensityScale, 0.0f, 1.0f, 0.35f);
	settings.RainCanopySpeedScale = ClampFinite(settings.RainCanopySpeedScale, 0.5f, 1.0f, 0.85f);
	settings.RainDebugMode = std::min(settings.RainDebugMode, 8u);
}

void RainRendering::ApplyPerformanceProfile(PerfProfile a_profile)
{
	const auto preset = GetRainPerformancePreset(a_profile);
	settings.RainDropCount = preset.dropCount;
	settings.RainDensity = preset.density;
	settings.RainFarDistance = preset.farDistance;
	NormalizeSettings();
}

bool RainRendering::MatchesPerformanceProfile(PerfProfile a_profile) const
{
	const auto preset = GetRainPerformancePreset(a_profile);
	constexpr float kEpsilon = 1e-4f;
	return settings.RainDropCount == preset.dropCount &&
	       std::abs(settings.RainDensity - preset.density) <= kEpsilon &&
	       std::abs(settings.RainFarDistance - preset.farDistance) <= kEpsilon;
}

std::string RainRendering::GetProfilePreviewText(PerfProfile a_profile) const
{
	const auto preset = GetRainPerformancePreset(a_profile);
	return std::vformat(T(TKEY("profile_preview"), "{} drops, {:.2f} density, {:.0f} unit range"),
		std::make_format_args(preset.dropCount, preset.density, preset.farDistance));
}

void RainRendering::DrawPerformanceSettings()
{
	NormalizeSettings();
	int dropCount = static_cast<int>(settings.RainDropCount);
	if (ImGui::SliderInt(T(TKEY("drop_count"), "Maximum Drop Count"), &dropCount, 2048, static_cast<int>(kMaximumDropCount)))
		settings.RainDropCount = static_cast<uint>(dropCount);
	int overheadDropCount = static_cast<int>(settings.RainOverheadDropCount);
	if (ImGui::SliderInt(T(TKEY("overhead_drop_count"), "Overhead Drop Count"), &overheadDropCount, 0, 64))
		settings.RainOverheadDropCount = static_cast<uint>(overheadDropCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("overhead_drop_count_tooltip"), "Reserves this many near-layer particles for a softly faded volume above the player. The total drop count does not increase."));
	ImGui::SliderFloat(T(TKEY("density"), "Rain Density"), &settings.RainDensity, 0.0f, 2.0f, "%.2f");
	if (ImGui::SliderFloat(T(TKEY("far_distance"), "Rain Far Distance"), &settings.RainFarDistance, 2000.0f, 30000.0f, "%.0f"))
		NormalizeSettings();
	if (ImGui::TreeNodeEx(T(TKEY("layer_budgets"), "Depth Layer Budgets"))) {
		const float nearLayerMaximum = std::min(4000.0f, settings.RainFarDistance * 0.45f);
		if (ImGui::SliderFloat(T(TKEY("near_layer_distance"), "Near Layer Distance"), &settings.RainNearLayerDistance, 200.0f, nearLayerMaximum, "%.0f units"))
			NormalizeSettings();
		const float midLayerMinimum = std::max(500.0f, settings.RainNearLayerDistance * 1.25f);
		const float midLayerMaximum = std::min(12000.0f, settings.RainFarDistance * 0.85f);
		ImGui::SliderFloat(T(TKEY("mid_layer_distance"), "Mid Layer Distance"), &settings.RainMidLayerDistance, midLayerMinimum, midLayerMaximum, "%.0f units");
		ImGui::SliderFloat(T(TKEY("near_budget"), "Near Budget Weight"), &settings.RainNearBudgetWeight, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("mid_budget"), "Mid Budget Weight"), &settings.RainMidBudgetWeight, 0.0f, 4.0f, "%.2f");
		ImGui::BeginDisabled(settings.EnableDistantRain != 0);
		ImGui::SliderFloat(T(TKEY("far_budget"), "Far Budget Weight"), &settings.RainFarBudgetWeight, 0.0f, 4.0f, "%.2f");
		ImGui::EndDisabled();
		const auto counts = GetLayerDropCounts();
		const auto radii = GetLayerRadii(ClampFinite(settings.RainFarDistance, 1000.0f, 50000.0f, 12000.0f));
		ImGui::Text(T(TKEY("allocated_drops"), "Reserved drops: %u near / %u mid / %u far"), counts[0], counts[1], counts[2]);
		ImGui::Text(T(TKEY("effective_layer_radii"), "Effective outer ranges: %.0f / %.0f / %.0f"), radii.x, radii.y, radii.z);
		ImGui::TextWrapped(T(TKEY("layer_budget_help"), "Weights split the existing maximum drop count. Adjacent layers overlap and fade smoothly; near/mid ranges are limited by the far range."));
		ImGui::TreePop();
	}
	NormalizeSettings();
}

std::array<uint32_t, 4> RainRendering::GetLayerDropCounts() const
{
	const uint32_t count = std::clamp(settings.RainDropCount, 1u, kMaximumDropCount);
	float nearWeight = ClampFinite(settings.RainNearBudgetWeight, 0.0f, 4.0f, 1.02f);
	float midWeight = ClampFinite(settings.RainMidBudgetWeight, 0.0f, 4.0f, 1.11f);
	const bool distantFieldReady = settings.EnableDistantRain && settings.RainDebugMode == 0 && distantRainVS && distantRainPS;
	float farWeight = distantFieldReady ? 0.0f : ClampFinite(settings.RainFarBudgetWeight, 0.0f, 4.0f, 0.25f);
	if (nearWeight + midWeight + farWeight < 1e-4f) {
		nearWeight = 1.0f;
		midWeight = 2.0f;
		farWeight = distantFieldReady ? 0.0f : 1.0f;
	}
	const float totalWeight = nearWeight + midWeight + farWeight;
	const float candidates = static_cast<float>(count);
	const uint32_t nearCount = std::min(count, static_cast<uint32_t>(candidates * (nearWeight / totalWeight)));
	const uint32_t midEnd = std::clamp(static_cast<uint32_t>(candidates * ((nearWeight + midWeight) / totalWeight)), nearCount, count);
	return { nearCount, midEnd - nearCount, count - midEnd, count };
}

float4 RainRendering::GetLayerRadii(float a_farDistance) const
{
	const float nearDistance = ClampFinite(settings.RainNearLayerDistance, 200.0f, a_farDistance * 0.45f, 422.0f);
	const float midDistance = ClampFinite(settings.RainMidLayerDistance, nearDistance * 1.25f, a_farDistance * 0.85f, 2371.0f);
	return { nearDistance, midDistance, a_farDistance, 0.2f };
}

void RainRendering::ApplyGlassyReferenceSettings()
{
	settings.EnableRainRendering = 1;
	settings.EnableGlassyRain = 1;
	settings.EnableTexturedRain = 1;
	settings.EnableRainRefraction = 1;
	settings.RainStreakWidth = 3.8f;
	settings.RainStreakLength = 72.0f;
	settings.RainVelocityStretch = 0.045f;
	settings.RainOpacity = 0.75f;
	settings.RainBrightness = 0.85f;
	settings.RainMinimumVisibility = 0.20f;
	settings.RainCoreDarkening = 0.08f;
	settings.RainEdgeHighlight = 1.0f;
	settings.RainRefractionStrength = globals::game::isVR ? 3.0f : 6.0f;
	settings.RainRefractionDistance = 4800.0f;
	settings.RainTextureNormalStrength = 1.0f;
	settings.RainTextureReflectionStrength = 1.0f;
	settings.RainTextureUVWidth = 1.0f;
	settings.RainEnvironmentTransmission = 0.8f;
	settings.RainSceneRefractionMix = 1.0f;
	settings.RainHighlightRoughness = 0.18f;
	settings.RainLightScattering = 0.25f;
	settings.RainLocalLightResponse = 1.0f;
	settings.RainDebugMode = 0;
}

void RainRendering::DrawSettings()
{
	NormalizeSettings();
	DrawFlagCheckbox(T(TKEY("enable"), "Enable Airborne Rain"), settings.EnableRainRendering);
	DrawFlagCheckbox(T(TKEY("force_rain"), "Force Rain for Testing"), settings.ForceRainRendering);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("force_rain_tooltip"), "Renders at full intensity regardless of the current weather or location."));
	if (shaderCompileAttempted && !renderPathReady) {
		Util::Text::Error("%s", T(TKEY("shader_compile_error"), "Airborne Rain shaders failed to compile, so the effect is not rendering."));
		if (ImGui::Button(T(TKEY("retry_shaders"), "Retry Rain Shaders")))
			ClearShaderCache();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("retry_shaders_tooltip"), "Clears the failed compile state and retries when rain next renders."));
	}

	if (ImGui::TreeNodeEx(T(TKEY("volume_density"), "Volume & Density"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawPerformanceSettings();
		ImGui::SliderFloat(T(TKEY("fall_speed"), "Fall Speed"), &settings.RainFallSpeed, 500.0f, 6000.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("fall_speed_tooltip"), "Fall speed for standard Skyrim rain. The active weather's gravity scales this value; Force Rain uses it directly."));
		ImGui::SliderFloat(T(TKEY("near_cutoff_distance"), "Near Cutoff Distance"), &settings.RainNearCutoffDistance, 0.0f, 64.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("near_cutoff_distance_tooltip"), "Hard-discards rain streaks that come within this distance of the head. There is no opacity fade; zero disables the cutoff."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distant_rain"), "Distant Rain Field"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawFlagCheckbox(T(TKEY("distant_rain_enable"), "Enable Distant Rain Field"), settings.EnableDistantRain);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("distant_rain_tooltip"), "Replaces detailed far-layer drops with inexpensive world-space streaks shared by both VR eyes."));
		ImGui::BeginDisabled(!settings.EnableDistantRain);
		int distantDropCount = static_cast<int>(settings.RainDistantDropCount);
		if (ImGui::SliderInt(T(TKEY("distant_drop_count"), "Distant Drop Count"), &distantDropCount, 512, static_cast<int>(kMaximumDistantDropCount)))
			settings.RainDistantDropCount = static_cast<uint>(distantDropCount);
		ImGui::SliderFloat(T(TKEY("distant_density"), "Distant Density"), &settings.RainDistantDensity, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("distant_opacity"), "Distant Opacity"), &settings.RainDistantOpacity, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("distant_streak_length"), "Distant Streak Length"), &settings.RainDistantStreakLength, 4.0f, 256.0f, "%.1f");
		ImGui::SliderFloat(T(TKEY("distant_streak_width"), "Distant Streak Width"), &settings.RainDistantStreakWidth, 0.2f, 8.0f, "%.2f");
		ImGui::EndDisabled();
		if (settings.EnableDistantRain && distantRainShaderCompileAttempted && (!distantRainVS || !distantRainPS)) {
			ImGui::TextDisabled("%s", T(TKEY("distant_rain_unavailable"), "Distant field shaders are unavailable; detailed far rain remains active."));
			if (ImGui::Button(T(TKEY("retry_distant_rain_shaders"), "Retry Distant Rain Shaders"))) {
				distantRainVS = nullptr;
				distantRainPS = nullptr;
				distantRainShaderCompileAttempted = false;
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("streaks"), "Streak Appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button(T(TKEY("glassy_reference"), "Apply Glassy Reference Look")))
			ApplyGlassyReferenceSettings();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("glassy_reference_tooltip"), "Enables textured/refraction rain and tunes width, stretch, opacity, brightness, curvature and reflection for clearer water bodies. Keeps density, drop count, weather forcing and volume unchanged."));
		ImGui::SliderFloat(T(TKEY("streak_length"), "Base Streak Length"), &settings.RainStreakLength, 4.0f, 500.0f, "%.1f");
		ImGui::SliderFloat(T(TKEY("velocity_stretch"), "Velocity Stretch"), &settings.RainVelocityStretch, 0.0f, 0.25f, "%.3f");
		ImGui::SliderFloat(T(TKEY("streak_width"), "Streak Width"), &settings.RainStreakWidth, 0.2f, 8.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("opacity"), "Rain Opacity"), &settings.RainOpacity, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("brightness"), "Rain Brightness"), &settings.RainBrightness, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("lighting_response"), "Rain Lighting Response"), &settings.RainLightingResponse, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("minimum_visibility"), "Minimum Dark-Scene Visibility"), &settings.RainMinimumVisibility, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("minimum_visibility_tooltip"), "Keeps restrained water highlights visible when a weather or post-processing preset makes environmental lighting nearly black. This affects shaped highlights, not the whole streak."));
		ImGui::SliderFloat(T(TKEY("intersection_fade"), "Geometry Intersection Fade"), &settings.RainIntersectionFadeDistance, 1.0f, 400.0f, "%.0f");
		DrawFlagCheckbox(T(TKEY("glassy_rain"), "Glassy Rain"), settings.EnableGlassyRain);
		const bool usesWaterMaterial = UsesWaterMaterial(settings);
		if (!settings.EnableGlassyRain)
			ImGui::TextUnformatted(T(TKEY("glassy_inactive"), "Material: standard streaks (Glassy Rain is off)"));
		else if (!usesWaterMaterial)
			ImGui::TextUnformatted(T(TKEY("glassy_debug_suspended"), "Material: diagnostic output replaces the water material in this debug mode"));
		else
			ImGui::TextUnformatted(T(TKEY("glassy_active"), "Material: transparent water"));

		ImGui::BeginDisabled(!usesWaterMaterial);
		DrawFlagCheckbox(T(TKEY("textured_rain"), "Textured Water Drops"), settings.EnableTexturedRain);
		DrawFlagCheckbox(T(TKEY("refraction"), "Nearby Rain Refraction"), settings.EnableRainRefraction);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("refraction_tooltip"), "Adds a scene-color copy and depth-checked background distortion inside nearby world-space streaks. Each eye samples only its own view."));
		ImGui::BeginDisabled(!settings.EnableRainRefraction);
		ImGui::SliderFloat(T(TKEY("refraction_strength"), "Refraction Strength"), &settings.RainRefractionStrength, 0.0f,
			globals::game::isVR ? kMaximumVRRefractionPixels : kMaximumRefractionPixels, "%.2f pixels");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("refraction_strength_tooltip"), "Maximum background displacement inside a drop, also bounded by its projected width. VR uses a lower limit. Foreground depth and eye boundaries are always respected."));
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (usesWaterMaterial && settings.EnableRainRefraction && sceneColorCopyFailed)
			ImGui::TextDisabled("%s", T(TKEY("refraction_unavailable"), "Scene-color copy is unavailable; rain refraction is disabled."));

		if (ImGui::TreeNodeEx(T(TKEY("advanced_water_material"), "Advanced Water Material"))) {
			ImGui::BeginDisabled(!usesWaterMaterial);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("textured_rain_tooltip"), "Uses RainDrop.png as a normal/opacity map on nearby world-space streaks. Distant rain uses a simpler water surface, preserving transparency. Reuses Dynamic Cubemaps when available."));
			ImGui::BeginDisabled(!settings.EnableTexturedRain);
			ImGui::SliderFloat(T(TKEY("texture_normals"), "Drop Curvature"), &settings.RainTextureNormalStrength, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("texture_reflections"), "Water Reflection Strength"), &settings.RainTextureReflectionStrength, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("texture_uv_width"), "Texture UV Width"), &settings.RainTextureUVWidth, 0.1f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("texture_uv_width_tooltip"), "Samples the centered portion of the texture to trim transparent side padding. Use 1.0 for the round drop map; narrower values crop its curved edges."));
			if (ImGui::Button(T(TKEY("reload_rain_texture"), "Reload Drop Texture"))) {
				rainTextureSRV = nullptr;
				rainTextureLoadAttempted = false;
			}
			if (rainTextureLoadAttempted && !rainTextureSRV)
				ImGui::TextUnformatted(T(TKEY("rain_texture_unavailable"), "Drop texture unavailable; using procedural rain."));
			ImGui::EndDisabled();
			ImGui::SliderFloat(T(TKEY("environment_transmission"), "Environment Transmission"), &settings.RainEnvironmentTransmission, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("environment_transmission_tooltip"), "Cubemap transmission fills the portion not using nearby scene distortion, including distant rain. It does not reduce Scene Distortion Mix. Zero leaves that portion as clear background transmission."));
			if (settings.RainEnvironmentTransmission > 0.0f && !GetRainEnvironment())
				ImGui::TextUnformatted(T(TKEY("rain_environment_unavailable"), "Environment cubemap unavailable; using background transmission."));
			ImGui::SliderFloat(T(TKEY("core_darkening"), "Translucent Core Darkening"), &settings.RainCoreDarkening, 0.0f, 0.8f, "%.2f");
			ImGui::SliderFloat(T(TKEY("edge_highlight"), "Edge Highlight"), &settings.RainEdgeHighlight, 0.0f, 4.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("streak_variation"), "Streak Variation"), &settings.RainStreakVariation, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("local_light_response"), "Local Light Response"), &settings.RainLocalLightResponse, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("local_light_response_tooltip"), "Reuses the light grid once per drop for colored scattering and an intensity-weighted highlight direction. Separate from cubemap reflections. Unshadowed approximation; spot and portal-restricted lights are excluded."));
			ImGui::SliderFloat(T(TKEY("highlight_roughness"), "Water Highlight Roughness"), &settings.RainHighlightRoughness, 0.08f, 0.6f, "%.2f");
			ImGui::SliderFloat(T(TKEY("light_scattering"), "Light Scattering"), &settings.RainLightScattering, 0.0f, 1.0f, "%.2f");
			ImGui::BeginDisabled(!settings.EnableRainRefraction);
			ImGui::SliderFloat(T(TKEY("scene_distortion_mix"), "Scene Distortion Mix"), &settings.RainSceneRefractionMix, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("scene_distortion_mix_tooltip"), "Transmission taken from the distorted scene inside nearby drops. One gives scene distortion priority over cubemap transmission; Rain Opacity controls the drop's coverage."));
			ImGui::SliderFloat(T(TKEY("refraction_distance"), "Glassy Detail Distance"), &settings.RainRefractionDistance, 256.0f, 6000.0f, "%.0f units");
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("rain_curtains"), "Rain Curtains"))) {
		ImGui::SliderFloat(T(TKEY("density_noise_scale"), "Density Noise Scale"), &settings.RainDensityNoiseScale, 256.0f, 16000.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("density_noise_strength"), "Density Noise Strength"), &settings.RainDensityNoiseStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("curtain_scale"), "Curtain Scale"), &settings.RainCurtainScale, 512.0f, 30000.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("curtain_strength"), "Curtain Strength"), &settings.RainCurtainStrength, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("curtain_contrast"), "Curtain Contrast"), &settings.RainCurtainContrast, 0.25f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("curtain_min_density"), "Curtain Minimum Density"), &settings.RainCurtainMinDensity, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("curtain_max_density"), "Curtain Maximum Density"), &settings.RainCurtainMaxDensity, 0.0f, 3.0f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("advanced_developer"), "Advanced & Developer"))) {
		DrawFlagCheckbox(T(TKEY("roof_occlusion"), "Skylighting Roof Occlusion"), settings.EnableRainRoofOcclusion);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("roof_occlusion_tooltip"), "Uses a rain-specific solid-cover field to block rain beneath enclosing geometry without treating animated tree canopies as roofs. Toggle for GPU A/B profiling."));
		if (!globals::features::skylighting.loaded)
			ImGui::TextDisabled("%s", T(TKEY("roof_occlusion_unavailable"), "Skylighting is unavailable; roof occlusion falls back to scene depth only."));
		ImGui::BeginDisabled(!settings.EnableRainRoofOcclusion);
		if (ImGui::SliderFloat(T(TKEY("roof_occlusion_fade_start"), "Roof Fade Start"), &settings.RainRoofOcclusionFadeStart, 0.0f, 0.99f, "%.2f"))
			NormalizeSettings();
		ImGui::SliderFloat(T(TKEY("roof_occlusion_fade_end"), "Roof Fade End"), &settings.RainRoofOcclusionFadeEnd,
			settings.RainRoofOcclusionFadeStart + 0.01f, 1.0f, "%.2f");
		DrawFlagCheckbox(T(TKEY("canopy_response"), "Tree Canopy Response"), settings.EnableRainCanopyResponse);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("canopy_response_tooltip"), "Classifies animated tree cover separately from solid roofs. Canopies thin and shorten rain while buildings continue to block it."));
		ImGui::BeginDisabled(!settings.EnableRainCanopyResponse);
		ImGui::SliderFloat(T(TKEY("canopy_density"), "Canopy Rain Density"), &settings.RainCanopyDensityScale, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("canopy_speed"), "Canopy Fall Speed"), &settings.RainCanopySpeedScale, 0.5f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		const char* debugModes[] = {
			T(TKEY("debug_off"), "Off"),
			T(TKEY("debug_positions"), "Drop Positions"),
			T(TKEY("debug_velocity"), "Drop Velocity"),
			T(TKEY("debug_density"), "Density Field"),
			T(TKEY("debug_curtains"), "Curtain Field"),
			T(TKEY("debug_lod"), "Distance LOD"),
			T(TKEY("debug_refraction"), "Actual Scene Distortion"),
			T(TKEY("debug_lighting"), "Local Light Contribution"),
			T(TKEY("debug_water_normals"), "Water Surface Normals")
		};
		int debugMode = static_cast<int>(settings.RainDebugMode);
		if (ImGui::Combo(T(TKEY("debug_mode"), "Debug Visualization"), &debugMode, debugModes, static_cast<int>(std::size(debugModes))))
			settings.RainDebugMode = static_cast<uint>(debugMode);
		if (settings.RainDebugMode >= 6)
			ImGui::TextWrapped("%s", T(TKEY("water_debug_help"), "Water diagnostics require Glassy Rain. Distortion: red = horizontal displacement, green = vertical; black = no effective distortion. Local Light Contribution excludes sunlight and cubemap reflections."));
		ImGui::TreePop();
	}
	NormalizeSettings();
}

RainRendering::WeatherRainState RainRendering::GetWeatherRainState() const
{
	WeatherRainState state{};
	const auto* sky = globals::game::sky;
	if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || Util::IsInterior() ||
		sky->flags.any(RE::Sky::Flags::kHideSky) || !sky->precip)
		return state;

	struct WeatherSample
	{
		float intensity = 0.0f;
		float gravity = kReferenceRainGravity;
	};
	const auto getWeatherSample = [](const RE::TESWeather* a_weather) {
		WeatherSample sample{};
		if (!a_weather || !a_weather->precipitationData)
			return sample;
		const auto particleType = a_weather->precipitationData->GetSettingValue(
																  RE::BGSShaderParticleGeometryData::DataID::kParticleType)
		                              .i;
		if (particleType != static_cast<uint32_t>(RE::BGSShaderParticleGeometryData::ParticleType::kRain))
			return sample;

		const float density = a_weather->precipitationData->GetSettingValue(
															  RE::BGSShaderParticleGeometryData::DataID::kParticleDensity)
		                          .f;
		if (std::isfinite(density) && density > 0.0f)
			sample.intensity = std::min(1.0f, density / kMaximumRainParticleDensity);
		const float gravity = a_weather->precipitationData->GetSettingValue(
															  RE::BGSShaderParticleGeometryData::DataID::kGravityVelocity)
		                          .f;
		if (std::isfinite(gravity) && gravity > 0.0f)
			sample.gravity = gravity;
		return sample;
	};

	const WeatherSample currentSample = getWeatherSample(sky->currentWeather);
	float currentIntensity = 0.0f;
	if (sky->currentWeather && currentSample.intensity > 0.0f) {
		const float fadeStart = sky->currentWeather->data.precipitationBeginFadeIn * (1.0f / 255.0f);
		currentIntensity = currentSample.intensity *
		                   LinearStep(fadeStart, 1.0f, sky->currentWeatherPct);
	}

	const WeatherSample previousSample = getWeatherSample(sky->lastWeather);
	float previousIntensity = 0.0f;
	if (sky->lastWeather && previousSample.intensity > 0.0f) {
		const float fadeEnd = sky->lastWeather->data.precipitationEndFadeOut * (1.0f / 255.0f);
		previousIntensity = previousSample.intensity *
		                    (1.0f - LinearStep(0.0f, fadeEnd, sky->currentWeatherPct));
	}

	const float combinedIntensity = currentIntensity + previousIntensity;
	if (!std::isfinite(combinedIntensity) || combinedIntensity <= 0.0f)
		return state;
	state.intensity = std::clamp(combinedIntensity, 0.0f, 1.0f);
	const float blendedGravity =
		(currentSample.gravity * currentIntensity + previousSample.gravity * previousIntensity) /
		combinedIntensity;
	state.fallSpeedScale = std::clamp(blendedGravity / kReferenceRainGravity, 0.25f, 4.0f);
	return state;
}

float RainRendering::GetWeatherIntensity() const
{
	return GetWeatherRainState().intensity;
}

bool RainRendering::ReplacesVanillaRain(const RE::TESWeather* a_weather) const
{
	if (!loaded || !settings.EnableRainRendering || !a_weather || !a_weather->precipitationData)
		return false;
	const auto particleType = a_weather->precipitationData->GetSettingValue(
															  RE::BGSShaderParticleGeometryData::DataID::kParticleType)
	                              .i;
	return particleType == static_cast<uint32_t>(RE::BGSShaderParticleGeometryData::ParticleType::kRain);
}

float3 RainRendering::GetRainLightColor() const
{
	if (const auto* sky = globals::game::sky) {
		const auto& sunlight = sky->skyColor[static_cast<uint>(RE::TESWeather::ColorTypes::kSunlight)];
		return {
			ClampFinite(sunlight.red, 0.0f, 8.0f, 1.0f),
			ClampFinite(sunlight.green, 0.0f, 8.0f, 1.0f),
			ClampFinite(sunlight.blue, 0.0f, 8.0f, 1.0f)
		};
	}
	return { 1.0f, 1.0f, 1.0f };
}

bool RainRendering::EnsureRainSampler()
{
	if (refractionSampler)
		return true;
	D3D11_SAMPLER_DESC description{};
	description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	description.AddressU = description.AddressV = description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	description.MaxAnisotropy = 1;
	description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	description.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(globals::d3d::device->CreateSamplerState(&description, refractionSampler.put())))
		return false;
	Util::SetResourceName(refractionSampler.get(), "RainRendering::LinearClampSampler");
	return true;
}

bool RainRendering::EnsureRainTexture()
{
	if (rainTextureLoadAttempted)
		return rainTextureSRV != nullptr;
	rainTextureLoadAttempted = true;
	ImVec2 dimensions{};
	// The shared PNG loader preserves linear normal data and creates the mip chain once.
	if (!Util::LoadTextureFromFile(globals::d3d::device, kRainTexturePath, rainTextureSRV.put(), dimensions) || !EnsureRainSampler()) {
		rainTextureSRV = nullptr;
		logger::warn("[RainRendering] Drop texture unavailable: {}; using procedural rain", kRainTexturePath);
		return false;
	}
	rainTextureSize = { dimensions.x, dimensions.y };
	winrt::com_ptr<ID3D11Resource> resource;
	rainTextureSRV->GetResource(resource.put());
	Util::SetResourceName(resource.get(), "RainRendering::DropNormalOpacity");
	Util::SetResourceName(rainTextureSRV.get(), "RainRendering::DropNormalOpacity SRV");
	logger::info("[RainRendering] Loaded drop normal/opacity texture: {} ({}x{})", kRainTexturePath, dimensions.x, dimensions.y);
	return true;
}

ID3D11ShaderResourceView* RainRendering::GetRainEnvironment() const
{
	const auto& cubemaps = globals::features::dynamicCubemaps;
	const auto* environment = cubemaps.activeReflections ? cubemaps.envReflectionsTextureBC6H : cubemaps.envTextureBC6H;
	return cubemaps.loaded && environment ? environment->srv.get() : nullptr;
}

bool RainRendering::EnsureSceneColorShaders()
{
	if (sceneColorDownsampleVS && sceneColorDownsamplePS)
		return true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::game::isVR)
		defines.emplace_back("VR", "");
	auto* vertexShader = static_cast<ID3D11VertexShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "vs_5_0", "RainSceneColorVS"));
	auto* pixelShader = static_cast<ID3D11PixelShader*>(Util::CompileShader(
		L"Data\\Shaders\\RainRendering\\RainRendering.hlsl", defines, "ps_5_0", "RainSceneColorPS"));
	if (!vertexShader || !pixelShader) {
		if (vertexShader)
			vertexShader->Release();
		if (pixelShader)
			pixelShader->Release();
		return false;
	}

	sceneColorDownsampleVS.attach(vertexShader);
	sceneColorDownsamplePS.attach(pixelShader);
	Util::SetResourceName(sceneColorDownsampleVS.get(), "RainRendering::SceneColorDownsampleVS");
	Util::SetResourceName(sceneColorDownsamplePS.get(), "RainRendering::SceneColorDownsamplePS");
	return true;
}

bool RainRendering::EnsureSceneColorCopy(ID3D11Texture2D* a_source, ID3D11RenderTargetView* a_view)
{
	D3D11_TEXTURE2D_DESC sourceDescription{};
	a_source->GetDesc(&sourceDescription);
	D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
	a_view->GetDesc(&viewDescription);
	if (sourceDescription.SampleDesc.Count != 1 || sourceDescription.ArraySize != 1)
		return false;

	const bool changed = sourceDescription.Width != sceneColorDescription.Width ||
	                     sourceDescription.Height != sceneColorDescription.Height ||
	                     sourceDescription.Format != sceneColorDescription.Format || viewDescription.Format != sceneColorViewFormat;
	if (!changed && (sceneColorSRV || sceneColorCopyFailed))
		return sceneColorSRV && sceneDepthSRV;

	sceneColorCopy = nullptr;
	sceneColorRTV = nullptr;
	sceneColorSRV = nullptr;
	sceneDepthCopy = nullptr;
	sceneDepthRTV = nullptr;
	sceneDepthSRV = nullptr;
	sceneColorDescription = sourceDescription;
	sceneColorViewFormat = viewDescription.Format;
	sceneColorCopyFailed = false;
	if (!EnsureSceneColorShaders()) {
		sceneColorCopyFailed = true;
		logger::warn("[RainRendering] Refraction downsample shaders are unavailable; using shading-only rain");
		return false;
	}
	D3D11_TEXTURE2D_DESC copyDescription = sourceDescription;
	const UINT sourceEyeWidth = globals::game::isVR ? (sourceDescription.Width + 1u) / 2u : sourceDescription.Width;
	copyDescription.Width = globals::game::isVR ? ((sourceEyeWidth + 1u) / 2u) * 2u : (sourceDescription.Width + 1u) / 2u;
	copyDescription.Height = (sourceDescription.Height + 1u) / 2u;
	copyDescription.MipLevels = 1;
	copyDescription.Usage = D3D11_USAGE_DEFAULT;
	copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	copyDescription.CPUAccessFlags = 0;
	copyDescription.MiscFlags = 0;
	auto* device = globals::d3d::device;
	HRESULT result = device->CreateTexture2D(&copyDescription, nullptr, sceneColorCopy.put());
	if (SUCCEEDED(result)) {
		Util::SetResourceName(sceneColorCopy.get(), "RainRendering::HalfResolutionSceneColor");
		D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
		rtvDescription.Format = viewDescription.Format;
		rtvDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		result = device->CreateRenderTargetView(sceneColorCopy.get(), &rtvDescription, sceneColorRTV.put());
	}
	if (SUCCEEDED(result)) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
		srvDescription.Format = viewDescription.Format;
		srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDescription.Texture2D.MipLevels = 1;
		result = device->CreateShaderResourceView(sceneColorCopy.get(), &srvDescription, sceneColorSRV.put());
	}
	D3D11_TEXTURE2D_DESC depthDescription = copyDescription;
	depthDescription.Format = DXGI_FORMAT_R32_FLOAT;
	if (SUCCEEDED(result))
		result = device->CreateTexture2D(&depthDescription, nullptr, sceneDepthCopy.put());
	if (SUCCEEDED(result)) {
		Util::SetResourceName(sceneDepthCopy.get(), "RainRendering::HalfResolutionSceneDepth");
		result = device->CreateRenderTargetView(sceneDepthCopy.get(), nullptr, sceneDepthRTV.put());
	}
	if (SUCCEEDED(result))
		result = device->CreateShaderResourceView(sceneDepthCopy.get(), nullptr, sceneDepthSRV.put());
	if (SUCCEEDED(result) && !EnsureRainSampler())
		result = E_FAIL;
	if (FAILED(result)) {
		sceneColorCopy = nullptr;
		sceneColorRTV = nullptr;
		sceneColorSRV = nullptr;
		sceneDepthCopy = nullptr;
		sceneDepthRTV = nullptr;
		sceneDepthSRV = nullptr;
		sceneColorCopyFailed = true;
		logger::warn("[RainRendering] Refraction unavailable (HRESULT {:#x}); using shading-only rain", static_cast<uint32_t>(result));
		return false;
	}
	Util::SetResourceName(sceneColorRTV.get(), "RainRendering::HalfResolutionSceneColor RTV");
	Util::SetResourceName(sceneColorSRV.get(), "RainRendering::HalfResolutionSceneColor SRV");
	Util::SetResourceName(sceneDepthRTV.get(), "RainRendering::HalfResolutionSceneDepth RTV");
	Util::SetResourceName(sceneDepthSRV.get(), "RainRendering::HalfResolutionSceneDepth SRV");
	return true;
}

void RainRendering::DownsampleSceneColor(
	ID3D11ShaderResourceView* a_color, ID3D11ShaderResourceView* a_depth, const float2& a_size)
{
	CS_GPU_PASS("RainRendering::SceneColorDownsample");
	auto* context = globals::d3d::context;
	std::array<ID3D11RenderTargetView*, 2> renderTargets{ sceneColorRTV.get(), sceneDepthRTV.get() };
	context->OMSetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), nullptr);
	context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	const float sourceEyeWidth = globals::game::isVR ? std::ceil(a_size.x * 0.5f) : a_size.x;
	const float targetWidth = globals::game::isVR ? std::ceil(sourceEyeWidth * 0.5f) * 2.0f : std::ceil(a_size.x * 0.5f);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, targetWidth, std::ceil(a_size.y * 0.5f), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(sceneColorDownsampleVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(sceneColorDownsamplePS.get(), nullptr, 0);
	ID3D11Buffer* rainBuffer = perFrameCB->CB();
	context->PSSetConstantBuffers(0, 1, &rainBuffer);
	context->PSSetShaderResources(0, 1, &a_depth);
	context->PSSetShaderResources(2, 1, &a_color);
	ID3D11SamplerState* sampler = refractionSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->Draw(3, 0);
	ID3D11ShaderResourceView* nullResource = nullptr;
	context->PSSetShaderResources(0, 1, &nullResource);
	context->PSSetShaderResources(2, 1, &nullResource);
}

void RainRendering::UpdateGlassyConstants(PerFrame& a_data, const D3D11_TEXTURE2D_DESC& a_description, const float2& a_size, bool a_hasSceneColor) const
{
	const bool glassy = UsesWaterMaterial(settings);
	a_data.Glassy = { glassy ? 1.0f : 0.0f,
		ClampFinite(settings.RainCoreDarkening, 0.0f, 0.8f, 0.35f),
		ClampFinite(settings.RainEdgeHighlight, 0.0f, 4.0f, 1.5f),
		ClampFinite(settings.RainRefractionStrength, 0.0f, globals::game::isVR ? kMaximumVRRefractionPixels : kMaximumRefractionPixels, 2.68f) };
	a_data.Refraction = { ClampFinite(settings.RainRefractionDistance, 256.0f, 6000.0f, 2400.0f),
		a_hasSceneColor ? 1.0f : 0.0f,
		glassy ? ClampFinite(settings.RainStreakVariation, 0.0f, 1.0f, 0.45f) : 0.0f,
		ClampFinite(settings.RainEnvironmentTransmission, 0.0f, 1.0f, 0.8f) };
	a_data.ScreenSize = { a_size.x, a_size.y, 1.0f / a_description.Width, 1.0f / a_description.Height };
	a_data.TexturedRain = { glassy && settings.EnableTexturedRain && rainTextureSRV ? 1.0f : 0.0f,
		ClampFinite(settings.RainTextureNormalStrength, 0.0f, 2.0f, 2.0f),
		ClampFinite(settings.RainTextureReflectionStrength, 0.0f, 2.0f, 1.0f), refractionSampler && GetRainEnvironment() ? 1.0f : 0.0f };
	a_data.RainTextureShape = { rainTextureSize.x, rainTextureSize.y,
		ClampFinite(settings.RainTextureUVWidth, 0.1f, 1.0f, 1.0f), 0.0f };
	a_data.MaterialLighting = { ClampFinite(settings.RainHighlightRoughness, 0.08f, 0.6f, 0.18f),
		ClampFinite(settings.RainLightScattering, 0.0f, 1.0f, 0.25f),
		ClampFinite(settings.RainSceneRefractionMix, 0.0f, 1.0f, 1.0f), 0.0f };
	const auto& lightLimitFix = globals::features::lightLimitFix;
	if (glassy && lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightGrid && lightLimitFix.lightIndexList) {
		a_data.LocalLighting = { ClampFinite(settings.RainLocalLightResponse, 0.0f, 2.0f, 0.6f),
			a_data.Refraction.x, std::max(lightLimitFix.lightsNear, 0.1f), std::max(lightLimitFix.lightsFar, lightLimitFix.lightsNear + 1.0f) };
		a_data.LightGrid = { lightLimitFix.clusterSize[0], lightLimitFix.clusterSize[1], lightLimitFix.clusterSize[2], lightLimitFix.lightCount };
	}
}

void RainRendering::DrawBeforeWater()
{
	const uint32_t frame = globals::state->frameCount;
	const bool waterWasRecentlyBlended =
		lastWaterBlendFrame != UINT32_MAX && frame - lastWaterBlendFrame <= 1u;
	if (!waterWasRecentlyBlended)
		DrawRain();
}

void RainRendering::DrawAfterWater()
{
	lastWaterBlendFrame = globals::state->frameCount;
	DrawRain();
}

void RainRendering::DrawRain()
{
	const uint32_t frame = globals::state->frameCount;
	if (lastDrawFrame == frame)
		return;

	if (!settings.EnableRainRendering || globals::state->IsFullScreenMenuOpen())
		return;

	const WeatherRainState weather = settings.ForceRainRendering ? WeatherRainState{ 1.0f, 1.0f } : GetWeatherRainState();
	if (weather.intensity <= 0.0f)
		return;

	auto* renderer = globals::game::renderer;
	auto* context = globals::d3d::context;
	auto* frameBuffer = globals::game::perFrame.get() ? *globals::game::perFrame.get() : nullptr;
	if (!renderer || !context || !frameBuffer)
		return;
	auto& mainTarget = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& stableWorldDepth =
		renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto* depthResource = stableWorldDepth.depthSRV ? stableWorldDepth.depthSRV : mainDepth.depthSRV;
	if (!mainTarget.texture || !mainTarget.RTV || !depthResource)
		return;
	D3D11_TEXTURE2D_DESC mainDescription{};
	mainTarget.texture->GetDesc(&mainDescription);
	const float2 dynamicSize = Util::ConvertToDynamic({ static_cast<float>(mainDescription.Width), static_cast<float>(mainDescription.Height) });
	if (dynamicSize.x < 2.0f || dynamicSize.y < 1.0f)
		return;

	SetupResources();
	if (!perFrameCB || !dropBuffer || !dropLocalOffsetBuffer || !dropGroupOffsetBuffer ||
		!visibleDropIndexBuffer || !indirectDrawArgsBuffer || !EnsureShaders()) {
		renderPathReady = false;
		return;
	}
	renderPathReady = true;
	lastDrawFrame = frame;
	const bool drawDistantRain = settings.EnableDistantRain && settings.RainDebugMode == 0 &&
	                             EnsureDistantRainShaders();

	CS_GPU_PASS("RainRendering::AirborneRain");
	if (UsesWaterMaterial(settings)) {
		EnsureRainSampler();
		if (settings.EnableTexturedRain)
			EnsureRainTexture();
	}
	const bool hasSceneColor = mainTarget.SRV && UsesWaterMaterial(settings) && settings.EnableRainRefraction &&
	                           settings.RainSceneRefractionMix > 0.0f && settings.RainRefractionStrength > 0.0f &&
	                           EnsureSceneColorCopy(mainTarget.texture, mainTarget.RTV);

	const float farDistance = ClampFinite(settings.RainFarDistance, 1000.0f, 50000.0f, 12000.0f);
	const auto head = Util::GetAverageEyePosition();
	const auto lightColor = GetRainLightColor();

	PerFrame data{};
	data.HeadPositionAndTime = { head.x, head.y, head.z, globals::state->timer };
	data.VolumeSizeAndDensity = {
		farDistance * 2.0f,
		farDistance * 2.0f,
		farDistance * 2.0f,
		ClampFinite(settings.RainDensity, 0.0f, 2.0f, 0.72f)
	};
	data.WeatherFallDepth = {
		weather.intensity,
		ClampFinite(settings.RainFallSpeed, 100.0f, 10000.0f, 2600.0f) * weather.fallSpeedScale,
		0.0f,
		ClampFinite(settings.RainIntersectionFadeDistance, 1.0f, 1000.0f, 96.0f)
	};
	data.Streak = {
		ClampFinite(settings.RainStreakLength, 1.0f, 1000.0f, 80.0f),
		ClampFinite(settings.RainVelocityStretch, 0.0f, 1.0f, 0.075f),
		ClampFinite(settings.RainStreakWidth, 0.05f, 20.0f, 5.18f),
		ClampFinite(settings.RainOpacity, 0.0f, 1.0f, 0.48f)
	};
	data.Appearance = {
		ClampFinite(settings.RainBrightness, 0.0f, 8.0f, 1.15f),
		ClampFinite(settings.RainLightingResponse, 0.0f, 1.0f, 0.50f),
		ClampFinite(settings.RainMinimumVisibility, 0.0f, 1.0f, 0.01f),
		ClampFinite(settings.RainNearCutoffDistance, 0.0f, 64.0f, 4.0f)
	};
	data.DistanceNoise = {
		farDistance,
		ClampFinite(settings.RainDensityNoiseScale, 64.0f, 50000.0f, 3200.0f),
		ClampFinite(settings.RainDensityNoiseStrength, 0.0f, 1.0f, 0.65f),
		0.0f
	};
	data.Curtain = {
		ClampFinite(settings.RainCurtainScale, 64.0f, 80000.0f, 7500.0f),
		ClampFinite(settings.RainCurtainStrength, 0.0f, 1.0f, 0.80f),
		ClampFinite(settings.RainCurtainContrast, 0.1f, 8.0f, 1.75f),
		0.0f
	};
	data.CurtainDensity = {
		ClampFinite(settings.RainCurtainMinDensity, 0.0f, 4.0f, 0.28f),
		ClampFinite(settings.RainCurtainMaxDensity, 0.0f, 4.0f, 1.85f),
		0.0f,
		0.0f
	};
	data.LightColor = { lightColor.x, lightColor.y, lightColor.z, 0.0f };
	data.CameraData = Util::GetCameraData();
	data.GridAndDebug = {
		kGridWidth,
		kGridDepth,
		kGridHeight,
		std::min<uint>(settings.RainDebugMode, 8u)
	};
	data.LayerRadii = GetLayerRadii(farDistance);
	data.LayerCounts = GetLayerDropCounts();
	const auto& skylighting = globals::features::skylighting;
	const bool hasRoofOcclusion = settings.EnableRainRoofOcclusion && skylighting.loaded &&
	                              skylighting.texProbeArray && skylighting.texProbeArray->srv.get();
	const bool hasCanopyClassification = settings.EnableRainCanopyResponse && hasRoofOcclusion &&
	                                     skylighting.texOcclusion && skylighting.texOcclusion->srv.get() &&
	                                     skylighting.comparisonSampler && canopyOcclusionCS && solidCoverOcclusion &&
	                                     canopyClassification && canopyAccumulation;
	const float roofFadeStart = ClampFinite(settings.RainRoofOcclusionFadeStart, 0.0f, 0.99f, 0.20f);
	const float roofFadeEnd = std::max(
		ClampFinite(settings.RainRoofOcclusionFadeEnd, 0.0f, 1.0f, 0.75f), roofFadeStart + 0.01f);
	const uint32_t overheadDropCount = std::min(settings.RainOverheadDropCount, data.LayerCounts[0]);
	data.RoofOcclusion = { hasRoofOcclusion ? 1.0f : 0.0f, roofFadeStart, roofFadeEnd, static_cast<float>(overheadDropCount) };
	data.DistantRain = {
		ClampFinite(settings.RainDistantDensity, 0.0f, 2.0f, 1.0f),
		ClampFinite(settings.RainDistantOpacity, 0.0f, 1.0f, 0.55f),
		ClampFinite(settings.RainDistantStreakLength, 4.0f, 256.0f, 64.0f),
		ClampFinite(settings.RainDistantStreakWidth, 0.2f, 8.0f, 3.0f)
	};
	data.Canopy = {
		hasCanopyClassification ? 1.0f : 0.0f,
		ClampFinite(settings.RainCanopyDensityScale, 0.0f, 1.0f, 0.35f),
		ClampFinite(settings.RainCanopySpeedScale, 0.5f, 1.0f, 0.85f),
		0.0f
	};
	UpdateGlassyConstants(data, mainDescription, dynamicSize, hasSceneColor);
	perFrameCB->Update(data);

	const uint32_t dropCount = data.LayerCounts[3];
	const uint32_t groupCount = (dropCount + kRainComputeGroupSize - 1u) / kRainComputeGroupSize;
	ID3D11Buffer* rainBuffer = perFrameCB->CB();
	ID3D11Buffer* sharedBuffer = globals::state->sharedDataCB->CB();
	{
		CS_GPU_PASS("RainRendering::UpdateDrops");
		RainComputeState savedComputeState(context);
		if (hasCanopyClassification)
			UpdateCanopyOcclusion(context, sharedBuffer, frameBuffer);
		context->CSSetShader(rainUpdateCS.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &rainBuffer);
		context->CSSetConstantBuffers(5, 1, &sharedBuffer);
		ID3D11Buffer* featureBuffer = globals::state->featureDataCB->CB();
		context->CSSetConstantBuffers(6, 1, &featureBuffer);
		context->CSSetConstantBuffers(12, 1, &frameBuffer);
		std::array<ID3D11ShaderResourceView*, 4> computeResources{};
		if (data.LocalLighting.x > 0.0f) {
			const auto& lightLimitFix = globals::features::lightLimitFix;
			computeResources[0] = lightLimitFix.lights->srv.get();
			computeResources[1] = lightLimitFix.lightIndexList->srv.get();
			computeResources[2] = lightLimitFix.lightGrid->srv.get();
		}
		if (hasRoofOcclusion)
			computeResources[3] = skylighting.texProbeArray->srv.get();
		context->CSSetShaderResources(35, static_cast<UINT>(computeResources.size()), computeResources.data());
		ID3D11ShaderResourceView* canopyResources[] = {
			hasCanopyClassification ? canopyClassification->srv.get() : nullptr,
			hasCanopyClassification ? canopyAccumulation->srv.get() : nullptr
		};
		context->CSSetShaderResources(41, static_cast<UINT>(std::size(canopyResources)), canopyResources);

		std::array<ID3D11UnorderedAccessView*, 5> computeUAVs{};
		computeUAVs[0] = dropBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->Dispatch(groupCount, 1, 1);

		computeUAVs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		ID3D11ShaderResourceView* dropResource = dropBuffer->SRV();
		context->CSSetShaderResources(1, 1, &dropResource);
		computeUAVs[1] = dropLocalOffsetBuffer->UAV();
		computeUAVs[2] = dropGroupOffsetBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainCountCS.get(), nullptr, 0);
		context->Dispatch(groupCount, 1, 1);

		computeUAVs.fill(nullptr);
		computeUAVs[2] = dropGroupOffsetBuffer->UAV();
		computeUAVs[4] = indirectDrawArgsBuffer->uav.get();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainPrefixCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);

		computeUAVs.fill(nullptr);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		ID3D11ShaderResourceView* compactionResources[]{ dropLocalOffsetBuffer->SRV(), dropGroupOffsetBuffer->SRV() };
		context->CSSetShaderResources(39, static_cast<UINT>(std::size(compactionResources)), compactionResources);
		computeUAVs[3] = visibleDropIndexBuffer->UAV();
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(computeUAVs.size()), computeUAVs.data(), nullptr);
		context->CSSetShader(rainScatterCS.get(), nullptr, 0);
		context->Dispatch(groupCount, 1, 1);
	}

	RainPipelineState savedState(context);
	if (hasSceneColor)
		DownsampleSceneColor(mainTarget.SRV, depthResource, dynamicSize);
	ID3D11RenderTargetView* renderTarget = mainTarget.RTV;
	context->OMSetRenderTargets(1, &renderTarget, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, UINT_MAX);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());

	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, dynamicSize.x, dynamicSize.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(rainVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(rainPS.get(), nullptr, 0);

	context->VSSetConstantBuffers(0, 1, &rainBuffer);
	context->VSSetConstantBuffers(5, 1, &sharedBuffer);
	context->VSSetConstantBuffers(12, 1, &frameBuffer);
	ID3D11ShaderResourceView* dropSRV = dropBuffer->SRV();
	ID3D11ShaderResourceView* visibleIndexSRV = visibleDropIndexBuffer->SRV();
	context->VSSetShaderResources(1, 1, &dropSRV);
	context->VSSetShaderResources(39, 1, &visibleIndexSRV);
	context->PSSetConstantBuffers(0, 1, &rainBuffer);
	ID3D11Buffer* featureBuffer = globals::state->featureDataCB->CB();
	ID3D11Buffer* pixelSharedBuffers[]{ sharedBuffer, featureBuffer };
	context->PSSetConstantBuffers(5, 2, pixelSharedBuffers);
	context->PSSetShaderResources(0, 1, &depthResource);
	ID3D11ShaderResourceView* colorResource = hasSceneColor ? sceneColorSRV.get() : nullptr;
	context->PSSetShaderResources(2, 1, &colorResource);
	ID3D11ShaderResourceView* waterResources[]{ rainTextureSRV.get(), GetRainEnvironment() };
	context->PSSetShaderResources(3, 2, waterResources);
	ID3D11ShaderResourceView* refractionDepthResource = hasSceneColor ? sceneDepthSRV.get() : nullptr;
	context->PSSetShaderResources(5, 1, &refractionDepthResource);
	ID3D11SamplerState* sampler = refractionSampler.get();
	context->PSSetSamplers(0, 1, &sampler);

	context->DrawInstancedIndirect(indirectDrawArgsBuffer->resource.get(), 0);
	if (drawDistantRain)
		DrawDistantRain(context, settings.RainDistantDropCount, globals::game::isVR ? 2u : 1u);
}

void RainRendering::DrawDistantRain(ID3D11DeviceContext* a_context, uint32_t a_dropCount, uint32_t a_eyeCount)
{
	CS_GPU_PASS("RainRendering::DistantRain");
	a_context->VSSetShader(distantRainVS.get(), nullptr, 0);
	a_context->PSSetShader(distantRainPS.get(), nullptr, 0);
	a_context->DrawInstanced(6, a_dropCount * a_eyeCount, 0, 0);
}
