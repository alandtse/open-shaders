#include "ENBEffectPostPass.h"

#include "../EffectManager.h"
#include "../TextureManager.h"

void ENBEffectPostPass::Execute()
{
	auto& textureManager = TextureManager::GetSingleton();

	auto textureSDRTemp = textureManager.GetCommonTexture("TextureSDRTemp");
	auto textureSDRTemp2 = textureManager.GetCommonTexture("TextureSDRTemp2");

	if (!textureSDRTemp || !textureSDRTemp2) {
		return;
	}

	// Crop explicitly here: this first pass skips ExecuteTechniqueSequence's own crop step,
	// so without this the eye's pass would sample both eyes.
	auto* inputSRV = EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp);
	auto [executed, inOutput] = ExecuteTechniqueSequence(GetSelectedTechnique(), inputSRV, *textureSDRTemp2, *textureSDRTemp);

	if (executed && inOutput) {
		textureManager.SwapTextures("TextureSDRTemp", "TextureSDRTemp2");
	}
}

void ENBEffectPostPass::UpdateEffectVariables()
{
	auto* textureSDRTemp = GetCachedCommonTexture("TextureSDRTemp");
	SetShaderResourceVariable("TextureOriginal", textureSDRTemp ? EffectManager::GetSingleton().GetEyeCroppedSRV(*textureSDRTemp) : nullptr);
}