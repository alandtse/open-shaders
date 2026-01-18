#include "TerrainBlending.h"

#include "Deferred.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainBlending::Settings,
	Enable,
	LockSignatureAfterFrames,
	SignatureLockFrames,
	TerrainCullDistance)

namespace
{
	struct PrepassSignature
	{
		uint32_t technique = 0;
		uint32_t renderFlags = 0;
		RE::BSShader::Type shaderType = RE::BSShader::Type::None;
		bool valid = false;
	};

	struct SignatureCount
	{
		uint64_t key = 0;
		RE::BSShader::Type shaderType = RE::BSShader::Type::None;
		uint32_t count = 0;
	};

	struct LoggedSignature
	{
		uint64_t key = 0;
		RE::BSShader::Type shaderType = RE::BSShader::Type::None;
	};

	PrepassSignature g_mainPrepassSignature{};
	std::vector<SignatureCount> g_depthOnlySignatures;
	std::vector<LoggedSignature> g_loggedDepthOnlySignatures;
	bool g_signaturePrepassActive = false;
	bool g_hasFrame = false;
	uint32_t g_lastFrame = 0;
	bool g_inMainDepthGroup = false;
	bool g_signatureMatchedInGroup = false;
	uint32_t g_signatureFrameCount = 0;
	bool g_signatureLocked = false;

	constexpr uint32_t kRenderFlagsSignatureMask = ~0x00000010u;
	constexpr uint32_t kUtilityRenderDepthFlag = static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderDepth);
	constexpr uint32_t kUtilityShadowFlags =
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmap) |
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmapClamped) |
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmapPb) |
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmask) |
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmaskSpot) |
		static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmaskPb);

	uint32_t NormalizeRenderFlags(uint32_t renderFlags)
	{
		return renderFlags & kRenderFlagsSignatureMask;
	}

	uint64_t MakeSignatureKey(uint32_t technique, uint32_t renderFlags)
	{
		return (static_cast<uint64_t>(technique) << 32) | renderFlags;
	}

	uint32_t NormalizeLightingTechnique(uint32_t technique)
	{
		const uint32_t techniqueType = (technique >> 24) & 0x3F;
		return techniqueType << 24;
	}

	bool GetNormalizedSignature(const RE::BSRenderPass* pass, uint32_t technique, uint32_t renderFlags, uint32_t& outTechnique, uint32_t& outFlags, RE::BSShader::Type& outShaderType)
	{
		if (!pass || !pass->shader) {
			return false;
		}

		const auto shaderType = pass->shader->shaderType.get();
		if (shaderType == RE::BSShader::Type::Utility) {
			if ((technique & kUtilityRenderDepthFlag) == 0 || (technique & kUtilityShadowFlags) != 0) {
				return false;
			}
			outTechnique = kUtilityRenderDepthFlag;
			outFlags = NormalizeRenderFlags(renderFlags);
			outShaderType = RE::BSShader::Type::Utility;
			return true;
		}

		if (shaderType == RE::BSShader::Type::Lighting) {
			outTechnique = NormalizeLightingTechnique(technique);
			outFlags = NormalizeRenderFlags(renderFlags);
			outShaderType = RE::BSShader::Type::Lighting;
			return true;
		}

		return false;
	}

	void TrackDepthOnlySignature(const RE::BSRenderPass* pass, uint32_t technique, uint32_t renderFlags)
	{
		uint32_t normalizedTechnique = 0;
		uint32_t normalizedFlags = 0;
		RE::BSShader::Type normalizedShaderType = RE::BSShader::Type::None;
		if (!GetNormalizedSignature(pass, technique, renderFlags, normalizedTechnique, normalizedFlags, normalizedShaderType)) {
			return;
		}

		const uint64_t key = MakeSignatureKey(normalizedTechnique, normalizedFlags);
		for (auto& entry : g_depthOnlySignatures) {
			if (entry.key == key && entry.shaderType == normalizedShaderType) {
				entry.count++;
				return;
			}
		}
		g_depthOnlySignatures.push_back({ key, normalizedShaderType, 1 });
	}

	void LogDepthOnlySignatureOnce(const RE::BSRenderPass* pass, uint32_t technique, uint32_t renderFlags)
	{
		uint32_t normalizedTechnique = 0;
		uint32_t normalizedFlags = 0;
		RE::BSShader::Type normalizedShaderType = RE::BSShader::Type::None;
		if (!GetNormalizedSignature(pass, technique, renderFlags, normalizedTechnique, normalizedFlags, normalizedShaderType)) {
			return;
		}

		const uint64_t key = MakeSignatureKey(normalizedTechnique, normalizedFlags);
		for (const auto& logged : g_loggedDepthOnlySignatures) {
			if (logged.key == key && logged.shaderType == normalizedShaderType) {
				return;
			}
		}
		g_loggedDepthOnlySignatures.push_back({ key, normalizedShaderType });

		const auto shaderType = pass && pass->shader ? pass->shader->shaderType.get() : RE::BSShader::Type::Total;
		const uint32_t shaderTypeValue = static_cast<uint32_t>(shaderType);
		const uint32_t passEnum = pass ? pass->passEnum : 0;
		const uint32_t hint = pass ? pass->accumulationHint : 0;
		const uint32_t lights = pass ? pass->numLights : 0;
		const uint32_t shadowLights = pass ? pass->numShadowLights : 0;
		logger::debug("[TB][SIG] depth-only signature tech=0x{:X} flags=0x{:X} (norm tech=0x{:X} flags=0x{:X}) shader={} pass={} hint={} lights={} shadowLights={}",
			technique, renderFlags, normalizedTechnique, normalizedFlags, shaderTypeValue, passEnum, hint, lights, shadowLights);
	}

	void UpdateMainPrepassSignature(uint32_t frame, bool logUpdates)
	{
		auto& settings = globals::features::terrainBlending.settings;
		const bool lockEnabled = settings.LockSignatureAfterFrames && settings.SignatureLockFrames > 0;
		if (!lockEnabled) {
			g_signatureLocked = false;
			g_signatureFrameCount = 0;
		}

		if (!g_hasFrame) {
			g_lastFrame = frame;
			g_hasFrame = true;
			return;
		}

		if (frame == g_lastFrame) {
			return;
		}

		g_lastFrame = frame;

		if (g_signatureLocked) {
			return;
		}

		++g_signatureFrameCount;

		const SignatureCount* best = nullptr;
		for (const auto& entry : g_depthOnlySignatures) {
			if (!best || entry.count > best->count) {
				best = &entry;
			}
		}

		if (best) {
			const uint32_t technique = static_cast<uint32_t>(best->key >> 32);
			const uint32_t renderFlags = static_cast<uint32_t>(best->key);
			const bool changed = !g_mainPrepassSignature.valid ||
			                     g_mainPrepassSignature.technique != technique ||
			                     g_mainPrepassSignature.renderFlags != renderFlags ||
			                     g_mainPrepassSignature.shaderType != best->shaderType;
			g_mainPrepassSignature = { technique, renderFlags, best->shaderType, true };
			if (changed && logUpdates) {
				logger::info("[TB] main prepass signature tech=0x{:X} flags=0x{:X} shader={} (depth-only count={})",
					technique, renderFlags, static_cast<uint32_t>(g_mainPrepassSignature.shaderType), best->count);
			}
		}

		if (lockEnabled && g_mainPrepassSignature.valid &&
			g_signatureFrameCount >= static_cast<uint32_t>(settings.SignatureLockFrames)) {
			g_signatureLocked = true;
		}

		g_depthOnlySignatures.clear();
	}

	bool IsDepthOnlyPass(ID3D11DeviceContext* context)
	{
		if (!context) {
			return false;
		}

		ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);

		bool hasRTV = false;
		for (auto& rtv : rtvs) {
			if (rtv) {
				hasRTV = true;
				rtv->Release();
			}
		}
		if (dsv) {
			dsv->Release();
		}

		return !hasRTV;
	}
}

ID3D11VertexShader* TerrainBlending::GetTerrainVertexShader()
{
	if (!terrainVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" } }, "vs_5_0");
	}
	return terrainVertexShader;
}

ID3D11VertexShader* TerrainBlending::GetTerrainOffsetVertexShader()
{
	if (!terrainOffsetVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainOffsetVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "OFFSET_DEPTH", "" } }, "vs_5_0");
	}
	return terrainOffsetVertexShader;
}

ID3D11ComputeShader* TerrainBlending::GetDepthBlendShader()
{
	if (!depthBlendShader) {
		logger::debug("Compiling DepthBlend.hlsl");
		depthBlendShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\DepthBlend.hlsl", {}, "cs_5_0");
	}
	return depthBlendShader;
}

void TerrainBlending::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &terrainDepth.texture));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(terrainDepth.texture, &srvDesc, &terrainDepth.depthSRV));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(terrainDepth.texture, &dsvDesc, &terrainDepth.views[0]));
	}

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		blendedDepthTexture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		uavDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		blendedDepthTexture16 = new Texture2D(texDesc);
		blendedDepthTexture16->CreateSRV(srvDesc);
		blendedDepthTexture16->CreateUAV(uavDesc);

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		depthSRVBackup = mainDepth.depthSRV;

		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		prepassSRVBackup = zPrepassCopy.depthSRV;
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, &terrainDepthStencilState));
	}
}

void TerrainBlending::PostPostLoad()
{
	Hooks::Install();
}

void TerrainBlending::DataLoaded()
{
	auto bEnableLandFade = RE::GetINISetting("bEnableLandFade:Display");
	bEnableLandFade->data.b = false;
}

void TerrainBlending::DrawSettings()
{
	ImGui::Checkbox("Enable Terrain Blending", &settings.Enable);
	ImGui::Spacing();

	if (ImGui::TreeNodeEx("Performance Options", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Lock Prepass Signature", &settings.LockSignatureAfterFrames);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Stops per-pass signature tracking after the signature stabilizes.");
		}

		ImGui::BeginDisabled(!settings.LockSignatureAfterFrames);
		ImGui::SliderInt("Signature Lock Frames", &settings.SignatureLockFrames, 1, 240, "%d");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Number of frames to observe before locking the prepass signature.");
		}
		ImGui::EndDisabled();

		ImGui::SliderFloat("Terrain Depth Culling Distance", &settings.TerrainCullDistance, 0.0f, 8192.0f, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Terrain farther than this distance skips TB depth rendering. Set to 0 to disable culling.");
		}
		ImGui::TreePop();
	}
}

void TerrainBlending::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainBlending::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainBlending::RestoreDefaultSettings()
{
	settings = {};
}

void TerrainBlending::TerrainShaderHacks()
{
	if (renderTerrainDepth) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;
		if (renderAltTerrain) {
			auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			context->VSSetShader(GetTerrainOffsetVertexShader(), NULL, NULL);
		} else {
			auto dsv = terrainDepth.views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			auto shadowState = globals::game::shadowState;
			GET_INSTANCE_MEMBER(currentVertexShader, shadowState)
			context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);
		}
		renderAltTerrain = !renderAltTerrain;
	}
}

void TerrainBlending::ResetDepth()
{
	auto context = globals::d3d::context;

	auto dsv = terrainDepth.views[0];
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0u);
}

void TerrainBlending::ResetTerrainDepth()
{
	auto context = globals::d3d::context;

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto currentVertexShader = *globals::game::currentVertexShader;
	context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);
}

void TerrainBlending::BlendPrepassDepths()
{
	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		ID3D11ShaderResourceView* views[2] = { depthSRVBackup, terrainDepth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[2] = { blendedDepthTexture->uav.get(), blendedDepthTexture16->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDepthBlendShader(), nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto renderer = globals::game::renderer;
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	context->CopyResource(terrainDepth.texture, mainDepth.texture);
}

void TerrainBlending::ClearShaderCache()
{
	if (terrainVertexShader) {
		terrainVertexShader->Release();
		terrainVertexShader = nullptr;
	}
	if (terrainOffsetVertexShader) {
		terrainOffsetVertexShader->Release();
		terrainOffsetVertexShader = nullptr;
	}
	if (depthBlendShader) {
		depthBlendShader->Release();
		depthBlendShader = nullptr;
	}
}

void TerrainBlending::Hooks::Main_RenderDepth::thunk(bool a1, bool a2)
{
	auto& singleton = globals::features::terrainBlending;
	auto shaderCache = globals::shaderCache;
	auto renderer = globals::game::renderer;

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	if (!singleton.settings.Enable) {
		singleton.renderDepth = false;
		singleton.renderTerrainDepth = false;
		singleton.renderAltTerrain = false;
		mainDepth.depthSRV = singleton.depthSRVBackup;
		zPrepassCopy.depthSRV = singleton.prepassSRVBackup;
		func(a1, a2);
		return;
	}

	singleton.averageEyePosition = Util::GetAverageEyePosition();

	if (globals::game::isVR && shaderCache->IsEnabled()) {
		g_inMainDepthGroup = true;
		g_signatureMatchedInGroup = false;
		func(a1, a2);
		g_inMainDepthGroup = false;

		if (g_signatureMatchedInGroup) {
			singleton.renderDepth = false;

			if (singleton.renderTerrainDepth) {
				singleton.renderTerrainDepth = false;
				singleton.ResetTerrainDepth();
			}

			singleton.BlendPrepassDepths();
		}
		g_signaturePrepassActive = false;
		return;
	}

	if (shaderCache->IsEnabled()) {
		mainDepth.depthSRV = singleton.blendedDepthTexture->srv.get();
		zPrepassCopy.depthSRV = singleton.blendedDepthTexture->srv.get();

		singleton.renderDepth = true;
		singleton.ResetDepth();

		func(a1, a2);

		singleton.renderDepth = false;

		if (singleton.renderTerrainDepth) {
			singleton.renderTerrainDepth = false;
			singleton.ResetTerrainDepth();
		}

		singleton.BlendPrepassDepths();
	} else {
		mainDepth.depthSRV = singleton.depthSRVBackup;
		zPrepassCopy.depthSRV = singleton.prepassSRVBackup;

		func(a1, a2);
	}
}

void TerrainBlending::Hooks::BSBatchRenderer__RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto& singleton = globals::features::terrainBlending;
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;

	if (!singleton.settings.Enable) {
		func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	if (shaderCache->IsEnabled()) {
		if (g_inMainDepthGroup) {
			UpdateMainPrepassSignature(state->frameCount, state->IsDeveloperMode());

			const bool depthOnly = IsDepthOnlyPass(globals::d3d::context);
			if (depthOnly && !g_signatureLocked) {
				TrackDepthOnlySignature(a_pass, a_technique, a_renderFlags);
				if (state->IsDeveloperMode()) {
					LogDepthOnlySignatureOnce(a_pass, a_technique, a_renderFlags);
				}
			}

			bool signatureMatch = false;
			if (g_mainPrepassSignature.valid) {
				uint32_t normalizedTechnique = 0;
				uint32_t normalizedFlags = 0;
				RE::BSShader::Type normalizedShaderType = RE::BSShader::Type::None;
				if (GetNormalizedSignature(a_pass, a_technique, a_renderFlags, normalizedTechnique, normalizedFlags, normalizedShaderType)) {
					signatureMatch =
						normalizedShaderType == g_mainPrepassSignature.shaderType &&
						normalizedTechnique == g_mainPrepassSignature.technique &&
						normalizedFlags == g_mainPrepassSignature.renderFlags;
				}
			}

			if (depthOnly && signatureMatch) {
				g_signatureMatchedInGroup = true;
			}

			if (!singleton.renderDepth && depthOnly && signatureMatch) {
				auto renderer = globals::game::renderer;
				auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
				auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

				singleton.averageEyePosition = Util::GetAverageEyePosition();

				mainDepth.depthSRV = singleton.blendedDepthTexture->srv.get();
				zPrepassCopy.depthSRV = singleton.blendedDepthTexture->srv.get();

				singleton.renderDepth = true;
				singleton.ResetDepth();
				g_signaturePrepassActive = true;
			}
		}

		if (singleton.renderDepth) {
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);

			if (inTerrain) {
				const float cullDistance = singleton.settings.TerrainCullDistance;
				if (cullDistance > 0.0f) {
					if ((a_pass->geometry->worldBound.center.GetDistance(singleton.averageEyePosition) - a_pass->geometry->worldBound.radius) > cullDistance) {
						inTerrain = false;
					}
				}
			}

			if (singleton.renderTerrainDepth != inTerrain) {
				if (!inTerrain) {
					singleton.ResetTerrainDepth();
				}
				singleton.renderTerrainDepth = inTerrain;
			}

			if (inTerrain) {
				// Render terrain depth now; normal pass still runs after this hook.
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
			}
		} else if (globals::state->inWorld) {
			if (auto shaderProperty = a_pass->shaderProperty) {
				if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.terrainRenderPasses.push_back(call);
						return;
					}

					// Use kNoTransparencyMultiSample as a TB opt-out flag.
					if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kNoTransparencyMultiSample)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.renderPasses.push_back(call);
						return;
					}
				}
			}
		}
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void TerrainBlending::RenderTerrainBlendingPasses()
{
	if (!settings.Enable) {
		renderDepth = false;
		renderTerrainDepth = false;
		renderAltTerrain = false;
		terrainRenderPasses.clear();
		renderPasses.clear();
		auto renderer = globals::game::renderer;
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		mainDepth.depthSRV = depthSRVBackup;
		zPrepassCopy.depthSRV = prepassSRVBackup;
		return;
	}

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto shadowState = globals::game::shadowState;
	auto stateUpdateFlags = globals::game::stateUpdateFlags;

	// Bind terrain depth mask for blending.
	auto view = terrainDepth.depthSRV;
	context->PSSetShaderResources(55, 1, &view);

	if (!terrainRenderPasses.empty() || !renderPasses.empty()) {
		GET_INSTANCE_MEMBER(alphaBlendMode, shadowState)
		GET_INSTANCE_MEMBER(alphaBlendWriteMode, shadowState)
		GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState)

		// TB pass: enable alpha blending and depth override, then restore for queued passes.
		alphaBlendWriteMode = 1;
		alphaBlendMode = 1;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		context->OMSetDepthStencilState(terrainDepthStencilState, 0xFF);

		for (auto& renderPass : terrainRenderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

		alphaBlendMode = 0;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestEqual;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

		for (auto& renderPass : renderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

		terrainRenderPasses.clear();
		renderPasses.clear();
	}

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	mainDepth.depthSRV = depthSRVBackup;
}
