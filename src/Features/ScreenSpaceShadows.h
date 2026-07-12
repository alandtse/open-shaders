#pragma once

#include "Buffer.h"

struct ScreenSpaceShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_shadows.name", "Screen Space Shadows"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.screen_space_shadows.description", "Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\nThis technique adds fine-detail shadows that traditional shadow mapping might miss."),
			{ T("feature.screen_space_shadows.key_feature_1", "Enhanced contact shadows"),
				T("feature.screen_space_shadows.key_feature_2", "Improved shadow detail"),
				T("feature.screen_space_shadows.key_feature_3", "Better shadow accuracy"),
				T("feature.screen_space_shadows.key_feature_4", "Fine-scale shadow effects"),
				T("feature.screen_space_shadows.key_feature_5", "Configurable shadow contrast") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = !globals::game::isVR ? 0.02f : 0.010f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = !globals::game::isVR ? 1.0f : 4.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		uint pad0[3];
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

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	bool enableStereoSync = true;
	// Route the VR stereo step through the view-independent reproject path (transfer eye 0's
	// shadow to eye 1) instead of the bilateral sync. Default on for the perf win (eye-1
	// raymarch skipped).
	bool useStereoReproject = true;
	// Dev viz: paint true-disocclusion eye-1 pixels black to measure the reproject gap.
	bool debugReprojectDisocclusion = false;

	struct alignas(16) StereoSyncCB
	{
		float FrameDim[2];
		float RcpFrameDim[2];
	};
	STATIC_ASSERT_ALIGNAS_16(StereoSyncCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchCS = nullptr;
	ID3D11ComputeShader* raymarchRightCS = nullptr;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	// VR stereo sync resources
	Texture2D* stereoSyncCopyTex = nullptr;
	ConstantBuffer* stereoSyncCB = nullptr;
	ID3D11ComputeShader* stereoSyncCS = nullptr;
	ID3D11ComputeShader* stereoReprojectCS = nullptr;
	ID3D11ComputeShader* stereoReprojectDebugCS = nullptr;
	// Per-variant latches: a dev-only debug-variant compile failure must not disable the
	// production reproject path.
	bool stereoReprojectCompileFailed = false;
	bool stereoReprojectDebugCompileFailed = false;

	/** @brief Lazily compiles and returns the active reproject variant; null (latched) on compile failure. */
	ID3D11ComputeShader* GetStereoReprojectCS();

	/** @brief Creates the raymarch constant buffer, point border sampler, and shadow output texture. */
	virtual void SetupResources() override;

	/** @brief Draws the ImGui settings UI for screen-space shadow configuration. */
	virtual void DrawSettings() override;
	virtual void DrawVRPerformanceSettings() override;
	std::string GetVRPerformanceSectionLabel() override { return GetDisplayName(); }
	int GetVRPerformanceOrder() const override { return 30; }
	virtual void ApplyVRPerformanceProfile(VRPerfProfile profile) override;
	bool MatchesVRPerformanceProfile(VRPerfProfile profile) const override;
	/// @brief Renders the VR stereo sync/reprojection toggles. Shared by the SSS panel and
	/// the VR Performance hub. VR-only; caller guards on isVR.
	void DrawStereoToggles();

	/** @brief Releases the compiled raymarch compute shader for recompilation. */
	virtual void ClearShaderCache() override;
	/** @brief Releases the raymarch compute shader so it is recompiled on next use. */
	void InvalidateRaymarchShaders();
	/** @brief Calculates the resolution-scaled and quantized sample count for the raymarch shader. */
	uint GetScaledSampleCount();
	uint lastCompiledSampleCount = 0;
	/**
	 * @brief Returns the compiled raymarch compute shader, recompiling if the sample count changed.
	 * @return The compiled ID3D11ComputeShader, or nullptr on failure.
	 */
	ID3D11ComputeShader* GetComputeRaymarch();
	ID3D11ComputeShader* GetComputeRaymarchRight();

	/** @brief Clears the shadow texture and dispatches shadow ray marching if conditions are met. */
	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Dispatches the Bend SSS compute shader to generate screen-space contact shadows. */
	void DrawShadows();
	void DrawStereoSync();

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };
};
