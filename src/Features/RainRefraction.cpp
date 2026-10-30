#include "RainRefraction.h"

#include <DirectXTex.h>

#include "I18n/I18n.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RainRefraction::Settings,
	Enable,
	MotionStretch,
	MotionTransparency)

namespace
{
	constexpr auto kDefaultRaindropsPath = L"Data\\Shaders\\RainRefraction\\Raindrops.png";
	constexpr uint32_t kRainRenderPass = 0x5C00006A;

	/** @brief Loads a raindrop normal map (RGB normal, A coverage) as a mipmapped BC7 texture named a_name. */
	RainRefraction::RaindropTexture LoadRaindropTexture(const std::filesystem::path& a_path, const char* a_name)
	{
		RainRefraction::RaindropTexture result;
		const auto fail = [&](std::string a_status) {
			logger::warn("[Rain Refraction] {}: {}", a_path.string(), a_status);
			result.status = std::move(a_status);
			return std::move(result);
		};

		// Most presets ship without enbraindrops.png, so a missing file is not worth a warning
		if (!std::filesystem::exists(a_path)) {
			logger::debug("[Rain Refraction] Raindrop texture not found: {}", a_path.string());
			result.status = std::format("Texture not found: {}", a_path.filename().string());
			return result;
		}

		DirectX::ScratchImage image;
		HRESULT hr = DirectX::LoadFromWICFile(a_path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, image);
		if (FAILED(hr))
			return fail(std::format("Failed to load texture (invalid image, HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		DirectX::ScratchImage rgbaImage;
		const auto* source = image.GetImage(0, 0, 0);
		if (source->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			hr = DirectX::Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, rgbaImage);
			if (FAILED(hr))
				return fail(std::format("Failed to load texture (invalid image, HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));
			source = rgbaImage.GetImage(0, 0, 0);
		}
		result.footprint = RainRefractionTexture::GetFootprint({ source->pixels, source->slicePitch }, source->width, source->height, source->rowPitch);

		DirectX::ScratchImage mipImage;
		hr = DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DirectX::TEX_FILTER_DEFAULT, 0, mipImage);
		if (FAILED(hr))
			return fail(std::format("Failed to generate mipmaps (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		DirectX::ScratchImage bc7Image;
		hr = DirectX::Compress(mipImage.GetImages(), mipImage.GetImageCount(), mipImage.GetMetadata(),
			DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_BC7_QUICK, 1.0f, bc7Image);
		if (FAILED(hr))
			return fail(std::format("Failed to compress texture (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));

		const auto& metadata = bc7Image.GetMetadata();
		winrt::com_ptr<ID3D11Texture2D> texture;
		hr = DirectX::CreateTexture(globals::d3d::device, bc7Image.GetImages(), bc7Image.GetImageCount(), metadata,
			reinterpret_cast<ID3D11Resource**>(texture.put()));
		if (FAILED(hr))
			return fail(std::format("Failed to create GPU texture (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));
		Util::SetResourceName(texture.get(), a_name);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{
			.Format = DXGI_FORMAT_BC7_UNORM,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = static_cast<UINT>(metadata.mipLevels) }
		};
		hr = globals::d3d::device->CreateShaderResourceView(texture.get(), &srvDesc, result.srv.put());
		if (FAILED(hr))
			return fail(std::format("Failed to create shader resource view (HRESULT 0x{:08X})", static_cast<uint32_t>(hr)));
		Util::SetResourceName(result.srv.get(), "%s SRV", a_name);

		logger::info("[Rain Refraction] Loaded raindrop texture: {} ({}x{}, BC7, {} mips)", a_path.string(), metadata.width, metadata.height, metadata.mipLevels);
		return result;
	}
}

#define I18N_KEY_PREFIX "feature.rain_refraction."

void RainRefraction::DrawSettings()
{
	if (IsManagedByENB()) {
		ImGui::TextColored(globals::menu->GetSettings().Theme.StatusPalette.Warning, "%s", T("common.settings_managed_by_enb", "Settings are currently managed by ENB."));
		return;
	}

	if (!defaultRaindrops.status.empty())
		Util::Text::WrappedWarning("%s", defaultRaindrops.status.c_str());

	ImGui::Checkbox(T(TKEY("enable"), "Enable"), &settings.Enable);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Renders rain as refractive drops. Requires Dynamic Cubemaps."));
	}
	ImGui::SliderFloat(T(TKEY("motion_stretch"), "Motion Stretch"), &settings.MotionStretch, kMinMotionStretch, kMaxMotionStretch, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("motion_stretch_tooltip"), "How far drops stretch along their fall velocity.\nLow values give round drops, 1 = vanilla streak length, higher values stretch further."));
	}
	ImGui::SliderFloat(T(TKEY("motion_transparency"), "Motion Transparency"), &settings.MotionTransparency, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("motion_transparency_tooltip"), "Fades the drops; higher values make rain more transparent."));
	}
}

#undef I18N_KEY_PREFIX

void RainRefraction::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MotionStretch = std::clamp(settings.MotionStretch, kMinMotionStretch, kMaxMotionStretch);
	settings.MotionTransparency = std::clamp(settings.MotionTransparency, 0.0f, 1.0f);
}

void RainRefraction::SaveSettings(json& o_json)
{
	o_json = settings;
}

void RainRefraction::RestoreDefaultSettings()
{
	settings = {};
}

void RainRefraction::SetupResources()
{
	defaultRaindrops = LoadRaindropTexture(kDefaultRaindropsPath, "RainRefraction::DefaultRaindrops");

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(globals::d3d::device->CreateBlendState(&blendDesc, alphaBlendState.put()))) {
		logger::error("[Rain Refraction] Failed to create alpha blend state, refractive rain disabled");
		return;
	}
	Util::SetResourceName(alphaBlendState.get(), "RainRefraction::AlphaBlendState");
}

void RainRefraction::LoadPresetRaindrops(const std::filesystem::path& a_presetPath)
{
	presetRaindrops = LoadRaindropTexture(a_presetPath / "enbraindrops.png", "RainRefraction::PresetRaindrops");
}

void RainRefraction::SetPresetResolver(PresetResolver a_resolver)
{
	presetResolver = std::move(a_resolver);
}

bool RainRefraction::IsManagedByENB() const
{
	return presetManaged;
}

ID3D11ShaderResourceView* RainRefraction::GetRaindropSRV() const
{
	if (IsManagedByENB() && presetRaindrops.srv)
		return presetRaindrops.srv.get();
	return defaultRaindrops.srv.get();
}

RainRefraction::PerFrame RainRefraction::GetCommonBufferData()
{
	if (!perFrameCacheChecker.IsNewFrame())
		return perFrameCache;

	PerFrame data{
		.Enable = settings.Enable,
		.MotionStretch = settings.MotionStretch,
		.MotionTransparency = settings.MotionTransparency
	};

	const auto preset = presetResolver ? presetResolver() : std::nullopt;
	presetManaged = preset.has_value();
	if (preset)
		data = *preset;
	data.Enable = data.Enable && GetRaindropSRV() && alphaBlendState;
	const auto& footprint = IsManagedByENB() && presetRaindrops.srv ? presetRaindrops.footprint : defaultRaindrops.footprint;
	data.TextureBounds = float4{ footprint.uvBounds[0], footprint.uvBounds[1], footprint.uvBounds[2], footprint.uvBounds[3] };
	data.TextureRadiusScale = float2{ footprint.radiusScale[0], footprint.radiusScale[1] };

	perFrameCache = data;
	return data;
}

bool RainRefraction::IsRefractiveRainPass()
{
	auto state = globals::state;
	// Runs per draw, so reuse the last feature buffer resolve instead of re-querying settings
	return state->currentPixelDescriptor == static_cast<uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::EnvCubeRain) &&
	       perFrameCache.Enable;
}

void RainRefraction::ModifyParticle()
{
	if (globals::state->currentPixelDescriptor != static_cast<uint32_t>(SIE::ShaderCache::ParticleShaderTechniques::EnvCubeRain))
		return;

	auto context = globals::d3d::context;
	ID3D11Buffer* cbs[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->VSSetConstantBuffers(5, 2, cbs);
	if (IsRefractiveRainPass()) {
		ID3D11ShaderResourceView* srv = GetRaindropSRV();
		context->PSSetShaderResources(80, 1, &srv);
		CaptureSceneColor();
	}
	BindRenderSpace(Util::ConvertToDynamic(globals::state->screenSize), { 1, 1 }, false);
}

bool RainRefraction::CaptureSceneColor()
{
	if (sceneFrame == globals::state->frameCount)
		return true;
	if (sceneCopyFailed || drawDataFailed)
		return false;
	auto* source = Util::AsReal(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN].texture);
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	source->GetDesc(&desc);
	if (desc.SampleDesc.Count != 1)
		return false;
	try {
		if (!sceneColor || sceneColor->desc.Width != desc.Width || sceneColor->desc.Height != desc.Height || sceneColor->desc.Format != desc.Format) {
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			auto next = std::make_unique<Texture2D>(desc, "RainRefraction::SceneColor");
			next->CreateSRV({ .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MipLevels = 1 } });
			sceneColor = std::move(next);
		}
		CS_GPU_PASS("RainRefraction::CaptureScene");
		auto* context = globals::d3d::context;
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(81, 1, &nullSRV);
		const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
		const D3D11_BOX region{ 0, 0, 0, std::min(desc.Width, static_cast<UINT>(std::round(renderSize.x))), std::min(desc.Height, static_cast<UINT>(std::round(renderSize.y))), 1 };
		context->CopySubresourceRegion(sceneColor->resource.get(), 0, 0, 0, 0, source, 0, &region);
		sceneFrame = globals::state->frameCount;
		return true;
	} catch (const std::exception& error) {
		logger::error("[Rain Refraction] Scene refraction unavailable: {}", error.what());
		sceneCopyFailed = true;
		return false;
	}
}

bool RainRefraction::BindRenderSpace(float2 a_rasterSize, float2 a_sourceScale, bool a_testDepth)
{
	auto* context = globals::d3d::context;
	ID3D11Buffer* cb = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	if (!drawDataFailed) {
		try {
			const DrawData next{ a_sourceScale, a_rasterSize, a_testDepth, sceneFrame == globals::state->frameCount };
			const auto index = a_testDepth ? 1u : 0u;
			auto& buffer = drawCB[index];
			if (!buffer) {
				buffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DrawData>(), a_testDepth ? "RainRefraction::DisplayDrawData" : "RainRefraction::DrawData");
				buffer->Update(next);
			} else if (std::memcmp(&next, &drawData[index], sizeof(next)) != 0) {
				buffer->Update(next);
			}
			drawData[index] = next;
			cb = buffer->CB();
			srv = next.sceneAvailable ? sceneColor->srv.get() : nullptr;
		} catch (const std::exception& error) {
			logger::error("[Rain Refraction] Rain draw data unavailable: {}", error.what());
			drawDataFailed = true;
		}
	}
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);
	context->PSSetShaderResources(81, 1, &srv);
	if (!cb)
		context->PSSetShaderResources(82, 1, &srv);
	return cb != nullptr;
}

void RainRefraction::ClearShaderCache()
{
	sceneColor.reset();
	drawCB = {};
	drawData = {};
	sceneFrame = UINT32_MAX;
	sceneCopyFailed = false;
	drawDataFailed = false;
}

std::function<void()> RainRefraction::OnRenderPassBegin(const RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Particle ||
		a_pass->passEnum != kRainRenderPass || particlePass)
		return nullptr;
	[[maybe_unused]] static const bool registered = GpuPassCapabilities::Register("RainRefraction::Particles");
	particlePass.emplace("RainRefraction::Particles", GpuPassSpan::Spanning);
	return [this]() { particlePass.reset(); };
}

void RainRefraction::ParticleShaderHacks()
{
	auto state = globals::state;
	if (!state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Particle)
		return;
	if (!IsRefractiveRainPass())
		return;

	float blendFactor[4] = { 0, 0, 0, 0 };
	globals::d3d::context->OMSetBlendState(alphaBlendState.get(), blendFactor, 0xFFFFFFFF);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}
