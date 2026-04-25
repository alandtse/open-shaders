#pragma once

#include "Buffer.h"

struct ScreenSpaceGI : Feature
{
private:
	static constexpr std::string_view MOD_ID = "130375";

public:
	bool inline SupportsVR() override { return true; }

	virtual inline std::string GetName() override { return "Screen Space GI"; }
	virtual inline std::string GetShortName() override { return "ScreenSpaceGI"; }
	virtual inline bool IsCore() const override { return true; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		std::string desc =
			"Screen Space Global Illumination adds realistic indirect lighting and ambient occlusion.";
		if (REL::Module::IsVR()) {
			desc +=
				" In VR, use AO preset with Full Res for best quality at a cost of speed. For more performance use Foveated or Half/Quarter Res setting. Foveated is not compatible with IL.";
		}
		return std::make_pair(
			desc,
			std::vector<std::string>{
				"Realistic indirect lighting",
				"Enhanced ambient occlusion",
				"Improved visual depth and atmosphere",
				"Temporal denoising for smooth results",
				"Configurable quality and performance settings" });
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();
	bool ShadersOK();

	void DrawSSGI();
	void UpdateSB();

	//////////////////////////////////////////////////////////////////////////////////

	bool recompileFlag = false;
	uint outputAoIdx = 0;
	uint outputIlIdx = 0;

	struct CenterDispatchRect
	{
		uint x = 0;
		uint y = 0;
		uint width = 0;
		uint height = 0;
	};

	struct CenterRectCacheState
	{
		uint frameWidth = 0;
		uint frameHeight = 0;
		bool isVR = false;
		float scale = -1.0f;
		float horizontalScale = 1.0f;
		std::array<float2, 2> centerOffsets{};
		std::array<CenterDispatchRect, 2> rects{};
	} centerRectCache;

	static constexpr int kResourceProfileFullGI = 0;
	static constexpr int kResourceProfileAOOnly = 1;

	struct Settings
	{
		bool Enabled = REL::Module::IsVR() ? false : true;   // disabled in VR by default
		bool EnableGI = REL::Module::IsVR() ? false : true;  // AO only for VR by default
		bool EnableExperimentalSpecularGI = false;
		bool EnableVanillaSSAO = false;
		bool AOInteriorsOnly = REL::Module::IsVR() ? true : false;
		bool ILInteriorsOnly = REL::Module::IsVR() ? true : false;
		// performance/quality
		uint NumSlices = REL::Module::IsVR() ? 3u : 4u;
		uint NumSteps = REL::Module::IsVR() ? 6u : 8u;   // AO preset for VR
		bool EnableAdaptiveSampling = true;
		int ResolutionMode = 0;  // Full Res default (VR and flat)
		int ResourceProfile = REL::Module::IsVR() ? kResourceProfileAOOnly : kResourceProfileFullGI;
		float VRCullDistance = 1500.0f;                  // 0 disables VR distance culling
		float CenterFullResMaskScale = 0.0f;             // runtime cache; foveated presets derive this from the active Upscaling FOV profile
		int FoveatedPresetMode = 0;                      // 0=off, 1=strict foveated, 2=foveated
		// visual
		float MinScreenRadius = 0.01f;
		float AORadius = 256.f;
		float GIRadius = 256.f;
		float Thickness = 32.f;
		float2 DepthFadeRange = { 4e4, 5e4 };
		// gi
		float GISaturation = 0.8f;
		float GIDistanceCompensation = 0.f;
		// mix
		float AOPower = 1.8f;
		float GIStrength = 1.0f;
		// denoise
		bool EnableTemporalDenoiser = REL::Module::IsVR() ? false : true;
		bool EnableBlur = REL::Module::IsVR() ? false : true;
		float DepthDisocclusion = .1f;
		float NormalDisocclusion = .1f;
		uint MaxAccumFrames = 16;
		float BlurRadius = 2.f;
		float DistanceNormalisation = 2.f;
	} settings;

	int activeResourceProfile = REL::Module::IsVR() ? kResourceProfileAOOnly : kResourceProfileFullGI;

	bool HasGIResources() const { return activeResourceProfile == kResourceProfileFullGI; }
	bool IsResourceProfileRestartPending() const { return settings.ResourceProfile != activeResourceProfile; }
	bool IsGIActive() const { return settings.EnableGI && HasGIResources(); }
	bool IsSpecularGIActive() const { return IsGIActive() && settings.EnableExperimentalSpecularGI; }

	struct SSGICB
	{
		float4x4 PrevInvViewMat[2];
		float2 NDCToViewMul[2];
		float2 NDCToViewAdd[2];

		float2 TexDim;
		float2 RcpTexDim;  //
		float2 FrameDim;
		float2 RcpFrameDim;  //
		uint FrameIndex;

		uint NumSlices;
		uint NumSteps;

		float MinScreenRadius;  //
		float AORadius;
		float GIRadius;
		float EffectRadius;
		float Thickness;  //
		float2 DepthFadeRange;
		float DepthFadeScaleConst;

		float GISaturation;  //
		float GIDistanceCompensation;
		float GICompensationMaxDist;
		float pad1;

		float AOPower;  //
		float GIStrength;

		float DepthDisocclusion;
		float NormalDisocclusion;
		uint MaxAccumFrames;  //

		float BlurRadius;
		float DistanceNormalisation;
		float VRCullDistance;
		float CenterFullResMaskScale;
		float4 CenterFullResMaskOffsets;
		float CenterFullResMaskHorizontalScale;
		float CenterFullResMaskFeather;
		float CenterDispatchOffsetX;
		float CenterDispatchOffsetY;
		float CenterDispatchSizeX;
		float CenterDispatchSizeY;
		float pad[2];
	};
	STATIC_ASSERT_ALIGNAS_16(SSGICB);
	eastl::unique_ptr<ConstantBuffer> ssgiCB;
	SSGICB ssgiCBData{};

	eastl::unique_ptr<Texture2D> texNoise = nullptr;
	eastl::unique_ptr<Texture2D> texWorkingDepth = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavWorkingDepth[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texPrevGeo = nullptr;
	eastl::unique_ptr<Texture2D> texRadiance = nullptr;
	eastl::unique_ptr<Texture2D> texRadianceTemp = nullptr;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavRadiance[5] = { nullptr };
	eastl::unique_ptr<Texture2D> texAccumFrames[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texAo[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlY[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texIlCoCg[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texGiSpecular[2] = { nullptr };
	eastl::unique_ptr<Texture2D> texCenterAo = nullptr;
	eastl::unique_ptr<Texture2D> texCenterIlY = nullptr;
	eastl::unique_ptr<Texture2D> texCenterIlCoCg = nullptr;
	eastl::unique_ptr<Texture2D> texCenterGiSpecular = nullptr;

	inline std::tuple<ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*> GetOutputTextures()
	{
		if (!(loaded && settings.Enabled) || outputAoIdx >= 2 || outputIlIdx >= 2 || !texAo[outputAoIdx])
			return { nullptr, nullptr, nullptr, nullptr };

		return {
			texAo[outputAoIdx]->srv.get(),
			texIlY[outputIlIdx] ? texIlY[outputIlIdx]->srv.get() : nullptr,
			texIlCoCg[outputIlIdx] ? texIlCoCg[outputIlIdx]->srv.get() : nullptr,
			texGiSpecular[outputAoIdx] ? texGiSpecular[outputAoIdx]->srv.get() : nullptr
		};
	}

	winrt::com_ptr<ID3D11SamplerState> linearClampSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> pointClampSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prefilterDepthsCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> prefilterRadianceCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> radianceDisoccAOOnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> giAOOnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> centerGIMaskedCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> centerGIMaskedAOOnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> upsampleAOOnlyCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> centerBlendCompute = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> centerBlendAOOnlyCompute = nullptr;
};
