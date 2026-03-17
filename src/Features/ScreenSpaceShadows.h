#pragma once

#include "Buffer.h"

struct ScreenSpaceShadows : Feature
{
private:
	static constexpr std::string_view MOD_ID = "93209";

public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline bool IsCore() const override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\n"
			"This technique adds fine-detail shadows that traditional shadow mapping might miss.",
			{ "Enhanced contact shadows",
				"Improved shadow detail",
				"Better shadow accuracy",
				"Fine-scale shadow effects",
				"Configurable shadow contrast" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = 0.02f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 1.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		float VRBaseSamplesAtReference = 44.0f;
		float VRCullDistance = 0.0f;  // 0 = disabled
		uint pad0[1];
	};

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;

		uint DynamicSampleCount;
		uint DynamicReadCount;
		float pad0[2];

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchCS = nullptr;
	ID3D11ComputeShader* raymarchRightCS = nullptr;
	uint compiledSampleCount = 0;
	uint compiledSampleCountRight = 0;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void ClearShaderCache() override;
	uint GetScaledSampleCount(bool a_dynamic);
	ID3D11ComputeShader* GetOrCreateRaymarchShader(ID3D11ComputeShader*& a_shader, uint& a_compiledSampleCount, bool a_rightEye);
	ID3D11ComputeShader* GetComputeRaymarch();
	ID3D11ComputeShader* GetComputeRaymarchRight();

	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	void DrawShadows();

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };
};
