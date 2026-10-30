#pragma once

#include <array>
#include <atomic>
#include <d3d11_4.h>
#include <d3d12.h>
#include <string>
#include <string_view>
#include <winrt/base.h>

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>

#include <FidelityFX/api/include/ffx_api.hpp>
#include <FidelityFX/api/include/ffx_api_loader.h>
#include <FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp>
#include <FidelityFX/framegeneration/include/ffx_framegeneration.hpp>
#include <FidelityFX/upscalers/include/ffx_upscale.hpp>

#include "../../Buffer.h"
#include "../../State.h"
#include "DX12SwapChain.h"

/** @brief Manages FidelityFX upscaling and frame generation with host FSR3 fallback and runtime provider discovery. */
class FidelityFX
{
public:
	static constexpr const wchar_t* PluginDir = L"Data\\Shaders\\Upscaling\\FidelityFX";
	// Host-linked FSR3 SDK version, used to distinguish it from a runtime-provided upscaler version.
	static constexpr uint32_t Fsr3Version = FFX_UPSCALER_MAKE_VERSION(FFX_FSR3_VERSION_MAJOR, FFX_FSR3_VERSION_MINOR, FFX_FSR3_VERSION_PATCH);
	// Optional AMD-distributed DLL providing a runtime-substitutable upscaler.
	static constexpr std::wstring_view RuntimeUpscalerDllName = L"amd_fidelityfx_upscaler_dx12.dll";
	static constexpr std::string_view RuntimeUpscalerDllNameUtf8 = "amd_fidelityfx_upscaler_dx12.dll";
	~FidelityFX();

	HMODULE module = nullptr;

	ffx::Context swapChainContext{};
	ffx::Context frameGenContext;
	FfxFsr3Context fsrContext[2];

	bool featureFSR3FG = false;
	bool featureRuntimeUpscaler = false;

	// Track if FidelityFX is currently being used for frame generation
	bool isFrameGenActive = false;

	// Track HDR state for frame generation callback (needs to be accessible from static callback)
	// Using atomic for thread safety since async workloads may read this from different threads
	static inline std::atomic<bool> isHDRActive = false;
	static inline std::atomic<float> hdrPeakNits = 1000.0f;
	static inline std::atomic<bool> needsReset = false;

	// Track previous HDR parameters to detect changes that require FG reset
	bool prevHDRActive = false;
	float prevPeakNits = 1000.0f;

	// Cached DLL version info for FidelityFX plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	/** @brief Loads the FidelityFX loader and runtime DLLs from the plugin directory. */
	void LoadFFX();
	/** @brief Creates the FidelityFX frame generation context for the current swap chain. */
	void SetupFrameGeneration();
	/**
	 * @brief Presents the current frame, optionally performing FidelityFX frame generation.
	 * @param a_useFrameGeneration Whether to enable frame generation for this present call.
	 * @param a_isHDR Whether HDR output is active, affecting color space configuration.
	 */
	void Present(bool a_useFrameGeneration, bool a_isHDR = false);

	/** @brief Creates FSR3 upscaling resources including scratch buffers and context. */
	void CreateFSRResources();

	/** @brief Destroys FSR3 upscaling resources and frees the scratch buffer. */
	void DestroyFSRResources();

	// a_colorOut, if non-null, redirects the upscaled result to a displayRes target instead
	// of writing back into a_upscalingTexture (needed when kMAIN is renderRes, e.g. PerfMode).
	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_depth, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness, ID3D11Resource* a_colorOut = nullptr);

	/** @brief True if the active adapter is an AMD GPU. */
	bool IsAmdAdapterDetected() const;
	/** @brief True if the active adapter is an NVIDIA GPU. */
	bool IsNvidiaAdapterDetected() const;
	/** @brief True if the runtime upscaler DLL was found and loaded. */
	bool IsRuntimeUpscalerPresent() const;
	/** @brief Queries device-compatible providers and confirms the default upgrade by creating its context. */
	void ProbeRuntimeUpscalerUpgrade(uint32_t a_displayWidth, uint32_t a_displayHeight);
	/** @brief True after a newer runtime provider successfully created a context on the active device. */
	bool IsRuntimeUpgradeAvailable() const;
	/** @brief Describes the confirmed runtime upgrade provider. */
	const std::string& GetRuntimeUpgradeProviderName() const { return runtimeUpgradeProviderName; }
	/** @brief Explains unavailable upgrades or a latched runtime failure. */
	const std::string& GetRuntimeUpgradeUnavailableReason() const { return runtimeUpgradeUnavailableReason; }
	/** @brief True if the boot setting selects the confirmed runtime upgrade. */
	bool ShouldRequestRuntimeUpgrade() const;
	/** @brief True if the runtime upscaler provider must handle FSR dispatch this frame. */
	bool ShouldUseRuntimeUpscalerForFSR() const;
	/** @brief Human-readable label for the frame path the runtime upscaler last dispatched through. */
	const std::string& GetRuntimeUpscalerLastFramePathLabel() const;
	/** @brief Human-readable label for the FSR path actually shown to the user this frame. */
	const std::string& GetDisplayedFsrPathLabel() const;
	/** @brief Human-readable label for the host-linked FSR3 SDK version. */
	static const std::string& GetHostFsrSdkLabel();
	/** @brief Name of the currently loaded runtime upscaler provider. */
	std::string GetRuntimeUpscalerProviderName() const;
	/** @brief True if runtime upscaler GPU resources are currently allocated. */
	bool HasRuntimeUpscalerResources() const;
	/** @brief Polls whether a pending runtime upscaler resource teardown has finished. */
	bool PollRuntimeUpscalerTeardownReady();
	/** @brief Releases runtime upscaler resources ahead of a provider relatch attempt. */
	void ReleaseRuntimeUpscalerResourcesForRelatch(bool a_waitForIdle = true);
	/** @brief Resets the host FSR3 GPU-idle fence used before context teardown. */
	void ResetFSRIdleFence();
	/** @brief Tears down runtime upscaler resources, optionally invalidating the cached provider. */
	void ResetRuntimeUpscalerResources(bool a_invalidateProviderCache = false);

	/**
	 * @brief Upscales one context (eye) via the runtime provider or the host FSR3 SDK. Per-context/eye
	 * dispatch primitive used by Upscale(); chooses runtime vs host FSR3.
	 * @param a_contextIndex Context/eye index; must be less than the active context count.
	 * @param a_renderWidth Render-resolution width of the input resources.
	 * @param a_renderHeight Render-resolution height of the input resources.
	 * @param a_displayWidth Display-resolution width of a_output.
	 * @param a_displayHeight Display-resolution height of a_output.
	 * @param a_motionVectorScaleX Motion-vector X scale to normalize into FFX's expected units.
	 * @param a_motionVectorScaleY Motion-vector Y scale to normalize into FFX's expected units.
	 * @param a_forceHostPath Skip the runtime provider and dispatch through the host FSR3 SDK
	 *  unconditionally. The runtime provider's shared DX12 interop resources are sized at
	 *  full-frame creation time; a caller dispatching a smaller region (e.g. foveated rendering)
	 *  must force the host path, whose context already supports dynamic per-dispatch render size.
	 * @return True if the region was upscaled.
	 */
	bool UpscaleRegion(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
		float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness, bool a_forceHostPath = false);

private:
	// Bounded poll for GPU idle before destroying the host FSR3 context; see
	// RuntimeUpscaler.cpp for the shared D3D11 fence-poll primitive.
	void WaitForHostFsrIdle();

	// FSR scratch buffer - needs to be freed in DestroyFSRResources
	void* fsrScratchBuffer = nullptr;

	// Flag to prevent spamming the log with FSR3 dispatch crash messages
	bool fsrDispatchCrashLogged = false;

	uint32_t runtimeUpscalerContextCount = 0;
	uint32_t runtimeUpscalerMaxRenderWidth = 0;
	uint32_t runtimeUpscalerMaxRenderHeight = 0;
	uint32_t runtimeUpscalerMaxDisplayWidth = 0;
	uint32_t runtimeUpscalerMaxDisplayHeight = 0;
	uint32_t runtimeUpscalerRequestedVersion = 0;
	D3D11_TEXTURE2D_DESC runtimeColorSharedDesc{};
	D3D11_TEXTURE2D_DESC runtimeDepthSharedDesc{};
	D3D11_TEXTURE2D_DESC runtimeMotionSharedDesc{};
	D3D11_TEXTURE2D_DESC runtimeReactiveSharedDesc{};
	D3D11_TEXTURE2D_DESC runtimeTransparencySharedDesc{};
	D3D11_TEXTURE2D_DESC runtimeOutputSharedDesc{};
	ffx::Context runtimeUpscalerContexts[2]{};

	SharedFence runtimeFence;
	ID3D11Query* pendingFSRResourceFreeIdleFence = nullptr;
	uint64_t pendingRuntimeTeardownD3D11FenceValue = 0;
	uint64_t pendingRuntimeTeardownD3D12FenceValue = 0;

	static constexpr uint32_t kRuntimeCommandContextCount = 8;
	struct RuntimeCommandContext
	{
		winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		uint64_t fenceValue = 0;
	};
	std::array<RuntimeCommandContext, kRuntimeCommandContextCount> runtimeCommandContexts;
	uint32_t runtimeCommandContextCursor = 0;

	WrappedResource* runtimeColorShared[2]{};
	WrappedResource* runtimeDepthShared[2]{};
	WrappedResource* runtimeMotionShared[2]{};
	WrappedResource* runtimeReactiveShared[2]{};
	WrappedResource* runtimeTransparencyShared[2]{};
	WrappedResource* runtimeOutputShared[2]{};

	HMODULE frameGenerationModule = nullptr;
	HMODULE runtimeUpscalerModule = nullptr;

	enum class RuntimeUpscalerFramePath : uint8_t
	{
		kInactive = 0,
		kHostFsr31 = 1,
		kRuntimeFsr31 = 2,
		kRuntimeUpgrade = 3,
		kHostFsr31Fallback = 4
	};

	bool runtimeUpscalerFailureLatched = false;
	bool runtimeUpgradeFailureLatched = false;
	// A provider fault can leave DX12 state unsafe, so resource recreation must not re-arm it.
	bool runtimeUpscalerQuarantined = false;
	uint32_t runtimeFallbackResetDispatchesRemaining = 0;
	bool runtimeUpscalerLastFramePathValid = false;
	uint32_t runtimeUpscalerLastFrameIndex = 0;
	RuntimeUpscalerFramePath runtimeUpscalerLastFramePath = RuntimeUpscalerFramePath::kInactive;
	std::string runtimeUpscalerLastFrameProviderName;

	bool runtimeUpgradeProbeComplete = false;
	bool runtimeUpgradeAvailable = false;
	std::string runtimeUpgradeProviderName;
	std::string runtimeUpgradeUnavailableReason = "Runtime upgrade has not been checked yet.";

	uint64_t runtimeUpscalerProviderMatchedVersionId = 0;
	std::string runtimeUpscalerProviderMatchedVersionName;

	bool CanUseRuntimeUpscalerPath();
	// True if an earlier eye already published a runtime-provider (upgrade/runtime-FSR3) frame
	// this same frame index -- a later eye must not fall through to the host FSR3 SDK, which
	// would present a mixed-provider stereo pair. See UpscaleRegion's failure paths.
	bool WasRuntimeUpscalerUsedThisFrame() const;
	void ResetRuntimeUpscalerTracking(bool a_invalidateProviderCache);
	void LatchRuntimeUpscalerFailure();
	void LatchRuntimeUpgradeFailure();
	// Retires the runtime provider for the rest of the session: latches the ordinary
	// failure flag immediately (so no eye attempts it again this frame) and marks it
	// quarantined so a later ResetRuntimeUpscalerTracking(true) cannot re-arm it.
	void QuarantineRuntimeUpscalerForSession(const char* a_reason);
	RuntimeUpscalerFramePath GetRuntimeUpscalerProviderFramePath(uint32_t a_requestedVersion) const;
	void RecordRuntimeUpscalerFramePath(RuntimeUpscalerFramePath a_path);
	bool EnsureRuntimeUpscalerInterop();
	bool EnsureRuntimeCommandContexts();
	RuntimeCommandContext* AcquireRuntimeCommandContext();
	void ResetRuntimeCommandContexts();
	bool WaitForRuntimeD3D12Fence(uint64_t a_value);
	bool EnsureRuntimeUpscalerContexts(uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight, uint32_t a_contextCount, uint32_t a_requestedVersion);
	void WaitForRuntimeUpscalerIdle();
	bool PollRuntimeUpscalerTeardownIdle();
	bool EnsureRuntimeUpscalerSharedResources(uint32_t a_contextCount, uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight,
		const D3D11_TEXTURE2D_DESC& a_colorDesc,
		const D3D11_TEXTURE2D_DESC& a_depthDesc,
		const D3D11_TEXTURE2D_DESC& a_motionDesc,
		const D3D11_TEXTURE2D_DESC& a_reactiveDesc,
		const D3D11_TEXTURE2D_DESC& a_transparencyDesc,
		const D3D11_TEXTURE2D_DESC& a_outputDesc);
	bool DispatchRuntimeUpscalerSingle(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
		uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
		float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness);
	void DestroyRuntimeUpscalerContexts(bool a_waitForIdle = true);
	void DestroyRuntimeUpscalerResources(bool a_waitForIdle = true);
};
