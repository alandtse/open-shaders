#include "ENBDepthOfField.h"

#include "../EffectManager.h"
#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <array>
#include <ranges>

static constexpr std::array<std::string_view, 3> FocusTechniques = { "Aperture", "ReadFocus", "Focus" };

bool ENBDepthOfField::Apply()
{
	historyValid.fill(false);
	historyIndex.fill(0);
	apertureSRV.fill(nullptr);
	apertureFrame.fill(UINT32_MAX);
	const bool applied = EffectBase::Apply();

	fallbackTechnique.clear();
	for (const auto& name : techniques | std::views::keys)
		if (std::ranges::find(FocusTechniques, name) == FocusTechniques.end() && (fallbackTechnique.empty() || name < fallbackTechnique))
			fallbackTechnique = name;

	return applied;
}

void ENBDepthOfField::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto& manager = EffectManager::GetSingleton();
	const auto eye = std::max(0, manager.currentEyeIndex);
	const std::string suffix = eye == 1 ? "Right" : "";
	auto& textureMain = manager.GetTextureOriginal();
	if (!textureMain.texture || !textureMain.SRV)
		return;

	auto* textureHDRTemp = textureManager.GetCommonTexture("TextureHDRTemp");
	auto* textureHDRTemp2 = textureManager.GetCommonTexture("TextureHDRTemp2");
	if (!textureHDRTemp || !textureHDRTemp2)
		return;

	const bool swap = historyIndex[eye] != 0;

	auto& textureApertureRead = effectTextureCache[std::string(swap ? "TextureApertureSwap" : "TextureAperture") + suffix];
	auto& textureApertureWrite = effectTextureCache[std::string(swap ? "TextureAperture" : "TextureApertureSwap") + suffix];
	auto& textureReadFocus = effectTextureCache[std::string("TextureReadFocus") + suffix];
	auto& textureFocusRead = effectTextureCache[std::string(swap ? "TextureFocusSwap" : "TextureFocus") + suffix];
	auto& textureFocusWrite = effectTextureCache[std::string(swap ? "TextureFocus" : "TextureFocusSwap") + suffix];

	if (!textureApertureRead.srv || !textureApertureWrite.rtv || !textureReadFocus.rtv ||
		!textureFocusRead.srv || !textureFocusWrite.rtv)
		return;

	if (!historyValid[eye]) {
		static constexpr float clearColor[4] = {};
		globals::d3d::context->ClearRenderTargetView(textureApertureRead.rtv.get(), clearColor);
		globals::d3d::context->ClearRenderTargetView(textureFocusRead.rtv.get(), clearColor);
	}

	SetShaderResourceVariable("TexturePrevious", textureApertureRead.srv.get());
	if (ExecuteTechnique("Aperture", textureApertureWrite)) {
		apertureSRV[eye] = textureApertureWrite.srv.get();
		apertureFrame[eye] = globals::state->frameCount;
	}

	SetShaderResourceVariable("TextureAperture", textureApertureWrite.srv.get());
	ExecuteTechnique("ReadFocus", textureReadFocus);

	SetShaderResourceVariable("TexturePrevious", textureFocusRead.srv.get());
	SetShaderResourceVariable("TextureCurrent", textureReadFocus.srv.get());
	ExecuteTechnique("Focus", textureFocusWrite);
	historyIndex[eye] ^= 1;
	historyValid[eye] = true;

	SetShaderResourceVariable("TextureFocus", textureFocusWrite.srv.get());
	SetShaderResourceVariable("TextureOriginal", Util::AsReal(textureMain.SRV));

	const auto technique = selectedTechniqueIndex < uiTechniques.size() ? GetSelectedTechnique() : fallbackTechnique;
	auto [executed, inOutput, inTemp] = ExecuteTechniqueSequence(technique, Util::AsReal(textureMain.SRV), *textureHDRTemp, *textureHDRTemp2);

	if (executed && (inOutput || inTemp)) {
		auto* result = inOutput ? textureHDRTemp : textureHDRTemp2;
		manager.CopyTexture(manager.GetEyeCroppedSRV(*result), Util::AsReal(textureMain.RTV), false);
		ID3D11RenderTargetView* nullRTV = nullptr;
		globals::d3d::context->OMSetRenderTargets(1, &nullRTV, nullptr);
	}
}

void ENBDepthOfField::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		idApertureTime = settingManager.GetSettingID("ApertureTime", "DEPTHOFFIELD");
		idFocusingTime = settingManager.GetSettingID("FocusingTime", "DEPTHOFFIELD");
		idEnableAdaptation = settingManager.GetSettingID("EnableAdaptation", "EFFECT");
		idsCached = true;
	}

	ID3D11ShaderResourceView* adaptationSRV = nullptr;
	if (idEnableAdaptation != 0xFFFFFFFF && settingManager.GetValue<bool>(idEnableAdaptation)) {
		adaptationSRV = EffectManager::GetSingleton().enbAdaptation.GetHistorySRV();
	}
	SetShaderResourceVariable("TextureAdaptation", adaptationSRV);

	const float deltaTime = globals::game::deltaTime ? (*globals::game::deltaTime) : 0.0f;
	const float apertureTime = settingManager.GetValue<float>(idApertureTime);
	const float focusingTime = settingManager.GetValue<float>(idFocusingTime);

	const auto eye = std::max(0, EffectManager::GetSingleton().currentEyeIndex);
	float4 dofParameters{};
	dofParameters.z = historyValid[eye] ? std::clamp((apertureTime > 0.0f) ? (deltaTime / apertureTime) : 1.0f, 0.0f, 1.0f) : 1.0f;
	dofParameters.w = historyValid[eye] ? std::clamp((focusingTime > 0.0f) ? (deltaTime / focusingTime) : 1.0f, 0.0f, 1.0f) : 1.0f;

	SetVectorVariable("DofParameters", &dofParameters, sizeof(dofParameters));
}

ID3D11ShaderResourceView* ENBDepthOfField::GetApertureSRV() const
{
	const auto eye = std::max(0, EffectManager::GetSingleton().currentEyeIndex);
	return apertureFrame[eye] == globals::state->frameCount ? apertureSRV[eye] : nullptr;
}

void ENBDepthOfField::CreateEffectTextures()
{
	for (int eye = 0; eye < (globals::game::isVR ? 2 : 1); ++eye) {
		const std::string suffix = eye == 1 ? "Right" : "";
		effectTextureCache["TextureAperture" + suffix] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureAperture" + suffix);
		effectTextureCache["TextureApertureSwap" + suffix] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureApertureSwap" + suffix);
		effectTextureCache["TextureReadFocus" + suffix] = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureReadFocus" + suffix);
		effectTextureCache["TextureFocus" + suffix] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureFocus" + suffix);
		effectTextureCache["TextureFocusSwap" + suffix] = CreateTexture(1, 1, DXGI_FORMAT_R32_FLOAT, "ENBDepthOfField::TextureFocusSwap" + suffix);
	}
}
