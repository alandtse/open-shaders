#include "TerrainBlending.h"

#include "Deferred.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "VR.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainBlending::Settings,
	Enabled)

namespace
{
	bool ShouldUseBlendedDepthSRV()
	{
		auto& vr = globals::features::vr;
		return !globals::game::isVR || !vr.gDepthBufferCulling || !*vr.gDepthBufferCulling;
	}

	bool IsShadowmaskDepthDescriptorWhitelisted(const uint32_t a_descriptor)
	{
		return a_descriptor == kShadowmaskDepthDescriptor0 || a_descriptor == kShadowmaskDepthDescriptor1;
	}

	bool IsTbVrDepthCullingActive(const TerrainBlending& a_tb)
	{
		return globals::game::isVR && a_tb.loaded && a_tb.settings.Enabled && !ShouldUseBlendedDepthSRV();
	}

	bool IsSlot2CallerAllowlisted(const uint32_t a_callerRva)
	{
		for (const auto rva : kSlot2CallerAllowlistRvas) {
			if (rva == a_callerRva) {
				return true;
			}
		}
		return false;
	}

	bool ShouldApplySlot2Rewrite(const uint32_t a_callerRva)
	{
		if (slot2BroadFallbackActive) {
			engineHookDiagnostics.slot2FallbackApplied++;
			return true;
		}

		if (IsSlot2CallerAllowlisted(a_callerRva)) {
			return true;
		}

		engineHookDiagnostics.slot2CallerRejected++;
		slot2RejectTotal++;
		slot2BlockedCallerRvas.insert(a_callerRva);

		if (kEnableAutoBroadSlot2Fallback && slot2RejectTotal >= kSlot2AutoFallbackRejectThreshold) {
			slot2BroadFallbackActive = true;
			engineHookDiagnostics.slot2FallbackApplied++;
			fallbackTriggerRva = a_callerRva;
			if (!fallbackActivatedLogged && IsDiagnosticSlot2GuardMode(globals::features::terrainBlending)) {
				logger::debug(
					"[TB Override] slot2 fallback activated triggerRva=0x{:X} blockedEvents={} blockedUniqueRvas={}",
					fallbackTriggerRva,
					slot2RejectTotal,
					slot2BlockedCallerRvas.size());
				fallbackActivatedLogged = true;
			}
			return true;
		}

		return false;
	}

	bool IsEngineHookFeatureGateSatisfied(const TerrainBlending& a_singleton)
	{
		if (!globals::game::isVR || !a_singleton.loaded || !a_singleton.settings.Enabled) {
			return false;
		}

		return !ShouldUseBlendedDepthSRV();
	}

	struct SlotOverrideResult
	{
		bool hasSrv = false;
		bool applied = false;
		bool alreadyBound = false;
	};

	using SlotRewriteGate = bool (*)(uint32_t);

	SlotOverrideResult ApplyPixelShaderSlotOverride(
		ID3D11DeviceContext* a_context,
		const uint32_t a_slot,
		ID3D11ShaderResourceView* a_overrideSrv,
		uint64_t& a_appliedCounter,
		uint64_t& a_alreadyBoundCounter,
		uint64_t& a_missingCounter,
		SlotRewriteGate a_rewriteGate,
		const uint32_t a_callerRva)
	{
		SlotOverrideResult result{};
		result.hasSrv = a_overrideSrv != nullptr;
		if (!result.hasSrv) {
			a_missingCounter++;
			return result;
		}

		ID3D11ShaderResourceView* currentSrv = nullptr;
		a_context->PSGetShaderResources(a_slot, 1, &currentSrv);
		result.alreadyBound = currentSrv == a_overrideSrv;
		if (result.alreadyBound) {
			a_alreadyBoundCounter++;
		} else {
			const bool canRewrite = a_rewriteGate ? a_rewriteGate(a_callerRva) : true;
			if (canRewrite) {
				a_context->PSSetShaderResources(a_slot, 1, &a_overrideSrv);
				result.applied = true;
				a_appliedCounter++;
			}
		}

		if (currentSrv) {
			currentSrv->Release();
		}

		return result;
	}

	void LogEngineHookOverrideState(
		EngineHookOverrideLogState& a_state,
		const char* a_label,
		RE::BSShader* a_shader,
		const uint32_t a_descriptor,
		const bool a_shouldApply,
		const bool a_hasSrv,
		const bool a_active)
	{
		(void)a_state;
		(void)a_label;
		(void)a_shader;
		(void)a_descriptor;
		(void)a_shouldApply;
		(void)a_hasSrv;
		(void)a_active;
	}

	void MaybeLogEngineHookSummary()
	{
		// Intentionally quiet: no periodic summary logs in normal/debug runs.
	}

	void MaybeLogHookActiveOnce()
	{
		if (!hookActiveLogged && IsDiagnosticSlot2GuardMode(globals::features::terrainBlending)) {
			std::ostringstream allowlist;
			for (size_t i = 0; i < kSlot2CallerAllowlistRvas.size(); i++) {
				if (i != 0) {
					allowlist << ", ";
				}
				allowlist << "0x" << std::uppercase << std::hex << kSlot2CallerAllowlistRvas[i];
			}
			logger::debug(
				"[TB Override] pass-specific hook active slot2AllowlistCount={} slot2AllowlistRvas=[{}] fallbackThreshold={}",
				kSlot2CallerAllowlistRvas.size(),
				allowlist.str(),
				kSlot2AutoFallbackRejectThreshold);
			hookActiveLogged = true;
		}
	}

	ID3D11ShaderResourceView* ResolveEngineOverrideSrv(const bool a_prefer16Bit)
	{
		auto& terrainBlending = globals::features::terrainBlending;
		if (a_prefer16Bit) {
			if (terrainBlending.blendedDepthTexture16 && terrainBlending.blendedDepthTexture16->srv) {
				return terrainBlending.blendedDepthTexture16->srv.get();
			}
			if (terrainBlending.blendedDepthTexture && terrainBlending.blendedDepthTexture->srv) {
				return terrainBlending.blendedDepthTexture->srv.get();
			}
		} else {
			if (terrainBlending.blendedDepthTexture && terrainBlending.blendedDepthTexture->srv) {
				return terrainBlending.blendedDepthTexture->srv.get();
			}
			if (terrainBlending.blendedDepthTexture16 && terrainBlending.blendedDepthTexture16->srv) {
				return terrainBlending.blendedDepthTexture16->srv.get();
			}
		}

		auto* renderer = globals::game::renderer;
		if (!renderer) {
			return nullptr;
		}

		auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
		return depthCopy.depthSRV;
	}

	void ReleaseEngineHookTechniqueOverride()
	{
		if (!engineHookTechniqueState.active) {
			return;
		}

		auto* context = globals::d3d::context;
		if (context) {
			context->PSSetShaderResources(17, 1, &engineHookTechniqueState.previousObbSrv);
			context->PSSetShaderResources(2, 1, &engineHookTechniqueState.previousShadowmaskSrv);
		}

		if (engineHookTechniqueState.previousObbSrv) {
			engineHookTechniqueState.previousObbSrv->Release();
			engineHookTechniqueState.previousObbSrv = nullptr;
		}
		if (engineHookTechniqueState.previousShadowmaskSrv) {
			engineHookTechniqueState.previousShadowmaskSrv->Release();
			engineHookTechniqueState.previousShadowmaskSrv = nullptr;
		}

		engineHookTechniqueState.active = false;
	}
}

std::vector<FeatureConstraints::Constraint> TerrainBlending::GetActiveConstraints() const
{
	std::vector<FeatureConstraints::Constraint> constraints;

	// Only constrain when all requested conditions are active:
	// VR + Terrain Blending enabled + runtime depth buffer culling enabled.
	if (!IsTbVrDepthCullingActive(*this)) {
		return constraints;
	}

	constraints.push_back({ { "ScreenSpaceShadows", "Enable" },
		false,
		"Screen Space Shadows is disabled in VR when Terrain Blending and Depth Buffer Culling are both active.",
		false });

	return constraints;
}

void TerrainBlending::DrawSettings()
{
	bool enabled = settings.Enabled != 0;
	if (ImGui::Checkbox("Enable Terrain Blending", &enabled)) {
		settings.Enabled = enabled ? 1u : 0u;
	}

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enable seamless blending between terrain and objects.");
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

	globals::game::graphicsState->SetCameraData(RE::Main::WorldRootCamera(), 1);

	singleton.averageEyePosition = Util::GetAverageEyePosition();

	if (shaderCache->IsEnabled() && singleton.settings.Enabled) {
		if (ShouldUseBlendedDepthSRV()) {
			mainDepth.depthSRV = singleton.blendedDepthTexture->srv.get();
			zPrepassCopy.depthSRV = singleton.blendedDepthTexture->srv.get();
		} else {
			mainDepth.depthSRV = singleton.depthSRVBackup;
			zPrepassCopy.depthSRV = singleton.prepassSRVBackup;
		}

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

	if (shaderCache->IsEnabled() && singleton.settings.Enabled) {
		if (singleton.renderDepth) {
			// Entering or exiting terrain depth section
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);

			if (inTerrain && a_pass->geometry) {
				if ((a_pass->geometry->worldBound.center.GetDistance(singleton.averageEyePosition) - a_pass->geometry->worldBound.radius) > 1024.0f) {
					inTerrain = false;
				}
			}

			if (singleton.renderTerrainDepth != inTerrain) {
				if (!inTerrain)
					singleton.ResetTerrainDepth();
				singleton.renderTerrainDepth = inTerrain;
			}

			if (inTerrain)
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);  // Run terrain twice
		} else if (globals::state->inWorld) {
			if (auto shaderProperty = a_pass->shaderProperty) {
				if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.terrainRenderPasses.push_back(call);
						return;
					}

					// Detect meshes which should not get terrain blending using an unused flag (kNoTransparencyMultiSample)
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
	if (!settings.Enabled) {
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

	// Used to get the distance of the surface to the lowest depth
	context->PSSetShaderResources(55, 1, &terrainDepth.depthSRV);

	if (!terrainRenderPasses.empty() || !renderPasses.empty()) {
		GET_INSTANCE_MEMBER(alphaBlendMode, shadowState)
		GET_INSTANCE_MEMBER(alphaBlendWriteMode, shadowState)
		GET_INSTANCE_MEMBER(depthStencilDepthMode, shadowState)

		// Reset alpha write and enable alpha blending
		alphaBlendWriteMode = 1;
		alphaBlendMode = 1;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Enable rendering for depth below the surface
		context->OMSetDepthStencilState(terrainDepthStencilState, 0xFF);

		for (auto& renderPass : terrainRenderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

		// Reset alpha blending
		alphaBlendMode = 0;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Reset depth testing
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
