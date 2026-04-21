#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/Streamline.h"
#include <array>
#include <d3d11_4.h>
#include <directx/d3d12.h>
#include <limits>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline bool SupportsVR() override { return true; }
	virtual inline bool IsCore() const override { return true; }
	virtual inline std::string_view GetCategory() const override { return "Display"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Advanced upscaling and frame generation technologies for improved performance",
			{ "DLSS (Deep Learning Super Sampling) support",
				"FSR (FidelityFX Super Resolution) support",
				"TAA (Temporal Anti-Aliasing) support",
				"Frame generation for supported systems" }
		};
	}

	float2 jitter = { 0, 0 };

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	static constexpr uint32_t kDLSSPresetMaxIndex = 4;  // 0=J, 1=K, 2=L, 3=M, 4=F
	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = 0;  // Default to DLAA / Native AA (1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance, 0=Native AA)
		uint dlssPreset = 1;   // 0=J, 1=K, 2=L, 3=M, 4=F (default K)
		uint frameLimitMode = 1;
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 0;
		uint streamlineLogLevel = 0;  // 0=Off, 1=Default, 2=Verbose
		float sharpnessFSR = 0.0f;
		float sharpnessDLSS = 0.1f;
		bool fsr4RuntimeEnable = true;
		bool fsr4AllowNonRx90Amd = false;
		bool fsr4AllowNvidia = false;
		bool vrPipelineDeduplication = false;
		bool foveatedVendorDispatch = false;
		float foveatedCenterArea = 0.6f;
		float foveatedLeftEyeMaskOffsetX = 0.0f;
		float foveatedLeftEyeMaskOffsetY = 0.0f;
		float foveatedRightEyeMaskOffsetX = 0.0f;
		float foveatedRightEyeMaskOffsetY = 0.0f;
		bool foveatedPeripheryEdgeBlur = false;
		float foveatedPeripheryEdgeBlurStrength = 1.0f;
		bool foveatedPeripheryMaskVisualization = false;
		bool periphery_taa_enable = false;
		bool periphery_taa_show_debug = false;
		uint periphery_taa_debug_mode = 0;
		float periphery_taa_center_blend_feather = 0.05f;
		bool periphery_taa_hmd_reprojection = false;
		bool periphery_taa_separate_hmd_rejection = false;
		bool periphery_taa_stabilize_motion = false;
		bool linkFoveatedCenterAreaWithSSGI = true;
		bool hasExplicitFoveatedCenterLinkPreference = false;
		bool reflexLowLatencyMode = true;
		bool reflexLowLatencyBoost = false;
		bool reflexUseMarkersToOptimize = true;
		bool reflexUseFPSLimit = false;
		float reflexFPSLimit = 60.0f;
	};

	Settings settings;

	struct JitterCB
	{
		float2 jitter;
		float useWideKernel;
		float pad0;
	};

	struct UpscalingDataCB
	{
		float2 dispatchDim;      // Current dispatch/output dimensions (per-eye in VR, full in flat)
		float2 trueSamplingDim;  // BufferDim.xy * ResolutionScale
		float2 invTrueSamplingDim;
		float seamCenterX;
		float seamHalfWidthPx;
		float maskDepthThreshold;
		float vrSeamHardening;
		float2 sourceOffset;  // Source offset in combined stereo inputs
		float2 outputOffset;  // Output offset in per-eye intermediates
	};

	struct FoveatedPeripheryCB
	{
		float2 outputDim;
		float2 invOutputDim;
		float2 invSourceDim;
		float2 sourceScale;
		float2 sourceOffset;
		float2 dispatchDim;
		float2 outputOffset;
		float2 jitter;
		float2 centerOffset;
		float2 pad0;
		float4 tuning0;  // x=centerScale, y=centerFeather, z/w reserved
		float4 tuning1;  // x=useEdgeBlur, y=edgeBlurStrength, z=edgeSensitivity, w reserved
		float4 tuning2;  // x=visualizeMask, y/z/w reserved
	};

	struct FoveatedCenterBlendCB
	{
		float2 invOutputDim;
		float centerScale;
		float centerFeather;
		float2 centerOffset;
		float2 outputOffset;
		float2 dispatchDim;
		float2 sourceOffset;
		float2 invSourceDim;
		float2 pad0;
	};

	struct PeripheryTAACB
	{
		float2 outputDim;
		float2 invOutputDim;
		float2 inputDim;
		float2 invInputDim;
		float2 dispatchDim;
		float2 outputOffset;
		float2 jitter;
		float2 centerOffset;
		float4 tuning0;  // x=centerScale, y=centerFeather, z=resetHistory, w=showDebug
		float4 tuning1;  // x=historyValid, y/z/w reserved
		float4 tuning2;  // x=reactivityScale, y=instabilityScale, z=velocityScale, w=lockDecay
		float4 tuning3;  // x=enableHmdReprojection, y=separateHmdRejection, z=enableMotionStabilization, w reserved
		float4x4 currentViewProjInverse;
		float4x4 previousViewProj;
		float4 currentCameraPosAdjust;
		float4 previousCameraPosAdjust;
		float4 debugParams;  // x=debugMode, y=motionMagnitudeScale, z=velocityDeltaScale, w reserved
	};

	static_assert(sizeof(FoveatedPeripheryCB) == 128, "FoveatedPeripheryCB layout changed; update HLSL cbuffer.");
	static_assert(sizeof(FoveatedCenterBlendCB) == 64, "FoveatedCenterBlendCB layout changed; update HLSL cbuffer.");
	static_assert(sizeof(PeripheryTAACB) == 304, "PeripheryTAACB layout changed; update HLSL cbuffer.");

	struct FoveatedDispatchRect
	{
		uint outputOffsetX = 0;
		uint outputOffsetY = 0;
		uint outputWidth = 0;
		uint outputHeight = 0;
		uint inputOffsetX = 0;
		uint inputOffsetY = 0;
		uint inputWidth = 0;
		uint inputHeight = 0;
	};

	struct FoveatedRectCacheState
	{
		uint inputWidthPerEye = 0;
		uint inputHeight = 0;
		uint outputWidthPerEye = 0;
		uint outputHeight = 0;
		bool isVR = false;
		float centerScale = -1.0f;
		std::array<float2, 2> centerOffsets{};
		std::array<FoveatedDispatchRect, 2> rects{};
	} foveatedRectCache;

	ConstantBuffer* jitterCB = nullptr;
	ConstantBuffer* upscalingDataCB = nullptr;
	ConstantBuffer* foveatedPeripheryCB = nullptr;
	ConstantBuffer* foveatedCenterBlendCB = nullptr;
	ConstantBuffer* peripheryTAACB = nullptr;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationDx12PathActive() const;
	bool IsFrameGenerationActive() const;
	float GetFrameGenerationFrameTime() const;
	bool IsUpscalingActive() const;

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	UpscaleMethod GetUpscaleMethod() const;

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCS[5];  // One for each UpscaleMethod
	ID3D11ComputeShader* GetEncodeTexturesCS();

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11ComputeShader> foveatedPeripheryCS;
	ID3D11ComputeShader* GetFoveatedPeripheryCS();

	winrt::com_ptr<ID3D11ComputeShader> foveatedCenterBlendCS;
	ID3D11ComputeShader* GetFoveatedCenterBlendCS();

	winrt::com_ptr<ID3D11ComputeShader> peripheryTAACS;
	ID3D11ComputeShader* GetPeripheryTAACS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	// Shared VR HMD Mask Clearing
	winrt::com_ptr<ID3D11ComputeShader> vrClearHMDMaskCS;
	winrt::com_ptr<ID3D11Buffer> vrClearHMDMaskCB;
	// Helper to dispatch mask clearing for a single eye region
	void ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
		uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX);

	// Shared VR Per-Eye Intermediate Buffers
	// Owned here so both Streamline (DLSS) and FidelityFX (FSR) can use them.
	eastl::unique_ptr<Texture2D> vrIntermediateColorIn[2];           // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateColorOut[2];          // per-eye output resolution
	eastl::unique_ptr<Texture2D> vrIntermediateDepth[2];             // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateMotionVectors[2];     // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateReactiveMask[2];      // per-eye render resolution
	eastl::unique_ptr<Texture2D> vrIntermediateTransparencyMask[2];  // per-eye render resolution

	// Helper to create/resize per-eye buffers matching source formats
	void CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);
	void EnsureVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
		ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc);

	// Helper: Create a Texture2D matching source format at a given size
	static eastl::unique_ptr<Texture2D> CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
		bool copyBindFlags = false, bool createSRV = false, bool createUAV = false, const char* name = nullptr);

	// Shared Pipeline Steps
	void PreparePerEyeInputs(ID3D11Resource* colorSrc, ID3D11Resource* depthSrc, ID3D11Resource* mvecSrc,
		ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc, bool copyAuxiliaryInputs = true, bool copyDepthInput = true);
	void FinalizePerEyeOutputs(ID3D11Resource* colorDst);

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	// D3D11 textures
	Texture2D* reactiveMaskTexture = nullptr;
	Texture2D* transparencyCompositionMaskTexture = nullptr;
	Texture2D* motionVectorCopyTexture = nullptr;
	Texture2D* sharpenerTexture = nullptr;
	eastl::unique_ptr<Texture2D> foveatedCenterColorIn[2];
	eastl::unique_ptr<Texture2D> foveatedCenterColorOut[2];
	eastl::unique_ptr<Texture2D> foveatedCenterDepth[2];
	eastl::unique_ptr<Texture2D> foveatedCenterMotionVectors[2];
	eastl::unique_ptr<Texture2D> foveatedCenterReactiveMask[2];
	eastl::unique_ptr<Texture2D> foveatedCenterTransparencyMask[2];
	eastl::unique_ptr<Texture2D> peripheryTAAHistoryColor[2][2];
	eastl::unique_ptr<Texture2D> peripheryTAAVelocityHistory[2][2];
	eastl::unique_ptr<Texture2D> peripheryTAALockHistory[2][2];
	uint32_t peripheryTAAHistoryReadIndex = 0;
	bool peripheryTAAHistoryValid = false;

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;
	static inline FidelityFX fidelityFX;  ///< AMD FidelityFX runtime for FSR upscaling and frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;  ///< Standalone RCAS sharpening for DLSS

	winrt::com_ptr<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;
	bool depthUpscaleUseWideKernel = false;
	bool historyResetRequested = true;
	bool historyResetThisFrame = false;
	uint32_t historyResetLatchedFrame = std::numeric_limits<uint32_t>::max();
	bool historyResetTrackingInitialized = false;
	float2 previousHistoryScreenSize = { 0.0f, 0.0f };
	float2 previousHistoryResolutionScale = { 1.0f, 1.0f };
	bool previousHistoryInWorld = false;
	bool previousHistoryInMapMenu = false;
	UpscaleMethod previousHistoryUpscaleMethod = UpscaleMethod::kNONE;
	bool previousHistoryVRPipelineDedup = false;
	bool previousHistoryFoveatedDispatch = false;
	float previousHistoryFoveatedCenterArea = 1.0f;
	std::array<float2, 2> previousHistoryFoveatedCenterOffsets = {};
	bool previousHistoryPeripheryTAA = false;
	bool previousHistoryPeripheryTAAPathActive = false;
	bool previousHistoryPeripheryTAAHmdReprojection = false;
	bool previousHistoryPeripheryTAASeparateHmdRejection = false;
	bool previousHistoryPeripheryTAAStabilizeMotion = false;
	bool previousHistoryFSRRuntimePathActive = false;

	void CopySharedD3D12Resources();
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();
	void RequestHistoryReset();
	bool ShouldResetHistoryThisFrame() const;
	void UpdateHistoryResetState(UpscaleMethod a_upscaleMethod);
	void LatchHistoryResetForCurrentFrame();
	bool IsVRPipelineDedupActive(UpscaleMethod a_upscaleMethod) const;
	bool IsFSRRuntimePathActive(UpscaleMethod a_upscaleMethod) const;
	bool IsFoveatedVendorDispatchEnabled(UpscaleMethod a_upscaleMethod) const;
	bool IsPeripheryTAAEnabled(UpscaleMethod a_upscaleMethod) const;
	bool IsPeripheryTAAPathActive(UpscaleMethod a_upscaleMethod) const;
	float2 GetDefaultFoveatedMaskCenterOffset(uint32_t eyeIndex) const;
	std::array<float2, 2> GetDefaultFoveatedMaskCenterOffsets() const;
	float2 GetResolvedFoveatedMaskCenterOffset(uint32_t eyeIndex) const;
	std::array<float2, 2> GetResolvedFoveatedMaskCenterOffsets() const;
	bool BuildFoveatedDispatchRects(uint32_t inputWidthPerEye, uint32_t inputHeight, uint32_t outputWidthPerEye, uint32_t outputHeight, bool isVR, float centerScale);
	bool EnsureFoveatedTexture(eastl::unique_ptr<Texture2D>& texture, ID3D11Resource* source, uint32_t width, uint32_t height, bool copyBindFlags, bool createSRV, bool createUAV, bool createRTV, const char* name);
	void DestroyFoveatedResources();
	bool EnsurePeripheryTAAResources(uint32_t outputWidthPerEye, uint32_t outputHeight, ID3D11Resource* colorSource);
	void DestroyPeripheryTAAResources();
	bool DispatchFoveatedVendorUpscaling(UpscaleMethod a_upscaleMethod, ID3D11Resource* colorTexture, ID3D11Resource* depthTexture, ID3D11Resource* motionVectors, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11ShaderResourceView* colorSRV, bool depthAlreadyPrepared);
	bool DispatchSingleFoveatedVendorEye(UpscaleMethod a_upscaleMethod, uint32_t eyeIndex, ID3D11Resource* colorIn, ID3D11Resource* depthIn, ID3D11Resource* motionVectorsIn, ID3D11Resource* reactiveMaskIn, ID3D11Resource* transparencyMaskIn, uint32_t outputWidthPerEye, uint32_t outputHeight, ID3D11Resource* historyColorResource = nullptr, ID3D11UnorderedAccessView* historyColorUAV = nullptr, uint32_t colorInputBaseOffsetX = 0, uint32_t depthInputBaseOffsetX = 0, uint32_t auxInputBaseOffsetX = 0);
	void DispatchFoveatedPeripheryPass(ID3D11ShaderResourceView* sourceSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t outputWidth, uint32_t outputHeight, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, bool keepBindingsBound = false, float sourceScaleX = 1.0f, float sourceScaleY = 1.0f, float sourceOffsetX = 0.0f, float sourceOffsetY = 0.0f, float centerOffsetX = 0.0f, float centerOffsetY = 0.0f);
	void DispatchPeripheryTAAPass(ID3D11ShaderResourceView* currentColorSRV, ID3D11ShaderResourceView* currentDepthSRV, ID3D11ShaderResourceView* currentMotionVectorSRV,
		ID3D11ShaderResourceView* currentReactiveSRV, ID3D11ShaderResourceView* currentTransparencySRV, ID3D11ShaderResourceView* historyColorSRV,
		ID3D11ShaderResourceView* historyVelocitySRV, ID3D11ShaderResourceView* historyLockSRV, ID3D11UnorderedAccessView* outputColorUAV,
		ID3D11UnorderedAccessView* outputHistoryColorUAV,
		ID3D11UnorderedAccessView* outputVelocityUAV, ID3D11UnorderedAccessView* outputLockUAV, uint32_t inputWidth, uint32_t inputHeight,
		uint32_t outputWidth, uint32_t outputHeight, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight,
		const float4x4& currentViewProjInverse, const float4x4& previousViewProj, const float4& currentCameraPosAdjust, const float4& previousCameraPosAdjust,
		bool resetHistory, float centerOffsetX, float centerOffsetY);
	void DispatchFoveatedBlendPass(ID3D11ShaderResourceView* centerSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t eyeIndex, uint32_t outputWidthPerEye, uint32_t outputHeight, const FoveatedDispatchRect& rect, uint32_t dispatchOffsetX, uint32_t dispatchOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, float centerFeather);

	/**
	 * @brief Applies RCAS sharpening to the main render target after DLSS upscaling.
	 *
	 * Runs in HDR space before tonemapping. Only called when DLSS is active and sharpness > 0.
	 */
	void ApplySharpening();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	void SetUIBuffer();
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(ID3D11Device* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

private:
	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetScissorRect
	{
		static void thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
