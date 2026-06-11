#pragma once

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "Utils/BootSnapshot.h"
#include <d3d11.h>
#include <unordered_map>
#include <winrt/base.h>

/**
 * @brief VR Stereo Rendering Optimizations feature.
 *
 * Uses hardware stencil culling to skip Eye 1 pixel shading for pixels that can be
 * reprojected from Eye 0 via lateral stereo reprojection, then runs a compute shader
 * to fill those pixels. This avoids redundant pixel shading in overlapping stereo regions.
 *
 * Pipeline:
 *   1. DispatchStencil()             - CS classifies per-pixel reprojection viability into a mode texture,
 *                                      then a fullscreen VS/PS pass writes that classification into the stencil buffer.
 *   2. (Game renders Eye 1)          - Hardware stencil test skips shading for marked pixels.
 *   3. VR::DrawStereoBlend()         - Stereo overwrite CS reprojects Eye 0 color into skipped Eye 1 pixels.
 */
struct VRStereoOptimizations
{
	bool loaded = false;

	//=============================================================================
	// ENUMS
	//=============================================================================

	/// Operating mode for stereo reprojection
	enum class StereoMode : uint32_t
	{
		Off = 0,    ///< Feature disabled
		Enable = 1  ///< Stereo reprojection enabled
	};

	/// Per-pixel classification written by StencilCS
	enum PixelMode : uint8_t
	{
		MODE_DISOCCLUDED = 0,     ///< Fully shaded, no reprojection, no blend
		MODE_EDGE = 1,            ///< Fully shaded + bilateral blend with other eye
		MODE_MAIN = 2,            ///< Eye 0: no reproject (Perf) / bilateral (Quality). Eye 1: overwrite (Perf) / bilateral (Quality)
		MODE_EDGE_NEIGHBOUR = 3,  ///< Outer band: background pixels near edge, blended in post-process
		MODE_FULL_BLEND = 4,      ///< Near-camera pixels: fully shaded in both eyes + bilateral blended
	};

	//=============================================================================
	// CONSTANTS
	//=============================================================================

	/// Sentinel written to texPomOffset when POM did not run for a pixel.
	/// -1.0 = no POM; >= 0.0 = POM ran. Matches Stereo::POM_NO_DATA in Common/VR.hlsli.
	static constexpr float kPomOffsetNoData = -1.0f;

	//=============================================================================
	// PUBLIC METHODS
	//=============================================================================

	void SetupResources();
	void Reset();
	void DrawSettings();
	void SaveSettings(json& o_json);
	void LoadSettings(json& o_json);
	void RestoreDefaultSettings();
	void ClearShaderCache();

	//=============================================================================
	// SETTINGS
	//=============================================================================

	struct Settings
	{
		StereoMode stereoMode = StereoMode::Off;
		float disocclusionDepthThreshold = 0.01f;
		float edgeDepthThreshold = 0.05f;
		float minEdgeDistance = 5000.0f;     ///< Minimum linearized depth for edge AA (game units)
		float fullBlendDistance = 0.0f;      ///< Linearized depth below which both eyes are fully shaded + blended (game units)
		float pomDepthScale = 22.5f;         ///< Scale factor for POM depth correction in stereo reprojection
		float forwardOcclusionScale = 0.1f;  ///< Eye 0 depth multiplier for directional disocclusion; 0 = disabled
		bool debugFullBlendDepth = false;    ///< Show full blend depth zone as cyan overlay
		float qualityJitterOffset = 0.125f;
		float foveatedRegionRadius = 0.3f;
		float foveatedRegionCenterX = 0.5f;
		float foveatedRegionCenterY = 0.5f;
		bool useEyeTracking = false;

		// Debug controls
		bool debugVisualization = false;
		bool debugSkipMerge = false;
		bool debugForceAllStencil = false;
		bool debugForceAllReprojectCS = false;
		bool debugDepthMap = false;
		bool debugPOMDepth = false;  ///< Show POM depth data (texPomOffset) as heatmap overlay

	} settings;

	// stereoMode is restart-gated: the stencil/CS resources are only set up
	// when `loaded` is true at boot, and toggling mid-session can't install
	// them. Latched from VR::PostPostLoad.
	inline static constexpr Util::Settings::RestartTable<Settings, 1> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, stereoMode, "VR Stereo Reprojection"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	void LatchBootSnapshot() { bootSnapshot.LatchIfNeeded(settings); }

	//=============================================================================
	// GPU CONSTANT BUFFER (must match HLSL cbuffer layout exactly)
	//=============================================================================

	struct alignas(16) VRStereoOptParams
	{
		float FrameDim[2];     // Full stereo buffer dimensions
		float RcpFrameDim[2];  // 1.0 / FrameDim

		uint32_t StereoModeValue;  // Cast of StereoMode enum (0-3)
		float DisocclusionThreshold;
		float EdgeDepthThreshold;
		uint32_t EdgeWidth;

		float QualityJitter[2];  // Sub-pixel jitter offset (Quality mode)
		float FoveatedRadius;
		float ForwardOcclusionScale;  ///< Eye 0 depth multiplier for directional disocclusion (0 = disabled)

		float FoveatedCenter[2];  // Foveal region center UV
		float MinEdgeDistance;
		float FullBlendDistance;  // Linearized depth for full blend zone
	};
	static_assert(sizeof(VRStereoOptParams) % 16 == 0, "VRStereoOptParams must be 16-byte aligned for HLSL cbuffer.");

	//=============================================================================
	// PUBLIC API
	//=============================================================================

	/**
	 * @brief Classify Eye 1 pixels and write stencil marks.
	 *
	 * Dispatches the stencil classification CS, then performs a fullscreen triangle pass
	 * to write the classification into the hardware stencil buffer.
	 * Called from Deferred::StartDeferred() after OverrideBlendStates().
	 */
	void DispatchStencil();

	/**
	 * @brief Returns true when stencil classification/write resources are ready.
	 *
	 * This mirrors DispatchStencil prerequisites except transient per-frame inputs
	 * like depth SRV availability.
	 */
	bool CanDispatchStencil() const
	{
		// Cull Eye 1 geometry only when every pass that repairs it is ready: the stencil
		// classify/write pass AND the depth-fill + G-buffer-fill passes. Without this, a
		// fill-shader compile failure would cull Eye 1 and never restore it (full corruption).
		return loaded &&
		       settings.stereoMode != StereoMode::Off &&
		       !settings.debugSkipMerge &&
		       gBufferFillSupported &&
		       stencilCS &&
		       stencilWriteVS &&
		       stencilWritePS &&
		       depthFillPS &&
		       gBufferFillCS &&
		       texPerPixelMode &&
		       paramsCB &&
		       stencilWriteDSS &&
		       stencilWriteRS &&
		       depthFillDSS;
	}

	/**
	 * @brief Creates or retrieves a modified DSS with stencil NOT_EQUAL test.
	 *
	 * Clones the given DSS with read-only stencil (WriteMask=0x00, Func=NOT_EQUAL, ref=1)
	 * so that pixels marked by our stencil write pass are skipped during normal rendering.
	 * Cached per unique input DSS pointer.
	 *
	 * @param originalDSS The original depth-stencil state to modify.
	 * @return Modified DSS with stencil test, or original if creation fails.
	 */
	ID3D11DepthStencilState* GetOrCreateModifiedDSS(ID3D11DepthStencilState* originalDSS);

	/// Whether the stencil pass is currently active this frame
	bool IsStencilActive() const { return stencilActive; }
	void NoteStencilSwap() { ++stencilSwapCount; }

	/**
	 * @brief Repair the stencil-culled Eye 1 pixels after geometry rendering.
	 *
	 * Single deferred-pipeline entry point: deactivates stencil culling, restores depth,
	 * then reprojects the G-buffer for the culled pixels so all downstream consumers
	 * (SSGI, composite, water, sky) run unmodified and light Eye 1 natively. No-op when
	 * stencil culling did not engage this frame. Must run after geometry, before any
	 * pass that reads depth or the G-buffer; the sub-steps are ordering-sensitive and
	 * encapsulated here so callers cannot interleave work between them.
	 */
	void RepairCulledEye1();

	/// Get mode texture SRV for external consumers (e.g., DeferredCompositeCS Eye 1 skip)
	ID3D11ShaderResourceView* GetModeTextureSRV() const { return texPerPixelMode ? texPerPixelMode->srv.get() : nullptr; }

	/// Get POM offset texture SRV for StereoBlendCS (reads per-pixel parallax depth offset)
	ID3D11ShaderResourceView* GetPomOffsetSRV() const { return texPomOffset ? texPomOffset->srv.get() : nullptr; }

	/// Get POM offset texture UAV for PS writes during deferred lighting (injected at u7)
	ID3D11UnorderedAccessView* GetPomOffsetUAV() const { return texPomOffset ? texPomOffset->uav.get() : nullptr; }

	/// Clear the POM offset texture to -1.0 (no-POM sentinel) at the start of each deferred frame
	void ClearPomOffsetTexture();

private:
	//=============================================================================
	// INTERNAL METHODS
	//=============================================================================

	/// Fullscreen triangle pass: reads mode texture, writes stencil ref=1 for MODE_MAIN pixels
	void ExecuteStencilWritePass();

	/// Deactivate stencil culling once geometry rendering completes (RepairCulledEye1 step 1).
	void DeactivateStencil();

	/// Fullscreen pass (stencil EQUAL ref=1) writing SV_Depth from the classification depth
	/// source, restoring depth for culled Eye 1 pixels (RepairCulledEye1 step 2).
	void ExecuteDepthFillPass();

	/// Reproject the G-buffer from Eye 0 into the culled Eye 1 pixels so downstream passes
	/// light Eye 1 natively (RepairCulledEye1 step 3).
	void DispatchGBufferFill();

	/// Compiles all shaders used by this feature
	void CompileShaders();

	/// Updates the constant buffer with current settings and frame dimensions
	void UpdateConstantBuffer();

	//=============================================================================
	// GPU RESOURCES
	//=============================================================================

	eastl::unique_ptr<ConstantBuffer> paramsCB;
	eastl::unique_ptr<Texture2D> texPerPixelMode;  ///< R8_UINT classification texture (full SBS resolution)
	eastl::unique_ptr<Texture2D> texPomOffset;     ///< R16_FLOAT POM depth offset written by Lighting PS, read by StereoBlendCS

	winrt::com_ptr<ID3D11DepthStencilState> stencilWriteDSS;
	winrt::com_ptr<ID3D11DepthStencilState> depthFillDSS;
	winrt::com_ptr<ID3D11RasterizerState> stencilWriteRS;

	winrt::com_ptr<ID3D11ComputeShader> stencilCS;
	winrt::com_ptr<ID3D11ComputeShader> gBufferFillCS;
	winrt::com_ptr<ID3D11ComputeShader> stencilDebugDepthMapCS;
	winrt::com_ptr<ID3D11VertexShader> stencilWriteVS;
	winrt::com_ptr<ID3D11PixelShader> stencilWritePS;
	winrt::com_ptr<ID3D11PixelShader> depthFillPS;

	/// Cache of original DSS -> modified DSS with stencil NOT_EQUAL enforcement
	std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> dssCache;

	bool stencilActive = false;
	uint32_t stencilSwapCount = 0;

	// GBufferFillCS does typed UAV loads on the G-buffer formats (R10G10B10A2,
	// R11G11B10, R16_UNORM, fp16); without TypedUAVLoadAdditionalFormats those reads
	// return undefined data, so the feature stays off rather than corrupt Eye 1.
	bool gBufferFillSupported = false;
};
