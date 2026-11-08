#pragma once

#include "ExtendedEffect.h"

class ENBAdaptation : public EffectBase
{
public:
	virtual std::string GetName() const override { return "enbadaptation.fx"; }

	virtual void Execute() override;
	virtual void UpdateEffectVariables() override;

	/** @brief Returns the last completed exposure, shared by both eyes, or null when adaptation is unavailable. */
	ID3D11ShaderResourceView* GetHistorySRV() const;
	/** @brief Releases the sun mask shader so it recompiles on the next frame it is needed. */
	void ClearShaderCache();

	TextureManager::Texture textureCurrent;
	TextureManager::Texture textureSunMasked;

protected:
	void CreateEffectTextures() override;

private:
	TextureManager::Texture* adaptationTextures[2] = {};
	uint32_t historyIndex = 0;
	bool historyValid = false;
	struct alignas(16) SunMaskCB
	{
		Matrix CameraViewProj;
		Matrix CameraViewProjInverse;
		float4 SunDirection;
		float4 DynamicResolution;
		float4 MaskParams;
	};

	/** @brief Compiles the sun mask shader and creates its constant buffer and sampler on first use. */
	bool EnsureSunMaskResources();
	/**
	 * @brief Replaces the [PROCEDURALSUN] disc with the surrounding sky in the image adaptation measures.
	 * @return The masked image, or a_source when masking is off or unavailable.
	 */
	ID3D11ShaderResourceView* MaskProceduralSun(ID3D11ShaderResourceView* a_source);

	winrt::com_ptr<ID3D11PixelShader> sunMaskPS;
	bool sunMaskPSFailed = false;
	winrt::com_ptr<ID3D11Buffer> sunMaskCB;
	winrt::com_ptr<ID3D11SamplerState> sunMaskSampler;

	uint32_t idForceMinMax = 0xFFFFFFFF;
	uint32_t idAdaptTime = 0xFFFFFFFF;
	uint32_t idAdaptMin = 0xFFFFFFFF;
	uint32_t idAdaptMax = 0xFFFFFFFF;
	uint32_t idAdaptSens = 0xFFFFFFFF;
	bool idsCached = false;
};
