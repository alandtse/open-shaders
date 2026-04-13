#include "Upscaling.h"

#include "Deferred.h"
#include "FoveatedCommon.h"
#include "Hooks.h"
#include "State.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/Streamline.h"
#include "Utils/UI.h"
#include "VR.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <directx/d3dx12.h>
#include <format>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	dlssPreset,
	frameLimitMode,
	frameGenerationMode,
	frameGenerationForceEnable,
	streamlineLogLevel,
	sharpnessFSR,
	sharpnessDLSS,
	foveatedVendorDispatch,
	foveatedCenterArea,
	foveatedLeftEyeMaskOffsetX,
	foveatedLeftEyeMaskOffsetY,
	foveatedRightEyeMaskOffsetX,
	foveatedRightEyeMaskOffsetY,
	foveatedPeripheryEdgeBlur,
	foveatedPeripheryEdgeBlurStrength,
	foveatedPeripheryMaskVisualization,
	linkFoveatedCenterAreaWithSSGI,
	hasExplicitFoveatedCenterLinkPreference,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	reflexUseMarkersToOptimize,
	reflexUseFPSLimit,
	reflexFPSLimit);

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

namespace
{
	constexpr float kPeripheryEdgeBlurStrengthMin = 1.0f;
	constexpr float kPeripheryEdgeBlurStrengthMax = 10.0f;
	constexpr float kFoveatedMaskOffsetAdjustMin = -0.15f;
	constexpr float kFoveatedMaskOffsetAdjustMax = 0.15f;
	constexpr float kFoveatedMaskOffsetResolvedMin = -0.25f;
	constexpr float kFoveatedMaskOffsetResolvedMax = 0.25f;

	float ClampFoveatedCenterArea(float value)
	{
		return FoveatedCommon::ClampCenterArea(value);
	}

	float ClampFoveatedMaskOffsetAdjustment(float value)
	{
		return std::clamp(value, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax);
	}

	float ClampPeripheryEdgeBlurStrength(float value)
	{
		return std::clamp(value, kPeripheryEdgeBlurStrengthMin, kPeripheryEdgeBlurStrengthMax);
	}

	uint ClampToggleUInt(uint value)
	{
		return std::min<uint>(value, 1u);
	}

	uint ClampQualityModeUInt(uint value)
	{
		return std::min<uint>(value, 4u);
	}

	uint ClampStreamlineLogLevelUInt(uint value)
	{
		return std::min<uint>(value, 2u);
	}

	float ClampFiniteUnitRange(float value, float fallback)
	{
		if (!std::isfinite(value))
			return fallback;
		return std::clamp(value, 0.0f, 1.0f);
	}

	void SanitizeFoveatedSettings(Upscaling::Settings& settings)
	{
		settings.foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
		settings.foveatedLeftEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetX);
		settings.foveatedLeftEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedLeftEyeMaskOffsetY);
		settings.foveatedRightEyeMaskOffsetX = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetX);
		settings.foveatedRightEyeMaskOffsetY = ClampFoveatedMaskOffsetAdjustment(settings.foveatedRightEyeMaskOffsetY);
		settings.foveatedPeripheryEdgeBlurStrength = ClampPeripheryEdgeBlurStrength(settings.foveatedPeripheryEdgeBlurStrength);
	}

	void SanitizeUpscalingSettings(Upscaling::Settings& settings)
	{
		settings.upscaleMethod = std::min<uint>(settings.upscaleMethod, static_cast<uint>(Upscaling::UpscaleMethod::kDLSS));
		settings.upscaleMethodNoDLSS = std::min<uint>(settings.upscaleMethodNoDLSS, static_cast<uint>(Upscaling::UpscaleMethod::kFSR));
		settings.qualityMode = ClampQualityModeUInt(settings.qualityMode);
		settings.dlssPreset = std::min<uint>(settings.dlssPreset, Upscaling::kDLSSPresetMaxIndex);
		settings.frameLimitMode = ClampToggleUInt(settings.frameLimitMode);
		settings.frameGenerationMode = ClampToggleUInt(settings.frameGenerationMode);
		settings.frameGenerationForceEnable = ClampToggleUInt(settings.frameGenerationForceEnable);
		settings.streamlineLogLevel = ClampStreamlineLogLevelUInt(settings.streamlineLogLevel);
		settings.sharpnessFSR = ClampFiniteUnitRange(settings.sharpnessFSR, 0.0f);
		settings.sharpnessDLSS = ClampFiniteUnitRange(settings.sharpnessDLSS, 0.1f);
		SanitizeFoveatedSettings(settings);
	}

	void ResetVRSpecificUpscalingSettings(Upscaling::Settings& settings)
	{
		settings.vrPipelineDeduplication = false;
		settings.foveatedVendorDispatch = false;
		settings.foveatedCenterArea = 0.6f;
		settings.foveatedLeftEyeMaskOffsetX = 0.0f;
		settings.foveatedLeftEyeMaskOffsetY = 0.0f;
		settings.foveatedRightEyeMaskOffsetX = 0.0f;
		settings.foveatedRightEyeMaskOffsetY = 0.0f;
		settings.foveatedPeripheryEdgeBlur = false;
		settings.foveatedPeripheryEdgeBlurStrength = 1.0f;
		settings.foveatedPeripheryMaskVisualization = false;
		settings.linkFoveatedCenterAreaWithSSGI = true;
		settings.hasExplicitFoveatedCenterLinkPreference = false;
	}

	void StripVRSpecificUpscalingSettings(json& o_json)
	{
		o_json.erase("vrPipelineDeduplication");
		o_json.erase("foveatedVendorDispatch");
		o_json.erase("foveatedCenterArea");
		o_json.erase("foveatedLeftEyeMaskOffsetX");
		o_json.erase("foveatedLeftEyeMaskOffsetY");
		o_json.erase("foveatedRightEyeMaskOffsetX");
		o_json.erase("foveatedRightEyeMaskOffsetY");
		o_json.erase("foveatedPeripheryEdgeBlur");
		o_json.erase("foveatedPeripheryEdgeBlurStrength");
		o_json.erase("foveatedPeripheryMaskVisualization");
		o_json.erase("linkFoveatedCenterAreaWithSSGI");
		o_json.erase("hasExplicitFoveatedCenterLinkPreference");
	}

	bool SupportsFoveatedVendorDispatch(Upscaling::UpscaleMethod a_upscaleMethod)
	{
		// Foveated vendor dispatch is VR-only and currently DLSS-only.
		return globals::game::isVR && a_upscaleMethod == Upscaling::UpscaleMethod::kDLSS;
	}

	bool TryGetTexture2DDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& outDesc)
	{
		if (!resource)
			return false;

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;

		texture->GetDesc(&outDesc);
		return true;
	}

	bool IsNvidiaAdapterDescription(const std::string& adapterDescription)
	{
		return adapterDescription.find("NVIDIA") != std::string::npos ||
		       adapterDescription.find("Nvidia") != std::string::npos ||
		       adapterDescription.find("nvidia") != std::string::npos;
	}
}

/**
 * @brief Creates a Direct3D 11 device and swap chain, with support for advanced upscaling and frame generation features.
 *
 * This function intercepts the standard D3D11 device and swap chain creation process to enable integration with Streamline and FidelityFX technologies, as well as optional D3D12 proxying for frame generation. It adjusts swap chain flags for tearing support, manages feature checks, and conditionally routes device creation through Streamline or FidelityFX proxies based on runtime settings and hardware capabilities. If frame generation is enabled and supported, a D3D12 proxy is used; otherwise, the standard D3D11 creation path is followed.
 *
 * @return HRESULT indicating the success or failure of device and swap chain creation.
 */
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	globals::state->SetAdapterDescription(adapterDesc.Description);

	auto& upscaling = globals::features::upscaling;
	upscaling.LoadUpscalingSDKs();

	if (upscaling.IsBackendInitialized())
		upscaling.CheckBackendFeatures(pAdapter);

	// Use better swap effect to prevent tearing and improve performance
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	bool shouldProxy = !globals::game::isVR;
	if (shouldProxy)
		if (!pSwapChainDesc->Windowed)
			shouldProxy = false;

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	if (shouldProxy) {
		if (upscaling.settings.frameGenerationMode)
			if (refreshRate >= 120)
				shouldProxy = true;
			else if (upscaling.settings.frameGenerationForceEnable)
				shouldProxy = true;
			else
				shouldProxy = false;
		else
			shouldProxy = false;
	}

	upscaling.lowRefreshRate = refreshRate < 120;
	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	if (shouldProxy) {
		logger::info("[Frame Generation] Frame Generation enabled, using D3D12 proxy");

		if (upscaling.HasFrameGenModule()) {
			DX::ThrowIfFailed(D3D11CreateDevice(
				pAdapter,
				DriverType,
				Software,
				Flags,
				&featureLevel,
				1,
				SDKVersion,
				ppDevice,
				pFeatureLevel,
				ppImmediateContext));

			upscaling.SetProxyD3D11Device(*ppDevice);
			upscaling.SetProxyD3D11DeviceContext(*ppImmediateContext);
			upscaling.CreateProxySwapChain(pAdapter, *pSwapChainDesc);
			upscaling.CreateProxyInterop();

			*ppSwapChain = upscaling.GetProxySwapChain();

			upscaling.d3d12SwapChainActive = true;

			if (upscaling.IsBackendInitialized()) {
				upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
				upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
				upscaling.SetBackendD3DDevice(*ppDevice);
				// Some Streamline features (notably Reflex/PCL) may not report
				// load/support status reliably until the D3D device is bound.
				upscaling.CheckBackendFeatures(pAdapter);
				upscaling.PostBackendDevice();
			}

			return S_OK;
		} else {
			logger::warn("[Frame Generation] FidelityFX DLLs are not loaded, skipping proxy");
			upscaling.fidelityFXMissing = true;
		}
	}

	auto ret = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	if (upscaling.IsBackendInitialized()) {
		upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
		upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
		upscaling.SetBackendD3DDevice(*ppDevice);
		// Re-check after device bind to ensure feature availability is accurate.
		upscaling.CheckBackendFeatures(pAdapter);
		upscaling.PostBackendDevice();
	}

	return ret;
}

void Upscaling::DrawSettings()
{
	// Display upscaling options in the UI
	std::vector<std::string> upscaleModes = { "None", "TAA" };

	std::string fsrLabel = "AMD FSR 3.1";
	upscaleModes.push_back(fsrLabel);

	std::string dlssLabel = "NVIDIA DLSS";
	upscaleModes.push_back(dlssLabel);

	// Determine available modes
	bool featureDLSS = streamline.featureDLSS;
	bool featureFSR = true;  // FSR is always available

	uint32_t* currentUpscaleMode = &settings.upscaleMethod;
	uint32_t availableModes = 1;  // Start with TAA
	if (featureFSR)
		availableModes = 2;  // Add FSR
	if (featureDLSS)
		availableModes = 3;  // Add DLSS if available
	else
		currentUpscaleMode = &settings.upscaleMethodNoDLSS;

	// Slider for method selection
	// Clamp the index used to read from the built label vector to avoid OOB if the stored value is stale
	uint32_t modeLabelIndex = std::min(*currentUpscaleMode, static_cast<uint32_t>(upscaleModes.size() - 1));
	std::string currentLabel = upscaleModes[modeLabelIndex];
	ImGui::SliderInt("Method", (int*)currentUpscaleMode, 0, availableModes, currentLabel.c_str());

	*currentUpscaleMode = std::min(availableModes, *currentUpscaleMode);

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	// Display warning for DLSS resolution limits (non-VR only; VR handles this automatically)
	if (!globals::game::isVR && upscaleMethod == UpscaleMethod::kDLSS) {
		auto screenSize = globals::state->screenSize;
		if (screenSize.x > streamline.MAX_RESOLUTION || screenSize.y > streamline.MAX_RESOLUTION) {
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
			ImGui::Text("Warning: Requested resolution %.0f x %.0f exceeds maximum supported resolution %d x %d for DLSS.",
				screenSize.x, screenSize.y, streamline.MAX_RESOLUTION, streamline.MAX_RESOLUTION);
			ImGui::Text("DLSS will not function. Lower your resolution or select a different upscaling method.");
			ImGui::PopStyleColor();
		}
	}

	// Display upscaling settings if applicable
	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		const char* upscalePresetsDLSS[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "DLAA" };
		const char* upscalePresets[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Native AA" };

		// Compute a safe preset index (4 - qualityMode) clamped to [0,4] to avoid negative/overflow indexing
		int presetIndex = 0;
		if (settings.qualityMode <= 4)
			presetIndex = 4 - static_cast<int>(settings.qualityMode);
		presetIndex = std::clamp(presetIndex, 0, 4);

		// Choose preset name set and the corresponding scales once, then show a
		// single SliderInt to avoid duplicated calls.
		const char* baseLabel = nullptr;

		if (upscaleMethod == UpscaleMethod::kFSR) {
			baseLabel = upscalePresets[presetIndex];
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			baseLabel = upscalePresetsDLSS[presetIndex];
		}

		if (baseLabel) {
			// Format the label with preset name and resolution scale
			std::string labelWithScale = std::format("{} ( {:.2f}x )", baseLabel, (resolutionScale.x + resolutionScale.y) * 0.5f);

			ImGui::SliderInt("Upscale Preset", (int*)&settings.qualityMode, 0, 4, labelWithScale.c_str());
		}

		if (upscaleMethod == UpscaleMethod::kFSR) {
			ImGui::SliderFloat("Sharpness", &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			// Keep persisted preset values stable (0=J,1=K,2=L,3=M,4=F) while
			// presenting an alphabetical selection list in the UI.
			const uint32_t dlssProfileOrder[] = { 4u, 0u, 1u, 2u, 3u };  // F, J, K, L, M
			const char* dlssProfiles[] = { "F", "J", "K", "L", "M" };
			settings.dlssPreset = std::min(settings.dlssPreset, kDLSSPresetMaxIndex);

			int dlssProfileUiIndex = 0;
			for (int i = 0; i < IM_ARRAYSIZE(dlssProfileOrder); ++i) {
				if (dlssProfileOrder[i] == settings.dlssPreset) {
					dlssProfileUiIndex = i;
					break;
				}
			}

			ImGui::SliderInt("DLSS Profile", &dlssProfileUiIndex, 0, static_cast<int>(kDLSSPresetMaxIndex), dlssProfiles[dlssProfileUiIndex]);
			dlssProfileUiIndex = std::clamp(dlssProfileUiIndex, 0, static_cast<int>(kDLSSPresetMaxIndex));
			settings.dlssPreset = dlssProfileOrder[dlssProfileUiIndex];

			if (auto _tt = Util::HoverTooltipWrapper()) {
				switch (settings.dlssPreset) {
				case 0:
					ImGui::Text("DLAA/Quality/Balanced preset. Slightly less ghosting than K, but more flicker. Speed: ~K. Use only if K ghosts.");
					break;
				case 1:
					ImGui::Text("Default for DLAA/Quality/Balanced. Best all-round stability and image quality. Speed: fast. Recommended for most users.");
					break;
				case 2:
					ImGui::Text("For quality/stability, lowest ghosting. Slowest preset. Best for Ultra Performance (esp. on 4K). Not recommended on pre-RTX 40 cards.");
					break;
				case 3:
					ImGui::Text("Near-L image quality with speed closer to K. Best for Performance mode. Not recommended on pre-RTX 40 cards.");
					break;
				case 4:
					ImGui::Text("Intended for Ultra Performance/DLAA. Default preset for Ultra Performance.");
					ImGui::Text("Use when chasing max FPS; outside Ultra Performance/DLAA, K is usually more stable.");
					break;
				default:
					ImGui::Text("Default for DLAA/Quality/Balanced. Best all-round stability and image quality. Speed: fast. Recommended for most users.");
					break;
				}
			}

			ImGui::SliderFloat("Sharpness", &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");

			const auto& adapter = globals::state->adapterDescription;
			const bool isNvidia = IsNvidiaAdapterDescription(adapter);
			if (isNvidia) {
				ImGui::TextWrapped("Note: Use presets L/M only on RTX 40/50. Even on those GPUs, J/K/F usually provide better quality/performance. On RTX 20/30, use K (default) or J/F for better FPS.");
			}
		}

		if (globals::game::isVR) {
			const bool foveatedDispatchSupportedForMethod = SupportsFoveatedVendorDispatch(upscaleMethod);
			if (!foveatedDispatchSupportedForMethod) {
				ImGui::TextDisabled("Foveated Upscaling is currently DLSS-only. FSR uses full-eye dispatch.");
			}
			ImGui::BeginDisabled(!foveatedDispatchSupportedForMethod);
			{
				Util::BlueFrameStyleWrapper foveatedStyle(true);
				ImGui::Checkbox("Foveated Upscaling", &settings.foveatedVendorDispatch);
			}
			SanitizeFoveatedSettings(settings);
			if (settings.foveatedVendorDispatch && foveatedDispatchSupportedForMethod) {
				{
					Util::BlueFrameStyleWrapper foveatedAreaStyle;
					ImGui::SliderFloat("Foveated Area", &settings.foveatedCenterArea, FoveatedCommon::kCenterAreaMin, FoveatedCommon::kCenterAreaMax, "%.2f");
				}
				settings.foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Major performance option. Controls how much of the view uses the vendor upscaler directly.");
					ImGui::TextUnformatted("Runs vendor upscaling only in the center region and fills periphery with the periphery pass.");
					ImGui::TextUnformatted("1.00 means full-center coverage (equivalent to full-frame vendor dispatch).");
					if (settings.linkFoveatedCenterAreaWithSSGI)
						ImGui::TextUnformatted("Shared with SSGI only while an SSGI foveated mode is active.");
				}

				ImGui::Separator();
				{
					Util::YellowFrameStyleWrapper maskStyle(true);
					ImGui::Checkbox("Foveated Mask Visualization", &settings.foveatedPeripheryMaskVisualization);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Shows foveated and periphery regions for each eye in Upscaling and SSGI foveated modes.");
					ImGui::TextUnformatted("X/Y sliders move the real per-eye masks, not just the debug view.");
					ImGui::TextUnformatted("Lower Foveated Area until the full ellipse is visible in each eye.");
					ImGui::TextUnformatted("Center each mask with the sliders, then raise Foveated Area until the periphery reaches the view edge.");
					ImGui::TextUnformatted("X and Y offsets do not need to match between eyes.");
				}

				if (settings.foveatedPeripheryMaskVisualization) {
					const auto defaultOffsets = GetDefaultFoveatedMaskCenterOffsets();
					const auto resolvedOffsets = GetResolvedFoveatedMaskCenterOffsets();
					const std::array<const char*, 2> eyeLabels{ "Left", "Right" };
					const std::array<std::array<float*, 2>, 2> adjustmentSettings{ {
						{ &settings.foveatedLeftEyeMaskOffsetX, &settings.foveatedLeftEyeMaskOffsetY },
						{ &settings.foveatedRightEyeMaskOffsetX, &settings.foveatedRightEyeMaskOffsetY },
					} };
					const std::array<const char*, 2> axisLabels{ "X", "Y" };

					auto drawMaskAlignmentSlider = [&](size_t eyeIndex, size_t axisIndex) {
						const bool isHorizontal = axisIndex == 0;
						float* adjustment = adjustmentSettings[eyeIndex][axisIndex];
						const float defaultCenter = isHorizontal ? defaultOffsets[eyeIndex].x : defaultOffsets[eyeIndex].y;
						const float resolvedCenter = isHorizontal ? resolvedOffsets[eyeIndex].x : resolvedOffsets[eyeIndex].y;
						const std::string sliderLabel = std::format("{} Eye Offset {}", eyeLabels[eyeIndex], axisLabels[axisIndex]);
						ImGui::SliderFloat(sliderLabel.c_str(), adjustment, kFoveatedMaskOffsetAdjustMin, kFoveatedMaskOffsetAdjustMax, "%.3f");
						*adjustment = ClampFoveatedMaskOffsetAdjustment(*adjustment);
						if (auto _tt = Util::HoverTooltipWrapper()) {
							if (eyeIndex == 0) {
								ImGui::TextUnformatted(isHorizontal ?
									                       "Close right eye to determine horizontal offset to the centered left-eye mask." :
									                       "Close right eye to determine vertical offset to the centered left-eye mask.");
								ImGui::TextUnformatted(isHorizontal ?
									                       "Positive moves the foveated mask right. Negative moves it left." :
									                       "Positive moves the foveated mask down. Negative moves it up.");
								ImGui::Text("Default center %s: %.3f, resolved center %s: %.3f", axisLabels[axisIndex], defaultCenter, axisLabels[axisIndex], resolvedCenter);
							} else {
								ImGui::TextUnformatted(isHorizontal ?
									                       "Close left eye to determine horizontal offset to the centered right-eye mask." :
									                       "Close left eye to determine vertical offset to the centered right-eye mask.");
								ImGui::TextUnformatted(isHorizontal ?
									                       "Positive moves the foveated mask right. Negative moves it left." :
									                       "Positive moves the foveated mask down. Negative moves it up.");
								ImGui::Text("Default center %s: %.3f, resolved center %s: %.3f", axisLabels[axisIndex], defaultCenter, axisLabels[axisIndex], resolvedCenter);
							}
						}
					};

					ImGui::Separator();
					ImGui::TextUnformatted("Mask Alignment");
					{
						Util::YellowFrameStyleWrapper maskSliderStyle;
						drawMaskAlignmentSlider(0, 0);
						drawMaskAlignmentSlider(0, 1);
						drawMaskAlignmentSlider(1, 0);
						drawMaskAlignmentSlider(1, 1);
					}

					ImGui::TextDisabled("Mask alignment notes");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted("Use the mask view to line the center ellipse up with the real headset image in each eye horizontally and vertically.");
						ImGui::TextUnformatted("These offsets also drive the actual foveated crop and blend placement.");
						ImGui::TextUnformatted("When SSGI foveation is active, the same eye offsets are reused there.");
					}
				}

				ImGui::Separator();
				if (ImGui::Checkbox("Link Foveated Area With SSGI", &settings.linkFoveatedCenterAreaWithSSGI))
					settings.hasExplicitFoveatedCenterLinkPreference = true;
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Enabled: one shared center-area value drives both SSGI foveation and Upscaling foveation.");
					ImGui::TextUnformatted("Disable to tune only the area size independently, or to leave SSGI unfoveated while Upscaling stays foveated.");
					ImGui::TextUnformatted("Mask placement always stays shared and is defined by Upscaling.");
					ImGui::TextUnformatted("If SSGI is in AO only or another non-foveated mode, this shared area is ignored there.");
				}

				ImGui::Separator();
				ImGui::Checkbox("Edge Blur", &settings.foveatedPeripheryEdgeBlur);
				if (settings.foveatedPeripheryEdgeBlur) {
					ImGui::SliderFloat("Edge Blur Strength", &settings.foveatedPeripheryEdgeBlurStrength, kPeripheryEdgeBlurStrengthMin, kPeripheryEdgeBlurStrengthMax, "%.2f");
					settings.foveatedPeripheryEdgeBlurStrength = ClampPeripheryEdgeBlurStrength(settings.foveatedPeripheryEdgeBlurStrength);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Applies a lightweight edge-aware blur in periphery only at a low performance cost.");
					ImGui::TextUnformatted("Stronger values reduce flicker but soften peripheral detail.");
				}
			}
			ImGui::EndDisabled();
		}
	}

	const bool frameGenerationDx12PathActive = IsFrameGenerationDx12PathActive();

	if (!globals::game::isVR) {
		if (ImGui::TreeNodeEx("Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Frame Generation interpolates real frames with generated ones for a smoother experience");
			ImGui::Text("Uses AMD FSR Frame Generation technology");
			if (HasFrameGenModule())
				ImGui::Text("AMD FSR Frame Generation is available.");
			ImGui::Text("Requires a D3D11 to D3D12 proxy which can create compatibility issues");
			ImGui::Text("Toggling this setting requires a restart to work correctly");

			bool onlyRequiresRestart = true;

			if (!isWindowed) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires windowed mode");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (lowRefreshRate && !settings.frameGenerationForceEnable) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires a high refresh rate monitor or Force Enable Frame Generation");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (fidelityFXMissing) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: FidelityFX DLLs are not loaded");
				ImGui::PopStyleColor();

				onlyRequiresRestart = false;
			}

			if (onlyRequiresRestart && settings.frameGenerationMode && !frameGenerationDx12PathActive) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires restart");
				ImGui::PopStyleColor();
			}

			if (!settings.frameGenerationMode && frameGenerationDx12PathActive) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires restart");
				ImGui::PopStyleColor();
			}

			std::string enabledLabel = "Enabled";
			const char* toggleModes[] = { "Disabled", "Enabled" };
			const char* toggleModesFG[] = { "Disabled", enabledLabel.c_str() };

			ImGui::SliderInt("Frame Generation", (int*)&settings.frameGenerationMode, 0, 1, toggleModesFG[settings.frameGenerationMode]);

			if (!frameGenerationDx12PathActive)
				ImGui::BeginDisabled();

			ImGui::SliderInt("Frame Limit (Variable Refresh Rate)", (int*)&settings.frameLimitMode, 0, 1, std::format("{}", toggleModes[settings.frameLimitMode]).c_str());

			if (!frameGenerationDx12PathActive)
				ImGui::EndDisabled();

			ImGui::Text("Allows frame generation to function on low refresh rate monitors");
			ImGui::SliderInt("Force Enable Frame Generation", (int*)&settings.frameGenerationForceEnable, 0, 1, std::format("{}", toggleModes[settings.frameGenerationForceEnable]).c_str());

			ImGui::TreePop();
		}
	}

	if (streamline.reflexSupportedOnCurrentAdapter && ImGui::TreeNodeEx("NVIDIA Reflex", ImGuiTreeNodeFlags_DefaultOpen)) {
		const bool reflexAvailable = streamline.initialized && streamline.featureReflex;
		const bool markerOptimizationAvailable = reflexAvailable && streamline.featurePCL;
		const bool reflexBlockedByFrameGeneration = IsFrameGenerationDx12PathActive();
		const char* toggleModes[] = { "Disabled", "Enabled" };

		if (!reflexAvailable) {
			ImGui::TextDisabled("Reflex is not available. Ensure sl.reflex.dll is present and restart.");
		}

		if (reflexBlockedByFrameGeneration) {
			ImGui::TextDisabled("Reflex is disabled while Frame Generation is active on the DX12 swap chain.");
		}

		if (!reflexAvailable || reflexBlockedByFrameGeneration)
			ImGui::BeginDisabled();

		int lowLatencyMode = settings.reflexLowLatencyMode ? 1 : 0;
		ImGui::SliderInt("Low Latency Mode", &lowLatencyMode, 0, 1, toggleModes[lowLatencyMode]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Cuts input delay by syncing CPU work closer to the GPU.");
			ImGui::TextUnformatted("May reduce max FPS a little, but usually feels much more responsive.");
		}
		settings.reflexLowLatencyMode = lowLatencyMode > 0;

		if (!settings.reflexLowLatencyMode)
			ImGui::BeginDisabled();

		int lowLatencyBoost = settings.reflexLowLatencyBoost ? 1 : 0;
		ImGui::SliderInt("Low Latency Boost", &lowLatencyBoost, 0, 1, toggleModes[lowLatencyBoost]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Keeps GPU clocks higher to avoid latency spikes at low GPU load.");
			ImGui::TextUnformatted("Useful if frametime jumps and responsiveness feels inconsistent.");
			ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Increases power draw and heat, so leave Off unless needed.");
		}
		settings.reflexLowLatencyBoost = lowLatencyBoost > 0;

		if (!settings.reflexLowLatencyMode)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable)
			ImGui::BeginDisabled();

		int markersToOptimize = settings.reflexUseMarkersToOptimize ? 1 : 0;
		ImGui::SliderInt("Use Markers To Optimize", &markersToOptimize, 0, 1, toggleModes[markersToOptimize]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Uses frame markers for tighter Reflex timing.");
			ImGui::TextUnformatted("Try On first; turn Off if it causes stutter on your setup.");
		}
		settings.reflexUseMarkersToOptimize = markersToOptimize > 0;

		if (!markerOptimizationAvailable)
			ImGui::EndDisabled();

		if (!markerOptimizationAvailable) {
			ImGui::TextDisabled("Marker optimization unavailable (PCL not loaded).");
		}

		int useFPSLimit = settings.reflexUseFPSLimit ? 1 : 0;
		ImGui::SliderInt("Use FPS Limit", &useFPSLimit, 0, 1, toggleModes[useFPSLimit]);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Uses Reflex's internal FPS cap for steadier frametimes.");
			ImGui::TextUnformatted("Can lower latency versus uncapped rendering.");
		}
		settings.reflexUseFPSLimit = useFPSLimit > 0;

		if (!settings.reflexUseFPSLimit)
			ImGui::BeginDisabled();

		if (!std::isfinite(settings.reflexFPSLimit))
			settings.reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
		ImGui::SliderFloat("FPS Limit", &settings.reflexFPSLimit, 20.0f, 240.0f, "%.0f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Set your frame cap target.");
			ImGui::TextUnformatted("Start about 2-3 FPS below refresh rate (e.g. 117 for 120 Hz).");
		}

		if (!settings.reflexUseFPSLimit)
			ImGui::EndDisabled();

		if (!reflexAvailable || reflexBlockedByFrameGeneration)
			ImGui::EndDisabled();

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Backend Diagnostics")) {
		// Streamline log level selection
		const char* logLevels[] = { "Off", "Default", "Verbose" };
		int logLevelIdx = static_cast<int>(settings.streamlineLogLevel);
		if (ImGui::Combo("Streamline Logging", &logLevelIdx, logLevels, IM_ARRAYSIZE(logLevels))) {
			settings.streamlineLogLevel = static_cast<uint>(logLevelIdx);
		}
		ImGui::TextUnformatted("Changing this requires a restart to take effect.");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Streamline logging controls the verbosity of NVIDIA Streamline backend logs. Useful for debugging issues with DLSS/DLSS-G.");
		}

		// VR Debug visualization -- per-eye buffers and native inputs
		if (globals::game::isVR) {
			ImGui::Separator();
			static float debugRescale = 0.15f;
			ImGui::SliderFloat("View Resize", &debugRescale, 0.05f, 1.f);

			if (ImGui::TreeNode("Upscaling Intermediates")) {
				if (vrIntermediateColorIn[0] && vrIntermediateColorOut[0]) {
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[0], "Left Eye In", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorIn[1], "Right Eye In", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[0], "Left Eye Out", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateColorOut[1], "Right Eye Out", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[0], "Left Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateMotionVectors[1], "Right Eye MVec", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[0], "Left Eye Reactive", debugRescale)
					BUFFER_VIEWER_NODE_TITLE(vrIntermediateReactiveMask[1], "Right Eye Reactive", debugRescale)
				} else {
					ImGui::TextDisabled("VR intermediates not yet created (enter game world)");
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNode("Native Inputs")) {
				auto renderer = globals::game::renderer;
				auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
				auto& mvec = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
				auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

				auto DisplayRT = [&](const char* label, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srv) {
					if (srv && tex) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						char buf[128];
						snprintf(buf, sizeof(buf), "%s (%ux%u)", label, desc.Width, desc.Height);
						if (ImGui::TreeNode(buf)) {
							ImGui::Image(srv, { desc.Width * debugRescale, desc.Height * debugRescale });
							ImGui::TreePop();
						}
					}
				};

				DisplayRT("kMAIN (Color Input)", (ID3D11Texture2D*)main.texture, (ID3D11ShaderResourceView*)main.SRV);
				DisplayRT("Motion Vectors", (ID3D11Texture2D*)mvec.texture, (ID3D11ShaderResourceView*)mvec.SRV);
				DisplayRT("Depth", depth.texture, depth.depthSRV);

				if (reactiveMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(reactiveMaskTexture, "Reactive Mask", debugRescale)
				if (transparencyCompositionMaskTexture)
					BUFFER_VIEWER_NODE_TITLE(transparencyCompositionMaskTexture, "Transparency Mask", debugRescale)

				ImGui::TreePop();
			}
		}

		ImGui::Separator();
		// FidelityFX section
		if (ImGui::Selectable("AMD FidelityFX DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", FidelityFX::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::string> headers = { "DLL Name", "Version" };
		std::vector<std::vector<std::string>> ffRows;
		for (const auto& [name, dllVersion] : FidelityFX::dllVersions)
			ffRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> ffSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"ffx_dll_versions",
			headers,
			ffRows,
			0,
			true,
			ffSorters);

		// Streamline section
		if (ImGui::Selectable("NVIDIA Streamline DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", Streamline::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::vector<std::string>> slRows;
		for (const auto& [name, dllVersion] : Streamline::dllVersions)
			slRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> slSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"sl_dll_versions",
			headers,
			slRows,
			0,
			true,
			slSorters);
		ImGui::TreePop();
	}
}

void Upscaling::SaveSettings(json& o_json)
{
	SanitizeUpscalingSettings(settings);
	o_json = settings;
	if (!REL::Module::IsVR()) {
		StripVRSpecificUpscalingSettings(o_json);
	}
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		if (auto setting = iniSettingCollection->GetSetting("bUseTAA:Display"))
			iniSettingCollection->WriteSetting(setting);
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	settings = o_json;
	if (!REL::Module::IsVR()) {
		ResetVRSpecificUpscalingSettings(settings);
	}
	if (!settings.hasExplicitFoveatedCenterLinkPreference)
		settings.linkFoveatedCenterAreaWithSSGI = true;

	if (settings.upscaleMethod > static_cast<uint>(UpscaleMethod::kDLSS)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod, static_cast<uint>(UpscaleMethod::kDLSS));
	}
	if (settings.upscaleMethodNoDLSS > static_cast<uint>(UpscaleMethod::kFSR)) {
		logger::warn("[Upscaling] Loaded upscaleMethodNoDLSS {} out of range, clamping to {}", settings.upscaleMethodNoDLSS, static_cast<uint>(UpscaleMethod::kFSR));
	}
	SanitizeUpscalingSettings(settings);
	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	if (!std::isfinite(settings.reflexFPSLimit)) {
		settings.reflexFPSLimit = 60.0f;
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} is not finite, resetting to {}",
			originalReflexFPSLimit,
			settings.reflexFPSLimit);
	}
	const float clampedReflexFPSLimit = std::clamp(settings.reflexFPSLimit, 20.0f, 240.0f);
	if (clampedReflexFPSLimit != settings.reflexFPSLimit) {
		logger::warn(
			"[Upscaling] Loaded reflexFPSLimit {} out of range, clamping to {}",
			settings.reflexFPSLimit,
			clampedReflexFPSLimit);
	}
	settings.reflexFPSLimit = clampedReflexFPSLimit;
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		if (auto setting = iniSettingCollection->GetSetting("bUseTAA:Display"))
			iniSettingCollection->ReadSetting(setting);
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
	SanitizeUpscalingSettings(settings);
}

void Upscaling::DataLoaded()
{
	// Fix screenshots fix from Engine Fixes
	RE::GetINISetting("bUseTAA:Display")->data.b = false;

	// The game defaults this to a non-zero value
	static auto fDRClampOffset = RE::GetINISetting("fDRClampOffset:Display");
	fDRClampOffset->data.f = 0.0f;
}

void Upscaling::Load()
{
	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off safely
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};
void Upscaling::PostPostLoad()
{
	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates resolution and jitter
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, isGOG ? 0x133 : 0xE2, 0x104));

	// Disables the original dynamic resolution system
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D, 0x25), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling in between volumetric lighting and post processing
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7, 0x206));

	// Patches RSSetScissorRect calls to use dynamic resolution
	// This is a PC-specific function hence it was missing
	if (!globals::game::isVR)
		stl::detour_thunk<SetScissorRect>(REL::RelocationID(75564, 77365));

	// Patches facegen texture generation to not use dynamic resolution
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(REL::RelocationID(26455, 27041));

	// Patches precipitation camera to not use dynamic resolution
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	// Forces FXAA off
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

	logger::info("[Upscaling] Installed hooks");
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod() const
{
	if (streamline.featureDLSS)
		return (UpscaleMethod)settings.upscaleMethod;
	return (UpscaleMethod)settings.upscaleMethodNoDLSS;
}

void Upscaling::CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Creating texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		if (!reactiveMaskTexture) {
			reactiveMaskTexture = new Texture2D(texDesc);
			reactiveMaskTexture->CreateSRV(srvDesc);
			reactiveMaskTexture->CreateUAV(uavDesc);
		}

		if (!transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture = new Texture2D(texDesc);
			transparencyCompositionMaskTexture->CreateSRV(srvDesc);
			transparencyCompositionMaskTexture->CreateUAV(uavDesc);
		}
	}

	// Motion vector copy texture is used by DLSS and FSR encode pass.
	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		if (!motionVectorCopyTexture) {
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			D3D11_TEXTURE2D_DESC motionTexDesc{};
			motionVector.texture->GetDesc(&motionTexDesc);

			texDesc.Format = motionTexDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			motionVectorCopyTexture = new Texture2D(motionTexDesc);
			motionVectorCopyTexture->CreateSRV(srvDesc);
			motionVectorCopyTexture->CreateUAV(uavDesc);
		}

	}

	// RCAS sharpener texture - matches kMAIN format for HDR sharpening
	if (a_upscalemethod == UpscaleMethod::kDLSS) {
		if (!sharpenerTexture) {
			main.texture->GetDesc(&texDesc);
			main.SRV->GetDesc(&srvDesc);

			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			sharpenerTexture = new Texture2D(texDesc);
			sharpenerTexture->CreateSRV(srvDesc);
			sharpenerTexture->CreateUAV(uavDesc);
		}
	}
}

void Upscaling::DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Destroying texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	// Clean up D3D11 textures that are no longer needed
	// Only destroy textures when switching away from methods that use them
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (reactiveMaskTexture) {
			reactiveMaskTexture->srv = nullptr;
			reactiveMaskTexture->uav = nullptr;
			reactiveMaskTexture->resource = nullptr;

			delete reactiveMaskTexture;
			reactiveMaskTexture = nullptr;
		}

		if (transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture->srv = nullptr;
			transparencyCompositionMaskTexture->uav = nullptr;
			transparencyCompositionMaskTexture->resource = nullptr;

			delete transparencyCompositionMaskTexture;
			transparencyCompositionMaskTexture = nullptr;
		}
	}

	// Motion vector copy texture is used by DLSS/FSR - destroy when switching away from both.
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		if (motionVectorCopyTexture) {
			motionVectorCopyTexture->srv = nullptr;
			motionVectorCopyTexture->uav = nullptr;
			motionVectorCopyTexture->resource = nullptr;

			delete motionVectorCopyTexture;
			motionVectorCopyTexture = nullptr;
		}
	}

	// RCAS sharpener texture is only needed for DLSS.
	if (a_upscalemethod != UpscaleMethod::kDLSS) {
		if (sharpenerTexture) {
			sharpenerTexture->srv = nullptr;
			sharpenerTexture->uav = nullptr;
			sharpenerTexture->resource = nullptr;

			delete sharpenerTexture;
			sharpenerTexture = nullptr;
		}
	}
}

void Upscaling::CheckResources(UpscaleMethod a_upscalemethod)
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	static bool previousFrameGenMode = false;
	static bool previousFoveatedDispatch = false;
	static uint32_t previousFoveatedCenterAreaMilli = static_cast<uint32_t>(std::round(ClampFoveatedCenterArea(settings.foveatedCenterArea) * 1000.0f));

	bool frameGenModeCurrent = (settings.frameGenerationMode && d3d12SwapChainActive);
	bool frameGenModeChanged = frameGenModeCurrent != previousFrameGenMode;
	bool upscaleModeChanged = (previousUpscaleMode != a_upscalemethod);
	const bool foveatedDispatchCurrent = IsFoveatedVendorDispatchEnabled(a_upscalemethod);
	const uint32_t foveatedCenterAreaMilli = static_cast<uint32_t>(std::round(ClampFoveatedCenterArea(settings.foveatedCenterArea) * 1000.0f));
	const bool compareFoveatedArea = foveatedDispatchCurrent || previousFoveatedDispatch;
	const bool foveatedDispatchChanged = previousFoveatedDispatch != foveatedDispatchCurrent ||
	                                     (compareFoveatedArea && previousFoveatedCenterAreaMilli != foveatedCenterAreaMilli);

	if (upscaleModeChanged || frameGenModeChanged || foveatedDispatchChanged) {
		logger::debug("[Upscaling] Resource change detected - Upscale: {} ({}) -> {} ({}), FrameGen: {} -> {} (d3d12Active={})",
			static_cast<int>(previousUpscaleMode), magic_enum::enum_name(previousUpscaleMode), static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), previousFrameGenMode, frameGenModeCurrent, d3d12SwapChainActive);

		// Destroy previous upscaling method resources (only if they were actually active)
		if (upscaleModeChanged) {
			DestroyUpscalingTextureResources(a_upscalemethod);

			// Only destroy SDK resources if the previous method was actually performing upscaling
			if (previousUpscalingWasActive) {
				if (previousUpscaleMode == UpscaleMethod::kDLSS)
					streamline.DestroyDLSSResources();
				else if (previousUpscaleMode == UpscaleMethod::kFSR)
					fidelityFX.DestroyFSRResources();

				if (globals::game::isVR) {
					for (int i = 0; i < 2; i++) {
						vrIntermediateColorIn[i].reset();
						vrIntermediateColorOut[i].reset();
						vrIntermediateDepth[i].reset();
						vrIntermediateMotionVectors[i].reset();
						vrIntermediateReactiveMask[i].reset();
						vrIntermediateTransparencyMask[i].reset();
					}
				}
			}
			if (a_upscalemethod == UpscaleMethod::kFSR)
				fidelityFX.CreateFSRResources();
		}

		// Create new upscaling method resources
		if (upscaleModeChanged) {
			CreateUpscalingTextureResources(a_upscalemethod);
		}

		if (upscaleModeChanged || foveatedDispatchChanged) {
			if (!foveatedDispatchCurrent)
				DestroyFoveatedResources();
		}

		// Update tracking for next call
		previousUpscaleMode = a_upscalemethod;
		previousFrameGenMode = (settings.frameGenerationMode && d3d12SwapChainActive);
		previousFoveatedDispatch = foveatedDispatchCurrent;
		previousFoveatedCenterAreaMilli = foveatedCenterAreaMilli;
		previousUpscalingWasActive = IsUpscalingActive();
	}
}

ID3D11ComputeShader* Upscaling::GetEncodeTexturesCS()
{
	auto upscaleMethod = GetUpscaleMethod();
	uint methodIndex = (uint)upscaleMethod;

	if (!encodeTexturesCS[methodIndex]) {
		logger::debug("Compiling EncodeTexturesCS.hlsl for upscale method {}", methodIndex);

		std::vector<std::pair<const char*, const char*>> defines;

		// Add upscale method define
		switch (upscaleMethod) {
		case UpscaleMethod::kDLSS:
			defines.push_back({ "DLSS", "" });
			break;
		case UpscaleMethod::kFSR:
			defines.push_back({ "FSR", "" });
			break;
		default:
			// No define for NONE or TAA
			break;
		}

		encodeTexturesCS[methodIndex].attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
	}
	return encodeTexturesCS[methodIndex].get();
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	if (!depthRefractionUpscalePS) {
		logger::debug("Compiling DepthRefractionUpscalePS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		depthRefractionUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return depthRefractionUpscalePS.get();
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	if (!underwaterMaskUpscalePS) {
		logger::debug("Compiling UnderwaterMaskPS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		underwaterMaskUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return underwaterMaskUpscalePS.get();
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	if (!upscaleVS) {
		logger::debug("Compiling UpscaleVS.hlsl");
		upscaleVS.attach((ID3D11VertexShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}

	return upscaleVS.get();
}

ID3D11ComputeShader* Upscaling::GetFoveatedPeripheryCS()
{
	if (!foveatedPeripheryCS) {
		logger::debug("Compiling FoveatedPeripheryCS.hlsl");
		foveatedPeripheryCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/FoveatedPeripheryCS.hlsl", {}, "cs_5_0"));
	}

	return foveatedPeripheryCS.get();
}

ID3D11ComputeShader* Upscaling::GetFoveatedCenterBlendCS()
{
	if (!foveatedCenterBlendCS) {
		logger::debug("Compiling FoveatedCenterBlendCS.hlsl");
		foveatedCenterBlendCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/FoveatedCenterBlendCS.hlsl", {}, "cs_5_0"));
	}

	return foveatedCenterBlendCS.get();
}

eastl::unique_ptr<Texture2D> Upscaling::CreateTextureFromSource(ID3D11Resource* src, uint32_t width, uint32_t height,
	bool copyBindFlags, bool createSRV, bool createUAV, const char* name)
{
	D3D11_TEXTURE2D_DESC srcDesc;
	static_cast<ID3D11Texture2D*>(src)->GetDesc(&srcDesc);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = srcDesc.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = copyBindFlags ? srcDesc.BindFlags : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

	auto tex = eastl::make_unique<Texture2D>(desc);

	if (name) {
		Util::SetResourceName(tex->resource.get(), name);
	}

	if (createSRV) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = srcDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		tex->CreateSRV(srvDesc);
	}
	if (createUAV) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = srcDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		tex->CreateUAV(uavDesc);
	}
	return tex;
}

bool Upscaling::IsFoveatedVendorDispatchEnabled(UpscaleMethod a_upscaleMethod) const
{
	if (!SupportsFoveatedVendorDispatch(a_upscaleMethod))
		return false;

	if (!settings.foveatedVendorDispatch)
		return false;

	const float centerArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
	// 1.0 is effectively full-frame vendor dispatch, so keep the default path.
	return centerArea < 0.999f;
}

float2 Upscaling::GetDefaultFoveatedMaskCenterOffset(uint32_t eyeIndex) const
{
	(void)eyeIndex;
	return { 0.0f, 0.0f };
}

std::array<float2, 2> Upscaling::GetDefaultFoveatedMaskCenterOffsets() const
{
	return { GetDefaultFoveatedMaskCenterOffset(0), GetDefaultFoveatedMaskCenterOffset(1) };
}

float2 Upscaling::GetResolvedFoveatedMaskCenterOffset(uint32_t eyeIndex) const
{
	float2 resolved = GetDefaultFoveatedMaskCenterOffset(eyeIndex);
	const bool isLeftEye = eyeIndex == 0;
	const float userAdjustX = isLeftEye ? settings.foveatedLeftEyeMaskOffsetX : settings.foveatedRightEyeMaskOffsetX;
	const float userAdjustY = isLeftEye ? settings.foveatedLeftEyeMaskOffsetY : settings.foveatedRightEyeMaskOffsetY;
	resolved.x = std::clamp(resolved.x + ClampFoveatedMaskOffsetAdjustment(userAdjustX), kFoveatedMaskOffsetResolvedMin, kFoveatedMaskOffsetResolvedMax);
	resolved.y = std::clamp(resolved.y + ClampFoveatedMaskOffsetAdjustment(userAdjustY), kFoveatedMaskOffsetResolvedMin, kFoveatedMaskOffsetResolvedMax);
	return resolved;
}

std::array<float2, 2> Upscaling::GetResolvedFoveatedMaskCenterOffsets() const
{
	return { GetResolvedFoveatedMaskCenterOffset(0), GetResolvedFoveatedMaskCenterOffset(1) };
}

bool Upscaling::BuildFoveatedDispatchRects(uint32_t inputWidthPerEye, uint32_t inputHeight, uint32_t outputWidthPerEye, uint32_t outputHeight, bool isVR, float centerScale)
{
	centerScale = ClampFoveatedCenterArea(centerScale);

	auto& cache = foveatedRectCache;
	auto centerOffsets = GetResolvedFoveatedMaskCenterOffsets();
	if (!isVR)
		centerOffsets[1] = { 0.0f, 0.0f };
	const bool cacheDirty =
		cache.inputWidthPerEye != inputWidthPerEye ||
		cache.inputHeight != inputHeight ||
		cache.outputWidthPerEye != outputWidthPerEye ||
		cache.outputHeight != outputHeight ||
		cache.isVR != isVR ||
		std::abs(cache.centerScale - centerScale) > 1e-6f ||
		std::abs(cache.centerOffsets[0].x - centerOffsets[0].x) > 1e-6f ||
		std::abs(cache.centerOffsets[0].y - centerOffsets[0].y) > 1e-6f ||
		(isVR && (std::abs(cache.centerOffsets[1].x - centerOffsets[1].x) > 1e-6f ||
		          std::abs(cache.centerOffsets[1].y - centerOffsets[1].y) > 1e-6f));

	if (!cacheDirty)
		return true;

	cache.inputWidthPerEye = inputWidthPerEye;
	cache.inputHeight = inputHeight;
	cache.outputWidthPerEye = outputWidthPerEye;
	cache.outputHeight = outputHeight;
	cache.isVR = isVR;
	cache.centerScale = centerScale;
	cache.centerOffsets = centerOffsets;
	cache.rects = {};

	auto buildRect = [&](uint32_t eyeIndex) {
		FoveatedDispatchRect rect{};
		if (!inputWidthPerEye || !inputHeight || !outputWidthPerEye || !outputHeight)
			return rect;

		const float2 centerOffset = centerOffsets[eyeIndex];
		const auto bounds = FoveatedCommon::BuildCenteredDispatchBounds(0, outputWidthPerEye, outputHeight, centerScale, centerOffset.x, centerOffset.y);
		const int minX = bounds.minX;
		const int maxX = bounds.maxX;
		const int minY = bounds.minY;
		const int maxY = bounds.maxY;

		if (maxX <= minX || maxY <= minY)
			return rect;

		rect.outputOffsetX = static_cast<uint32_t>(minX);
		rect.outputOffsetY = static_cast<uint32_t>(minY);
		rect.outputWidth = static_cast<uint32_t>(maxX - minX);
		rect.outputHeight = static_cast<uint32_t>(maxY - minY);

		const float inputScaleX = static_cast<float>(inputWidthPerEye) / static_cast<float>(outputWidthPerEye);
		const float inputScaleY = static_cast<float>(inputHeight) / static_cast<float>(outputHeight);

		int inputMinX = static_cast<int>(std::floor(static_cast<float>(minX) * inputScaleX));
		int inputMaxX = static_cast<int>(std::ceil(static_cast<float>(maxX) * inputScaleX));
		int inputMinY = static_cast<int>(std::floor(static_cast<float>(minY) * inputScaleY));
		int inputMaxY = static_cast<int>(std::ceil(static_cast<float>(maxY) * inputScaleY));

		inputMinX = std::clamp(inputMinX, 0, static_cast<int>(inputWidthPerEye));
		inputMaxX = std::clamp(inputMaxX, 0, static_cast<int>(inputWidthPerEye));
		inputMinY = std::clamp(inputMinY, 0, static_cast<int>(inputHeight));
		inputMaxY = std::clamp(inputMaxY, 0, static_cast<int>(inputHeight));

		if (inputMaxX <= inputMinX || inputMaxY <= inputMinY)
			return FoveatedDispatchRect{};

		rect.inputOffsetX = static_cast<uint32_t>(inputMinX);
		rect.inputOffsetY = static_cast<uint32_t>(inputMinY);
		rect.inputWidth = static_cast<uint32_t>(inputMaxX - inputMinX);
		rect.inputHeight = static_cast<uint32_t>(inputMaxY - inputMinY);

		(void)eyeIndex;
		return rect;
	};

	cache.rects[0] = buildRect(0);
	if (isVR)
		cache.rects[1] = buildRect(1);

	return true;
}

bool Upscaling::EnsureFoveatedTexture(eastl::unique_ptr<Texture2D>& texture, ID3D11Resource* source, uint32_t width, uint32_t height, bool copyBindFlags, bool createSRV, bool createUAV, bool createRTV, const char* name)
{
	if (!source || !width || !height)
		return false;

	D3D11_TEXTURE2D_DESC sourceDesc{};
	if (!TryGetTexture2DDesc(source, sourceDesc))
		return false;

	bool recreate = !texture;
	if (!recreate) {
		recreate = texture->desc.Width != width ||
		           texture->desc.Height != height ||
		           texture->desc.Format != sourceDesc.Format;
		if (createSRV && !texture->srv)
			recreate = true;
		if (createUAV && !texture->uav)
			recreate = true;
		if (createRTV && !texture->rtv)
			recreate = true;
	}

	if (recreate) {
		texture = CreateTextureFromSource(source, width, height, copyBindFlags, createSRV, createUAV, name);
		if (!texture)
			return false;
	}

	if (createRTV && !texture->rtv) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = texture->desc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		texture->CreateRTV(rtvDesc);
	}

	if (createSRV && !texture->srv) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texture->desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		texture->CreateSRV(srvDesc);
	}

	if (createUAV && !texture->uav) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texture->desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		texture->CreateUAV(uavDesc);
	}

	return true;
}

void Upscaling::DestroyFoveatedResources()
{
	for (uint32_t i = 0; i < 2; ++i) {
		foveatedCenterColorIn[i].reset();
		foveatedCenterColorOut[i].reset();
		foveatedCenterDepth[i].reset();
		foveatedCenterMotionVectors[i].reset();
		foveatedCenterReactiveMask[i].reset();
		foveatedCenterTransparencyMask[i].reset();
	}
	foveatedRectCache = {};
}

void Upscaling::DispatchFoveatedPeripheryPass(ID3D11ShaderResourceView* sourceSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t outputWidth, uint32_t outputHeight, uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight, bool keepBindingsBound, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, float centerOffsetX, float centerOffsetY)
{
	auto* peripheryCS = GetFoveatedPeripheryCS();
	if (!peripheryCS || !sourceSRV || !outputUAV || !foveatedPeripheryCB)
		return;
	if (!dispatchWidth || !dispatchHeight)
		return;

	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!context || !deferred || !deferred->linearSampler)
		return;
	if (outputOffsetX >= outputWidth || outputOffsetY >= outputHeight)
		return;
	dispatchWidth = std::min(dispatchWidth, outputWidth - outputOffsetX);
	dispatchHeight = std::min(dispatchHeight, outputHeight - outputOffsetY);
	if (!dispatchWidth || !dispatchHeight)
		return;

	FoveatedPeripheryCB cbData{};
	cbData.outputDim = { static_cast<float>(outputWidth), static_cast<float>(outputHeight) };
	cbData.invOutputDim = {
		outputWidth > 0 ? 1.0f / static_cast<float>(outputWidth) : 0.0f,
		outputHeight > 0 ? 1.0f / static_cast<float>(outputHeight) : 0.0f
	};
	cbData.invSourceDim = {
		sourceWidth > 0 ? 1.0f / static_cast<float>(sourceWidth) : 0.0f,
		sourceHeight > 0 ? 1.0f / static_cast<float>(sourceHeight) : 0.0f
	};
	cbData.sourceScale = { sourceScaleX, sourceScaleY };
	cbData.sourceOffset = { sourceOffsetX, sourceOffsetY };
	cbData.dispatchDim = { static_cast<float>(dispatchWidth), static_cast<float>(dispatchHeight) };
	cbData.outputOffset = { static_cast<float>(outputOffsetX), static_cast<float>(outputOffsetY) };
	cbData.jitter = jitter;
	cbData.centerOffset = { centerOffsetX, centerOffsetY };
	cbData.pad0 = { 0.0f, 0.0f };
	const float centerArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);
	const float edgeBlurStrength = ClampPeripheryEdgeBlurStrength(settings.foveatedPeripheryEdgeBlurStrength);
	cbData.tuning0 = {
		centerArea,
		FoveatedCommon::kCenterFeather,
		0.0f,
		0.0f
	};
	cbData.tuning1 = {
		settings.foveatedPeripheryEdgeBlur ? 1.0f : 0.0f,
		edgeBlurStrength,
		12.0f,
		0.0f
	};
	cbData.tuning2 = {
		settings.foveatedPeripheryMaskVisualization ? 1.0f : 0.0f,
		0.0f,
		0.0f,
		0.0f
	};
	foveatedPeripheryCB->Update(cbData);

	if (keepBindingsBound) {
		context->Dispatch((dispatchWidth + 7u) >> 3, (dispatchHeight + 7u) >> 3, 1);
	} else {
		ID3D11Buffer* cb = foveatedPeripheryCB->CB();
		ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
		ID3D11ShaderResourceView* srvs[1] = { sourceSRV };
		ID3D11UnorderedAccessView* uavs[1] = { outputUAV };

		context->CSSetShader(peripheryCS, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetSamplers(0, 1, samplers);
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((dispatchWidth + 7u) >> 3, (dispatchHeight + 7u) >> 3, 1);

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11SamplerState* nullSampler[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void Upscaling::DispatchFoveatedBlendPass(ID3D11ShaderResourceView* centerSRV, ID3D11UnorderedAccessView* outputUAV, uint32_t eyeIndex, uint32_t outputWidthPerEye, uint32_t outputHeight, const FoveatedDispatchRect& rect, uint32_t dispatchOffsetX, uint32_t dispatchOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight)
{
	if (!centerSRV || !outputUAV || rect.outputWidth == 0 || rect.outputHeight == 0 || !foveatedCenterBlendCB)
		return;
	if (!dispatchWidth || !dispatchHeight)
		return;

	auto* blendCS = GetFoveatedCenterBlendCS();
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!blendCS || !context || !deferred || !deferred->linearSampler)
		return;
	if (dispatchOffsetX >= outputWidthPerEye || dispatchOffsetY >= outputHeight)
		return;

	dispatchWidth = std::min(dispatchWidth, outputWidthPerEye - dispatchOffsetX);
	dispatchHeight = std::min(dispatchHeight, outputHeight - dispatchOffsetY);
	if (!dispatchWidth || !dispatchHeight)
		return;

	const uint32_t rectMinX = rect.outputOffsetX;
	const uint32_t rectMinY = rect.outputOffsetY;
	const uint32_t rectMaxX = rect.outputOffsetX + rect.outputWidth;
	const uint32_t rectMaxY = rect.outputOffsetY + rect.outputHeight;

	const uint32_t dispatchMinX = std::max(dispatchOffsetX, rectMinX);
	const uint32_t dispatchMinY = std::max(dispatchOffsetY, rectMinY);
	const uint32_t dispatchMaxX = std::min(dispatchOffsetX + dispatchWidth, rectMaxX);
	const uint32_t dispatchMaxY = std::min(dispatchOffsetY + dispatchHeight, rectMaxY);
	if (dispatchMaxX <= dispatchMinX || dispatchMaxY <= dispatchMinY)
		return;

	const uint32_t actualDispatchWidth = dispatchMaxX - dispatchMinX;
	const uint32_t actualDispatchHeight = dispatchMaxY - dispatchMinY;
	const uint32_t sourceOffsetX = dispatchMinX - rectMinX;
	const uint32_t sourceOffsetY = dispatchMinY - rectMinY;

	FoveatedCenterBlendCB cbData{};
	cbData.invOutputDim = {
		outputWidthPerEye > 0 ? 1.0f / static_cast<float>(outputWidthPerEye) : 0.0f,
		outputHeight > 0 ? 1.0f / static_cast<float>(outputHeight) : 0.0f
	};
	const float2 centerOffset = GetResolvedFoveatedMaskCenterOffset(eyeIndex);
	cbData.centerScale = ClampFoveatedCenterArea(settings.foveatedCenterArea);
	cbData.centerFeather = FoveatedCommon::kCenterFeather;
	cbData.centerOffset = centerOffset;
	cbData.outputOffset = { static_cast<float>(dispatchMinX), static_cast<float>(dispatchMinY) };
	cbData.dispatchDim = { static_cast<float>(actualDispatchWidth), static_cast<float>(actualDispatchHeight) };
	cbData.sourceOffset = { static_cast<float>(sourceOffsetX), static_cast<float>(sourceOffsetY) };
	cbData.invSourceDim = {
		rect.outputWidth > 0 ? 1.0f / static_cast<float>(rect.outputWidth) : 0.0f,
		rect.outputHeight > 0 ? 1.0f / static_cast<float>(rect.outputHeight) : 0.0f
	};
	foveatedCenterBlendCB->Update(cbData);

	(void)eyeIndex;

	ID3D11Buffer* cb = foveatedCenterBlendCB->CB();
	ID3D11SamplerState* samplers[1] = { deferred->linearSampler };
	ID3D11ShaderResourceView* srvs[1] = { centerSRV };
	ID3D11UnorderedAccessView* uavs[1] = { outputUAV };

	context->CSSetShader(blendCS, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, 1, samplers);
	context->CSSetShaderResources(0, 1, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->Dispatch((actualDispatchWidth + 7u) >> 3, (actualDispatchHeight + 7u) >> 3, 1);

	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
	ID3D11SamplerState* nullSampler[1] = { nullptr };
	ID3D11Buffer* nullCB[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
	context->CSSetSamplers(0, 1, nullSampler);
	context->CSSetConstantBuffers(0, 1, nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
}

bool Upscaling::DispatchSingleFoveatedVendorEye(UpscaleMethod a_upscaleMethod, uint32_t eyeIndex, ID3D11Resource* colorIn, ID3D11Resource* depthIn, ID3D11Resource* motionVectorsIn, ID3D11Resource* reactiveMaskIn, ID3D11Resource* transparencyMaskIn, uint32_t outputWidthPerEye, uint32_t outputHeight, uint32_t innerMinX, uint32_t innerMinY, uint32_t innerMaxX, uint32_t innerMaxY, uint32_t colorInputBaseOffsetX, uint32_t depthInputBaseOffsetX, uint32_t auxInputBaseOffsetX)
{
	if (eyeIndex > 1)
		return false;

	const auto& rect = foveatedRectCache.rects[eyeIndex];
	if (!rect.outputWidth || !rect.outputHeight || !rect.inputWidth || !rect.inputHeight)
		return false;

	const std::string suffix = eyeIndex == 0 ? "Left" : "Right";

	if (!EnsureFoveatedTexture(foveatedCenterColorIn[eyeIndex], colorIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_ColorIn_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterColorOut[eyeIndex], colorIn, rect.outputWidth, rect.outputHeight, false, true, false, false, ("Upscale_FoveatedCenter_ColorOut_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterDepth[eyeIndex], depthIn, rect.inputWidth, rect.inputHeight, true, false, false, false, ("Upscale_FoveatedCenter_Depth_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterMotionVectors[eyeIndex], motionVectorsIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_MVec_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterReactiveMask[eyeIndex], reactiveMaskIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_Reactive_" + suffix).c_str()))
		return false;
	if (!EnsureFoveatedTexture(foveatedCenterTransparencyMask[eyeIndex], transparencyMaskIn, rect.inputWidth, rect.inputHeight, false, false, false, false, ("Upscale_FoveatedCenter_Transparency_" + suffix).c_str()))
		return false;

	auto context = globals::d3d::context;
	if (!context)
		return false;

	D3D11_BOX colorSrcBox{
		colorInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		colorInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};
	D3D11_BOX depthSrcBox{
		depthInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		depthInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};
	D3D11_BOX auxSrcBox{
		auxInputBaseOffsetX + rect.inputOffsetX,
		rect.inputOffsetY,
		0u,
		auxInputBaseOffsetX + rect.inputOffsetX + rect.inputWidth,
		rect.inputOffsetY + rect.inputHeight,
		1u
	};

	context->CopySubresourceRegion(foveatedCenterColorIn[eyeIndex]->resource.get(), 0, 0, 0, 0, colorIn, 0, &colorSrcBox);
	context->CopySubresourceRegion(foveatedCenterDepth[eyeIndex]->resource.get(), 0, 0, 0, 0, depthIn, 0, &depthSrcBox);
	context->CopySubresourceRegion(foveatedCenterMotionVectors[eyeIndex]->resource.get(), 0, 0, 0, 0, motionVectorsIn, 0, &auxSrcBox);
	context->CopySubresourceRegion(foveatedCenterReactiveMask[eyeIndex]->resource.get(), 0, 0, 0, 0, reactiveMaskIn, 0, &auxSrcBox);
	context->CopySubresourceRegion(foveatedCenterTransparencyMask[eyeIndex]->resource.get(), 0, 0, 0, 0, transparencyMaskIn, 0, &auxSrcBox);

	bool dispatchOK = false;
	if (a_upscaleMethod == UpscaleMethod::kDLSS) {
		const float outputWidthPerEyeF = std::max(1.0f, static_cast<float>(outputWidthPerEye));
		const float outputHeightF = std::max(1.0f, static_cast<float>(outputHeight));
		const float rectCenterX = (static_cast<float>(rect.outputOffsetX) + static_cast<float>(rect.outputWidth) * 0.5f) / outputWidthPerEyeF;
		const float rectCenterY = (static_cast<float>(rect.outputOffsetY) + static_cast<float>(rect.outputHeight) * 0.5f) / outputHeightF;
		const float pinholeOffsetX = std::clamp((rectCenterX - 0.5f) * 2.0f, -1.0f, 1.0f);
		// Texture-space Y grows downward, while clip-space Y grows upward.
		const float pinholeOffsetY = std::clamp((0.5f - rectCenterY) * 2.0f, -1.0f, 1.0f);

		dispatchOK = streamline.UpscaleRegion(
			eyeIndex,
			foveatedCenterColorIn[eyeIndex]->resource.get(),
			foveatedCenterColorOut[eyeIndex]->resource.get(),
			foveatedCenterDepth[eyeIndex]->resource.get(),
			foveatedCenterMotionVectors[eyeIndex]->resource.get(),
			foveatedCenterReactiveMask[eyeIndex]->resource.get(),
			foveatedCenterTransparencyMask[eyeIndex]->resource.get(),
			rect.inputWidth,
			rect.inputHeight,
			rect.outputWidth,
			rect.outputHeight,
			pinholeOffsetX,
			pinholeOffsetY);
	} else if (a_upscaleMethod == UpscaleMethod::kFSR) {
		dispatchOK = fidelityFX.UpscaleRegion(
			eyeIndex,
			foveatedCenterColorIn[eyeIndex]->resource.get(),
			foveatedCenterDepth[eyeIndex]->resource.get(),
			foveatedCenterMotionVectors[eyeIndex]->resource.get(),
			foveatedCenterReactiveMask[eyeIndex]->resource.get(),
			foveatedCenterTransparencyMask[eyeIndex]->resource.get(),
			foveatedCenterColorOut[eyeIndex]->resource.get(),
			rect.inputWidth,
			rect.inputHeight,
			rect.outputWidth,
			rect.outputHeight,
			static_cast<float>(rect.inputWidth),
			static_cast<float>(rect.inputHeight),
			settings.sharpnessFSR);
	}

	if (!dispatchOK)
		return false;

	if (!vrIntermediateColorOut[eyeIndex] || !vrIntermediateColorOut[eyeIndex]->uav || !vrIntermediateColorOut[eyeIndex]->resource)
		return false;
	if (!foveatedCenterColorOut[eyeIndex] || !foveatedCenterColorOut[eyeIndex]->resource || !foveatedCenterColorOut[eyeIndex]->srv)
		return false;

	const uint32_t rectMinX = rect.outputOffsetX;
	const uint32_t rectMinY = rect.outputOffsetY;
	const uint32_t rectMaxX = rect.outputOffsetX + rect.outputWidth;
	const uint32_t rectMaxY = rect.outputOffsetY + rect.outputHeight;

	uint32_t interiorMinX = std::max(innerMinX, rectMinX);
	uint32_t interiorMinY = std::max(innerMinY, rectMinY);
	uint32_t interiorMaxX = std::min(innerMaxX, rectMaxX);
	uint32_t interiorMaxY = std::min(innerMaxY, rectMaxY);

	if (interiorMaxX > interiorMinX && interiorMaxY > interiorMinY) {
		D3D11_BOX centerInteriorBox{
			interiorMinX - rectMinX,
			interiorMinY - rectMinY,
			0u,
			interiorMaxX - rectMinX,
			interiorMaxY - rectMinY,
			1u
		};
		context->CopySubresourceRegion(
			vrIntermediateColorOut[eyeIndex]->resource.get(),
			0,
			interiorMinX,
			interiorMinY,
			0,
			foveatedCenterColorOut[eyeIndex]->resource.get(),
			0,
			&centerInteriorBox);
	}

	ID3D11UnorderedAccessView* outputUAV = vrIntermediateColorOut[eyeIndex]->uav.get();
	ID3D11ShaderResourceView* centerSRV = foveatedCenterColorOut[eyeIndex]->srv.get();

	bool dispatchedRingBlend = false;
	auto dispatchBlendRing = [&](uint32_t offsetX, uint32_t offsetY, uint32_t width, uint32_t height) {
		if (!width || !height)
			return;
		dispatchedRingBlend = true;
		DispatchFoveatedBlendPass(
			centerSRV,
			outputUAV,
			eyeIndex,
			outputWidthPerEye,
			outputHeight,
			rect,
			offsetX,
			offsetY,
			width,
			height);
	};

	if (interiorMaxX > interiorMinX && interiorMaxY > interiorMinY) {
		const uint32_t middleMinY = interiorMinY;
		const uint32_t middleMaxY = interiorMaxY;
		const uint32_t middleHeight = middleMaxY - middleMinY;

		dispatchBlendRing(rectMinX, rectMinY, rect.outputWidth, interiorMinY - rectMinY);
		dispatchBlendRing(rectMinX, interiorMaxY, rect.outputWidth, rectMaxY - interiorMaxY);
		dispatchBlendRing(rectMinX, middleMinY, interiorMinX - rectMinX, middleHeight);
		dispatchBlendRing(interiorMaxX, middleMinY, rectMaxX - interiorMaxX, middleHeight);
	}

	if (!dispatchedRingBlend) {
		DispatchFoveatedBlendPass(
			centerSRV,
			outputUAV,
			eyeIndex,
			outputWidthPerEye,
			outputHeight,
			rect,
			rect.outputOffsetX,
			rect.outputOffsetY,
			rect.outputWidth,
			rect.outputHeight);
	}

	return true;
}

bool Upscaling::DispatchFoveatedVendorUpscaling(UpscaleMethod a_upscaleMethod, ID3D11Resource* colorTexture, ID3D11Resource* depthTexture, ID3D11Resource* motionVectors, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11ShaderResourceView* colorSRV, bool depthAlreadyPrepared)
{
	if (!globals::game::isVR)
		return false;

	if (!colorTexture || !depthTexture || !motionVectors || !reactiveMask || !transparencyMask)
		return false;

	auto state = globals::state;
	if (!state)
		return false;

	auto renderSize = Util::ConvertToDynamic(state->screenSize);
	const uint32_t outputWidthPerEye = static_cast<uint32_t>(state->screenSize.x / 2.0f);
	const uint32_t outputHeight = static_cast<uint32_t>(state->screenSize.y);
	const uint32_t inputWidthPerEye = static_cast<uint32_t>(renderSize.x / 2.0f);
	const uint32_t inputHeight = static_cast<uint32_t>(renderSize.y);

	if (!BuildFoveatedDispatchRects(inputWidthPerEye, inputHeight, outputWidthPerEye, outputHeight, true, settings.foveatedCenterArea))
		return false;

	auto* peripheryCS = GetFoveatedPeripheryCS();
	const bool visualizeMask = settings.foveatedPeripheryMaskVisualization;
	auto* blendCS = visualizeMask ? nullptr : GetFoveatedCenterBlendCS();
	if (!peripheryCS || !foveatedPeripheryCB || (!visualizeMask && (!blendCS || !foveatedCenterBlendCB)))
		return false;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!context || !deferred || !deferred->linearSampler)
		return false;

	const bool useDirectSourcePath = (a_upscaleMethod == UpscaleMethod::kDLSS) && settings.qualityMode == 0 && colorSRV != nullptr;
	if (!useDirectSourcePath) {
		PreparePerEyeInputs(colorTexture, depthTexture, motionVectors, reactiveMask, transparencyMask, false, !depthAlreadyPrepared);
	}

	const float centerScale = ClampFoveatedCenterArea(settings.foveatedCenterArea);

	for (uint32_t eye = 0; eye < 2; ++eye) {
		const float2 centerOffset = GetResolvedFoveatedMaskCenterOffset(eye);
		const auto innerBounds = FoveatedCommon::BuildCenteredInscribedEllipseRect(outputWidthPerEye, outputHeight, centerScale, centerOffset.x, centerOffset.y);
		const uint32_t innerMinX = static_cast<uint32_t>(innerBounds.minX);
		const uint32_t innerMaxX = static_cast<uint32_t>(innerBounds.maxX);
		const uint32_t innerMinY = static_cast<uint32_t>(innerBounds.minY);
		const uint32_t innerMaxY = static_cast<uint32_t>(innerBounds.maxY);
		const bool hasCenterInterior = innerMaxX > innerMinX && innerMaxY > innerMinY;

		if (!vrIntermediateColorOut[eye] || !vrIntermediateColorOut[eye]->uav) {
			return false;
		}
		if (!useDirectSourcePath && (!vrIntermediateColorIn[eye] || !vrIntermediateColorIn[eye]->srv)) {
			return false;
		}
		if (!visualizeMask &&
			(!vrIntermediateMotionVectors[eye] || !vrIntermediateReactiveMask[eye] || !vrIntermediateTransparencyMask[eye])) {
			return false;
		}
		if (!visualizeMask && !useDirectSourcePath && !vrIntermediateDepth[eye]) {
			return false;
		}

		ID3D11ShaderResourceView* peripherySourceSRV = useDirectSourcePath ? colorSRV : vrIntermediateColorIn[eye]->srv.get();
		const uint32_t peripherySourceWidth = useDirectSourcePath ? (inputWidthPerEye * 2u) : inputWidthPerEye;
		const uint32_t peripherySourceHeight = inputHeight;
		const float peripherySourceScaleX = useDirectSourcePath ? 0.5f : 1.0f;
		const float peripherySourceScaleY = 1.0f;
		const float peripherySourceOffsetX = useDirectSourcePath ? (eye == 1 ? 0.5f : 0.0f) : 0.0f;
		const float peripherySourceOffsetY = 0.0f;

		// Batch periphery setup once per eye to avoid repeated CS bind/unbind overhead.
		ID3D11Buffer* peripheryCB = foveatedPeripheryCB->CB();
		ID3D11SamplerState* peripherySamplers[1] = { deferred->linearSampler };
		ID3D11ShaderResourceView* peripherySRVs[1] = { peripherySourceSRV };
		ID3D11UnorderedAccessView* peripheryUAVs[1] = { vrIntermediateColorOut[eye]->uav.get() };
		context->CSSetShader(peripheryCS, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &peripheryCB);
		context->CSSetSamplers(0, 1, peripherySamplers);
		context->CSSetShaderResources(0, 1, peripherySRVs);
		context->CSSetUnorderedAccessViews(0, 1, peripheryUAVs, nullptr);

		auto dispatchPeripheryBand = [&](uint32_t outputOffsetX, uint32_t outputOffsetY, uint32_t dispatchWidth, uint32_t dispatchHeight) {
			DispatchFoveatedPeripheryPass(
				peripherySourceSRV,
				vrIntermediateColorOut[eye]->uav.get(),
				peripherySourceWidth,
				peripherySourceHeight,
				outputWidthPerEye,
				outputHeight,
				outputOffsetX,
				outputOffsetY,
				dispatchWidth,
				dispatchHeight,
				true,
				peripherySourceScaleX,
				peripherySourceScaleY,
				peripherySourceOffsetX,
				peripherySourceOffsetY,
				centerOffset.x,
				centerOffset.y);
		};

		if (visualizeMask) {
			dispatchPeripheryBand(0, 0, outputWidthPerEye, outputHeight);
		} else if (hasCenterInterior) {
			const uint32_t innerHeight = innerMaxY - innerMinY;
			dispatchPeripheryBand(0, 0, outputWidthPerEye, innerMinY);
			dispatchPeripheryBand(0, innerMaxY, outputWidthPerEye, outputHeight - innerMaxY);
			dispatchPeripheryBand(0, innerMinY, innerMinX, innerHeight);
			dispatchPeripheryBand(innerMaxX, innerMinY, outputWidthPerEye - innerMaxX, innerHeight);
		} else {
			dispatchPeripheryBand(0, 0, outputWidthPerEye, outputHeight);
		}

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11SamplerState* nullSampler[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);

		if (visualizeMask)
			continue;

		ID3D11Resource* centerColorInput = useDirectSourcePath ? colorTexture : vrIntermediateColorIn[eye]->resource.get();
		ID3D11Resource* centerDepthInput = useDirectSourcePath ? depthTexture : vrIntermediateDepth[eye]->resource.get();
		const uint32_t combinedEyeInputOffsetX = eye * inputWidthPerEye;

		if (!DispatchSingleFoveatedVendorEye(
				a_upscaleMethod,
				eye,
				centerColorInput,
				centerDepthInput,
				vrIntermediateMotionVectors[eye]->resource.get(),
				vrIntermediateReactiveMask[eye]->resource.get(),
				vrIntermediateTransparencyMask[eye]->resource.get(),
				outputWidthPerEye,
				outputHeight,
				innerMinX,
				innerMinY,
				innerMaxX,
				innerMaxY,
				useDirectSourcePath ? combinedEyeInputOffsetX : 0u,
				useDirectSourcePath ? combinedEyeInputOffsetX : 0u,
				0u)) {
			return false;
		}
	}

	FinalizePerEyeOutputs(colorTexture);
	return true;
}

void Upscaling::CreateVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
	ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc)
{
	// All buffers are per-eye: Streamline validates all extents against the input color texture
	// dimensions, so every tagged resource must be isolated per-eye at {0,0}.
	for (int i = 0; i < 2; i++) {
		std::string suffix = (i == 0) ? "Left" : "Right";

		vrIntermediateColorIn[i] = CreateTextureFromSource(colorSrc, inWidth, inHeight, false, true, true, ("Upscale_ColorIn_" + suffix).c_str());
		vrIntermediateColorOut[i] = CreateTextureFromSource(colorSrc, outWidth, outHeight, false, true, true, ("Upscale_ColorOut_" + suffix).c_str());

		// Depth: R32_TYPELESS base (matches kMAIN), with R32_FLOAT SRV for ClearHMDMaskCS.
		// CopySubresourceRegion requires matching typeless formats; SRV reinterprets as R32_FLOAT.
		{
			D3D11_TEXTURE2D_DESC depthDesc = {};
			depthDesc.Width = inWidth;
			depthDesc.Height = inHeight;
			depthDesc.MipLevels = 1;
			depthDesc.ArraySize = 1;
			depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			depthDesc.SampleDesc.Count = 1;
			depthDesc.Usage = D3D11_USAGE_DEFAULT;
			depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			vrIntermediateDepth[i] = eastl::make_unique<Texture2D>(depthDesc);

			Util::SetResourceName(vrIntermediateDepth[i]->resource.get(), ("Upscale_Depth_" + suffix).c_str());

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			vrIntermediateDepth[i]->CreateSRV(srvDesc);
		}

		vrIntermediateMotionVectors[i] = CreateTextureFromSource(mvecSrc, inWidth, inHeight, false, true, true, ("Upscale_MVec_" + suffix).c_str());
		vrIntermediateReactiveMask[i] = CreateTextureFromSource(reactiveSrc, inWidth, inHeight, false, true, true, ("Upscale_Reactive_" + suffix).c_str());
		vrIntermediateTransparencyMask[i] = CreateTextureFromSource(transparencySrc, inWidth, inHeight, false, true, true, ("Upscale_Transparency_" + suffix).c_str());
	}

	logger::info("[Upscaling] Created VR intermediate textures: per-eye in {}x{}, out {}x{}",
		inWidth, inHeight, outWidth, outHeight);
}

void Upscaling::EnsureVRIntermediateTextures(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
	ID3D11Resource* colorSrc, ID3D11Resource* mvecSrc, ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc)
{
	bool hasAllIntermediates =
		vrIntermediateColorIn[0] && vrIntermediateColorIn[1] &&
		vrIntermediateColorOut[0] && vrIntermediateColorOut[1] &&
		vrIntermediateDepth[0] && vrIntermediateDepth[1] &&
		vrIntermediateMotionVectors[0] && vrIntermediateMotionVectors[1] &&
		vrIntermediateReactiveMask[0] && vrIntermediateReactiveMask[1] &&
		vrIntermediateTransparencyMask[0] && vrIntermediateTransparencyMask[1];

	bool needsRecreate = !hasAllIntermediates;
	if (!needsRecreate) {
		needsRecreate =
			(vrIntermediateColorIn[0]->desc.Width != inWidth || vrIntermediateColorIn[0]->desc.Height != inHeight ||
				vrIntermediateColorOut[0]->desc.Width != outWidth || vrIntermediateColorOut[0]->desc.Height != outHeight ||
				!vrIntermediateColorOut[0]->uav || !vrIntermediateColorOut[1]->uav ||
				!vrIntermediateMotionVectors[0]->uav || !vrIntermediateMotionVectors[1]->uav ||
				!vrIntermediateReactiveMask[0]->uav || !vrIntermediateReactiveMask[1]->uav ||
				!vrIntermediateTransparencyMask[0]->uav || !vrIntermediateTransparencyMask[1]->uav);
	}

	if (needsRecreate) {
		logger::info("[Upscaling] (Re)creating VR intermediates: per-eye in {}x{}, out {}x{}",
			inWidth, inHeight, outWidth, outHeight);
		CreateVRIntermediateTextures(inWidth, inHeight, outWidth, outHeight, colorSrc, mvecSrc, reactiveSrc, transparencySrc);
	}
}

void Upscaling::PreparePerEyeInputs(ID3D11Resource* colorSrc, ID3D11Resource* depthSrc, ID3D11Resource* mvecSrc,
	ID3D11Resource* reactiveSrc, ID3D11Resource* transparencySrc, bool copyAuxiliaryInputs, bool copyDepthInput)
{
	if (!globals::game::isVR)
		return;

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("VR Upscaling Prepare");

	auto context = globals::d3d::context;
	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	uint32_t eyeWidthOut = (uint32_t)(screenSize.x / 2);
	uint32_t eyeHeightOut = (uint32_t)screenSize.y;
	uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
	uint32_t eyeHeightIn = (uint32_t)renderSize.y;

	EnsureVRIntermediateTextures(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut, colorSrc, mvecSrc, reactiveSrc, transparencySrc);

	// Extract both eyes' required inputs from combined stereo buffers.
	// Reactive / transparency / encoded motion vectors can be pre-generated directly per-eye by the encode pass.
	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXIn = (i == 1) ? eyeWidthIn : 0;
		D3D11_BOX srcBox = { offsetXIn, 0, 0, offsetXIn + eyeWidthIn, eyeHeightIn, 1 };

		context->CopySubresourceRegion(vrIntermediateColorIn[i]->resource.get(), 0, 0, 0, 0, colorSrc, 0, &srcBox);
		if (copyDepthInput)
			context->CopySubresourceRegion(vrIntermediateDepth[i]->resource.get(), 0, 0, 0, 0, depthSrc, 0, &srcBox);
		if (copyAuxiliaryInputs) {
			context->CopySubresourceRegion(vrIntermediateMotionVectors[i]->resource.get(), 0, 0, 0, 0, mvecSrc, 0, &srcBox);
			context->CopySubresourceRegion(vrIntermediateTransparencyMask[i]->resource.get(), 0, 0, 0, 0, transparencySrc, 0, &srcBox);
			context->CopySubresourceRegion(vrIntermediateReactiveMask[i]->resource.get(), 0, 0, 0, 0, reactiveSrc, 0, &srcBox);
		}
	}

	// Zero color where depth == 0 (HMD hidden area) in each per-eye buffer.
	// Bind CS/SRV/CB once for both eyes to reduce per-frame CPU overhead.
	auto& depthTexture = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!vrClearHMDMaskCS) {
		vrClearHMDMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/ClearHMDMaskCS.hlsl", {}, "cs_5_0"));

		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 16;  // 4 uints
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = 0;
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, vrClearHMDMaskCB.put()));
	}

	if (vrClearHMDMaskCS && vrClearHMDMaskCB) {
		auto dispatchX = (eyeWidthIn + 7) / 8;
		auto dispatchY = (eyeHeightIn + 7) / 8;

		context->CSSetShader(vrClearHMDMaskCS.get(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthTexture.depthSRV };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11Buffer* cbs[1] = { vrClearHMDMaskCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		for (uint32_t i = 0; i < 2; ++i) {
			uint32_t depthOffset = (i == 1) ? eyeWidthIn : 0;
			uint32_t offsets[4] = { depthOffset, 0, 0, 0 };
			context->UpdateSubresource(vrClearHMDMaskCB.get(), 0, nullptr, offsets, 0, 0);

			ID3D11UnorderedAccessView* uavs[1] = { vrIntermediateColorIn[i]->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch(dispatchX, dispatchY, 1);
		}

		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	if (state->frameAnnotations)
		state->EndPerfEvent();
}

void Upscaling::FinalizePerEyeOutputs(ID3D11Resource* colorDst)
{
	if (!globals::game::isVR)
		return;

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("VR Upscaling Finalize");

	auto context = globals::d3d::context;
	auto screenSize = state->screenSize;

	uint32_t eyeWidthOut = (uint32_t)(screenSize.x / 2);
	uint32_t eyeHeightOut = (uint32_t)screenSize.y;

	// Write upscaled outputs back
	for (uint32_t i = 0; i < 2; ++i) {
		uint32_t offsetXOut = (i == 1) ? eyeWidthOut : 0;
		D3D11_BOX outBox = { 0, 0, 0, eyeWidthOut, eyeHeightOut, 1 };
		context->CopySubresourceRegion(colorDst, 0, offsetXOut, 0, 0, vrIntermediateColorOut[i]->resource.get(), 0, &outBox);
	}

	if (state->frameAnnotations)
		state->EndPerfEvent();
}

void Upscaling::ClearHMDMask(ID3D11UnorderedAccessView* colorUAV, ID3D11ShaderResourceView* depthSRV,
	uint32_t eyeWidth, uint32_t eyeHeight, uint32_t depthOffsetX, uint32_t colorOffsetX)
{
	if (!globals::game::isVR)
		return;

	auto context = globals::d3d::context;

	if (!vrClearHMDMaskCS) {
		vrClearHMDMaskCS.attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/ClearHMDMaskCS.hlsl", {}, "cs_5_0"));

		D3D11_BUFFER_DESC cbDesc = {};
		cbDesc.ByteWidth = 16;  // 4 uints
		cbDesc.Usage = D3D11_USAGE_DEFAULT;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = 0;
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, vrClearHMDMaskCB.put()));
	}

	if (vrClearHMDMaskCS) {
		auto dispatchX = (eyeWidth + 7) / 8;
		auto dispatchY = (eyeHeight + 7) / 8;

		context->CSSetShader(vrClearHMDMaskCS.get(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[1] = { depthSRV };
		context->CSSetShaderResources(0, 1, srvs);

		ID3D11UnorderedAccessView* uavs[1] = { colorUAV };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		uint32_t offsets[4] = { depthOffsetX, colorOffsetX, 0, 0 };
		context->UpdateSubresource(vrClearHMDMaskCB.get(), 0, nullptr, offsets, 0, 0);

		ID3D11Buffer* cbs[1] = { vrClearHMDMaskCB.get() };
		context->CSSetConstantBuffers(0, 1, cbs);

		context->Dispatch(dispatchX, dispatchY, 1);

		// Unbind
		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ID3D11UnorderedAccessView* nullUAV[1] = { nullptr };
		ID3D11Buffer* nullCB[1] = { nullptr };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCB);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculate halton number for index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = GetUpscaleMethod();

	// When no upscaling method is active, preserve vanilla TAA state.
	// UpdateJitter (called immediately after this hook) owns the non-upscaling path.
	if (upscaleMethod == UpscaleMethod::kNONE)
		return;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	// CS TAA replaces vanilla TAA, so disable water TAA there.
	// FSR/DLSS keep water TAA enabled.
	bool* enableWaterTAA = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISTemporalAA) + 0x38LL);
	*enableWaterTAA = upscaleMethod != UpscaleMethod::kTAA;

	BSImagespaceShaderISTemporalAA->taaEnabled = true;
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport)
{
	auto upscaleMethod = GetUpscaleMethod();

	// Delete or create resources as necessary
	CheckResources(upscaleMethod);

	// Cache original TAA values for UI
	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	// Get full screen size
	auto state = globals::state;
	auto screenSize = state->screenSize;

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		float resolutionScaleBase = 1.0f / ffxFsr3GetUpscaleRatioFromQualityMode((FfxFsr3QualityMode)settings.qualityMode);

		auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase);
		auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase);

		resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
		resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);

		auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

		GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);

		if (globals::game::isVR)
			a_viewport->projectionPosScaleX = -jitter.x / renderWidth;
		else
			a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

		a_viewport->projectionPosScaleY = 2.0f * jitter.y / renderHeight;
	} else {
		resolutionScale = { 1.0f, 1.0f };

		if (globals::game::isVR)
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth;
		else
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth / 2.0f;

		jitter.y = a_viewport->projectionPosScaleY * screenHeight / 2.0f;
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	// Disable dynamic resolution unless the game explicitly enables it
	if (!globals::game::isVR)
		runtimeData.dynamicResolutionLock = 1;
}

void Upscaling::SetupResources()
{
	QueryPerformanceFrequency(&qpf);

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;                           // Enable depth testing
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;  // Write to all depth bits
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;          // Always pass depth test (write all depths)

	if (globals::game::isVR) {
		depthStencilDesc.StencilEnable = true;     // Enable stencil testing
		depthStencilDesc.StencilReadMask = 0xFF;   // Read all stencil bits
		depthStencilDesc.StencilWriteMask = 0xFF;  // Write to all stencil bits

		// Configure front-facing stencil operations
		depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;       // Replace on stencil fail
		depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;  // Replace on depth fail
		depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;    // Replace on pass
		depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;       // Always pass stencil test

		// Configure back-facing stencil operations (same as front)
		depthStencilDesc.BackFace.StencilFailOp = depthStencilDesc.FrontFace.StencilFailOp;
		depthStencilDesc.BackFace.StencilDepthFailOp = depthStencilDesc.FrontFace.StencilDepthFailOp;
		depthStencilDesc.BackFace.StencilPassOp = depthStencilDesc.FrontFace.StencilPassOp;
		depthStencilDesc.BackFace.StencilFunc = depthStencilDesc.FrontFace.StencilFunc;
	} else {
		depthStencilDesc.StencilEnable = false;  // Disable stencil testing
	}

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));

	// Create jitter offset constant buffer for depth upscaling
	jitterCB = new ConstantBuffer(ConstantBufferDesc<JitterCB>());

	// Create upscaling data constant buffer for encode textures compute shader
	upscalingDataCB = new ConstantBuffer(ConstantBufferDesc<UpscalingDataCB>());
	foveatedPeripheryCB = new ConstantBuffer(ConstantBufferDesc<FoveatedPeripheryCB>());
	foveatedCenterBlendCB = new ConstantBuffer(ConstantBufferDesc<FoveatedCenterBlendCB>());

	// Create blend state for depth upscaling
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));

	// Create rasterizer state for fullscreen rendering
	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));

	CheckResources(GetUpscaleMethod());

	rcas.Initialize();

	if (d3d12SwapChainActive)
		dx12SwapChain.CreateSharedResources();

	copyDepthToSharedBufferPS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
}

void Upscaling::ClearShaderCache()
{
	for (int i = 0; i < 5; ++i) {
		encodeTexturesCS[i] = nullptr;  // com_ptr automatically releases
	}

	depthRefractionUpscalePS = nullptr;  // com_ptr automatically releases
	underwaterMaskUpscalePS = nullptr;   // com_ptr automatically releases
	upscaleVS = nullptr;                 // com_ptr automatically releases
	foveatedPeripheryCS = nullptr;       // com_ptr automatically releases
	foveatedCenterBlendCS = nullptr;     // com_ptr automatically releases
}

void Upscaling::CopySharedD3D12Resources()
{
	globals::state->BeginPerfEvent("Copy Shared D3D12 Resources");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	context->CopyResource(dx12SwapChain.motionVectorBufferShared12->resource11, motionVector.texture);

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	{
		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(GetUpscaleVS(), nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(upscaleRasterizerState.get());
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(views), views);

		// Set render target view for pixel shader output
		ID3D11RenderTargetView* rtvs[1] = { dx12SwapChain.depthBufferShared12->rtv };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(copyDepthToSharedBufferPS.get(), nullptr, 0);

		context->Draw(3, 0);
	}

	// Clean up
	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(views), views);

	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(nullptr, nullptr, 0);

	globals::state->EndPerfEvent();
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	auto viewport = globals::game::graphicsState;

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	globals::game::renderer->UpdateViewPort(0, 0, 1);
	UpdateCameraData();

	if (d3d12SwapChainActive)
		SetUIBuffer();

	globals::state->UpdateSharedData(false, false);
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12SwapChainActive) {
		// Use frame latency waitable object if available for better frame pacing
		HANDLE waitableObject = GetFrameLatencyWaitableObject();

		// Wait for the next frame presentation slot
		WaitForSingleObject(waitableObject, INFINITE);

		if (settings.frameLimitMode) {
			// Fall back to the original timing method
			// Use integer arithmetic for more precise timing
			int64_t targetFrameTimeNS = int64_t(1000000000.0 / (refreshRate * (settings.frameGenerationMode && !globals::game::ui->GameIsPaused() ? 0.5 : 1.0)));
			int64_t targetFrameTicks = (targetFrameTimeNS * qpf.QuadPart) / 1000000000LL;

			static LARGE_INTEGER lastFrame = {};
			LARGE_INTEGER timeNow;
			QueryPerformanceCounter(&timeNow);

			int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
			if (delta < targetFrameTicks) {
				TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
			}
			QueryPerformanceCounter(&lastFrame);
		}
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						// get the refresh rate
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsFrameGenerationActive() const
{
	return IsFrameGenerationDx12PathActive() && settings.frameGenerationMode && fidelityFX.isFrameGenActive;
}

bool Upscaling::IsFrameGenerationDx12PathActive() const
{
	// Frame generation in this implementation runs via the DX12 swap-chain proxy path.
	return d3d12SwapChainActive && !globals::game::isVR;
}

bool Upscaling::IsUpscalingActive() const
{
	auto method = GetUpscaleMethod();

	// Only consider vendor upscalers (FSR/DLSS) as "active" when the
	// selected method actually produces a downscale. If the renderer is
	// currently running at 1:1 (no downscale), treat upscaling as inactive.
	if (!(method == UpscaleMethod::kFSR || method == UpscaleMethod::kDLSS)) {
		return false;
	}

	// resolutionScale.x represents renderWidth / displayWidth.
	return resolutionScale.x < .99f;
}

void Upscaling::RequestHistoryReset()
{
	historyResetRequested = true;
}

bool Upscaling::ShouldResetHistoryThisFrame() const
{
	return historyResetThisFrame;
}

void Upscaling::LatchHistoryResetForCurrentFrame()
{
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (historyResetLatchedFrame == frame)
		return;

	historyResetLatchedFrame = frame;
	historyResetThisFrame = historyResetRequested;
	historyResetRequested = false;
}

void Upscaling::UpdateHistoryResetState(UpscaleMethod a_upscaleMethod)
{
	auto state = globals::state;
	if (!state)
		return;

	const bool inWorld = state->inWorld;
	const bool inMapMenu = globals::game::ui ? globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME) : false;
	const float2 screenSize = state->screenSize;
	const bool foveatedDispatchEnabled = IsFoveatedVendorDispatchEnabled(a_upscaleMethod);
	const float foveatedCenterArea = ClampFoveatedCenterArea(settings.foveatedCenterArea);

	auto cameraCutDetected = []() {
		constexpr float kCameraCutDistanceThreshold = 2500.0f;  // ~35m teleport/cut in Skyrim units
		const float cutDistanceSq = kCameraCutDistanceThreshold * kCameraCutDistanceThreshold;

		auto exceededThreshold = [&](uint32_t eyeIndex) {
			const auto& currentPos = globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
			const auto& previousPos = globals::game::frameBufferCached.GetCameraPreviousPosAdjust(eyeIndex);
			const float dx = currentPos.x - previousPos.x;
			const float dy = currentPos.y - previousPos.y;
			const float dz = currentPos.z - previousPos.z;
			return (dx * dx + dy * dy + dz * dz) > cutDistanceSq;
		};

		if (globals::game::isVR)
			return exceededThreshold(0) || exceededThreshold(1);
		return exceededThreshold(0);
	};

	bool shouldReset = false;
	if (!historyResetTrackingInitialized) {
		shouldReset = true;
		historyResetTrackingInitialized = true;
	} else {
		const bool screenSizeChanged =
			std::abs(screenSize.x - previousHistoryScreenSize.x) > 0.5f ||
			std::abs(screenSize.y - previousHistoryScreenSize.y) > 0.5f;
		const bool scaleChanged =
			std::abs(resolutionScale.x - previousHistoryResolutionScale.x) > 1e-4f ||
			std::abs(resolutionScale.y - previousHistoryResolutionScale.y) > 1e-4f;
		const bool worldStateChanged =
			inWorld != previousHistoryInWorld ||
			inMapMenu != previousHistoryInMapMenu;
		const bool methodChanged = a_upscaleMethod != previousHistoryUpscaleMethod;
		const bool compareFoveatedArea = foveatedDispatchEnabled || previousHistoryFoveatedDispatch;
		const bool foveatedChanged =
			foveatedDispatchEnabled != previousHistoryFoveatedDispatch ||
			(compareFoveatedArea && std::abs(foveatedCenterArea - previousHistoryFoveatedCenterArea) > 1e-4f);
		const bool longFrameGap = globals::game::deltaTime &&
								  std::isfinite(*globals::game::deltaTime) &&
								  *globals::game::deltaTime > 0.20f;
		const bool cameraCut = inWorld && cameraCutDetected();

		shouldReset = screenSizeChanged || scaleChanged || worldStateChanged || methodChanged || foveatedChanged || longFrameGap || cameraCut;
	}

	if (state->pendingPostLoadRuntimeReset)
		shouldReset = true;

	if (shouldReset)
		RequestHistoryReset();

	previousHistoryScreenSize = screenSize;
	previousHistoryResolutionScale = resolutionScale;
	previousHistoryInWorld = inWorld;
	previousHistoryInMapMenu = inMapMenu;
	previousHistoryUpscaleMethod = a_upscaleMethod;
	previousHistoryFoveatedDispatch = foveatedDispatchEnabled;
	previousHistoryFoveatedCenterArea = foveatedCenterArea;
}

/**
 * @brief Retrieves the current frame time for frame generation.
 *
 * Returns the frame time from the D3D12 swap chain if frame generation is active; otherwise, returns 0.
 *
 * @return float The current frame time in seconds, or 0 if frame generation is inactive.
 */
float Upscaling::GetFrameGenerationFrameTime() const
{
	if (!IsFrameGenerationActive())
		return 0.0f;

	// Get the current frame time from D3D12 swapchain
	if (dx12SwapChain.swapChain) {
		// Get frame time from the D3D12 SwapChain
		return GetFrameTime();
	}

	return 0.0f;
}

// Unified interface methods
void Upscaling::LoadUpscalingSDKs()
{
	// Initialize upscaling SDK components during plugin startup
	// This ensures all SDKs are available before any D3D device creation
	streamline.LoadInterposer();
	fidelityFX.LoadFFX();  // Only for frame generation now
}

void Upscaling::SetUIBuffer()
{
	dx12SwapChain.SetUIBuffer();
}

HANDLE Upscaling::GetFrameLatencyWaitableObject() const
{
	return dx12SwapChain.GetFrameLatencyWaitableObject();
}

float Upscaling::GetFrameTime() const
{
	return dx12SwapChain.GetFrameTime();
}

// Backend interface methods
bool Upscaling::IsBackendInitialized() const
{
	return streamline.initialized;
}

void Upscaling::CheckBackendFeatures(IDXGIAdapter* adapter)
{
	streamline.CheckFeatures(adapter);
}

void Upscaling::UpgradeBackendInterface(void** ppInterface)
{
	streamline.slUpgradeInterface(ppInterface);
}

void Upscaling::SetBackendD3DDevice(ID3D11Device* device)
{
	streamline.slSetD3DDevice(device);
}

void Upscaling::PostBackendDevice()
{
	streamline.PostDevice();
}

// Module availability methods
bool Upscaling::HasFrameGenModule() const
{
	return fidelityFX.featureFSR3FG;
}

// Proxy interface methods
void Upscaling::SetProxyD3D11Device(ID3D11Device* device)
{
	dx12SwapChain.SetD3D11Device(device);
}

void Upscaling::SetProxyD3D11DeviceContext(ID3D11DeviceContext* context)
{
	dx12SwapChain.SetD3D11DeviceContext(context);
}

void Upscaling::CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChain(adapter, swapChainDesc);
}

void Upscaling::CreateProxyInterop()
{
	dx12SwapChain.CreateInterop();
}

IDXGISwapChain* Upscaling::GetProxySwapChain()
{
	return dx12SwapChain.GetSwapChainProxy();
}

void Upscaling::Upscale()
{
	auto upscaleMethod = GetUpscaleMethod();
	UpdateHistoryResetState(upscaleMethod);
	LatchHistoryResetForCurrentFrame();

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	const bool requiresEncodedMotionVectors = upscaleMethod == UpscaleMethod::kDLSS || upscaleMethod == UpscaleMethod::kFSR;
	const bool requiresCombinedEncodedMotionVectors = requiresEncodedMotionVectors && !globals::game::isVR;
	if (requiresCombinedEncodedMotionVectors && (!motionVectorCopyTexture || !motionVectorCopyTexture->uav || !motionVectorCopyTexture->resource)) {
		logger::error("[Upscaling] Missing encoded motion-vector resources for method {}", magic_enum::enum_name(upscaleMethod));
		return;
	}

	auto dispatchCount = Util::GetScreenDispatchCount(true);
	bool depthPreparedForFoveatedDispatch = false;

	{
		state->BeginPerfEvent("Encode Upscaling Textures");

		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		{
			// Set up upscaling data constant buffer
			auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
			ID3D11ShaderResourceView* views[4] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			auto upscalingBuffer = upscalingDataCB->CB();
			context->CSSetConstantBuffers(0, 1, &upscalingBuffer);
			context->CSSetShader(GetEncodeTexturesCS(), nullptr, 0);

			if (globals::game::isVR) {
				uint32_t eyeWidthOut = (uint32_t)(state->screenSize.x / 2);
				uint32_t eyeHeightOut = (uint32_t)state->screenSize.y;
				uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
				uint32_t eyeHeightIn = (uint32_t)renderSize.y;

				EnsureVRIntermediateTextures(eyeWidthIn, eyeHeightIn, eyeWidthOut, eyeHeightOut,
					main.texture, motionVector.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get());

				for (uint32_t i = 0; i < 2; ++i) {
					UpscalingDataCB upscalingData{};
					upscalingData.dispatchDim = { (float)eyeWidthIn, (float)eyeHeightIn };
					upscalingData.trueSamplingDim = renderSize;
					upscalingData.invTrueSamplingDim = { renderSize.x > 0.0f ? 1.0f / renderSize.x : 0.0f, renderSize.y > 0.0f ? 1.0f / renderSize.y : 0.0f };
					upscalingData.seamCenterX = renderSize.x * 0.5f;
					upscalingData.seamHalfWidthPx = 2.0f;
					upscalingData.maskDepthThreshold = 1e-6f;
					upscalingData.vrSeamHardening = 1.0f;
					upscalingData.sourceOffset = { i == 1 ? (float)eyeWidthIn : 0.0f, 0.0f };

					upscalingDataCB->Update(upscalingData);

					ID3D11UnorderedAccessView* uavs[3] = {
						vrIntermediateReactiveMask[i]->uav.get(),
						vrIntermediateTransparencyMask[i]->uav.get(),
						vrIntermediateMotionVectors[i]->uav.get()
					};
					context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

					auto eyeDispatchX = (eyeWidthIn + 7) / 8;
					auto eyeDispatchY = (eyeHeightIn + 7) / 8;
					context->Dispatch(eyeDispatchX, eyeDispatchY, 1);
				}
			} else {
				UpscalingDataCB upscalingData{};
				upscalingData.dispatchDim = renderSize;
				upscalingData.trueSamplingDim = renderSize;
				upscalingData.invTrueSamplingDim = { renderSize.x > 0.0f ? 1.0f / renderSize.x : 0.0f, renderSize.y > 0.0f ? 1.0f / renderSize.y : 0.0f };
				upscalingData.seamCenterX = renderSize.x * 0.5f;
				upscalingData.seamHalfWidthPx = 2.0f;
				upscalingData.maskDepthThreshold = 1e-6f;
				upscalingData.vrSeamHardening = 0.0f;
				upscalingData.sourceOffset = { 0.0f, 0.0f };
				upscalingDataCB->Update(upscalingData);

				ID3D11UnorderedAccessView* uavs[3] = {
					reactiveMaskTexture->uav.get(),
					transparencyCompositionMaskTexture->uav.get(),
					requiresEncodedMotionVectors ? motionVectorCopyTexture->uav.get() : nullptr
				};
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
			}
		}

		ID3D11ShaderResourceView* views[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);

		state->EndPerfEvent();
	}

	{
		state->BeginPerfEvent("Upscaling");
		ID3D11Resource* motionVectorResource = globals::game::isVR ? motionVector.texture : motionVectorCopyTexture->resource.get();
		const bool foveatedDispatchRequested = IsFoveatedVendorDispatchEnabled(upscaleMethod);
		bool dispatched = false;
		static bool loggedFoveatedFallback = false;

		if (foveatedDispatchRequested) {
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			dispatched = DispatchFoveatedVendorUpscaling(
				upscaleMethod,
				main.texture,
				depth.texture,
				motionVectorResource,
				reactiveMaskTexture->resource.get(),
				transparencyCompositionMaskTexture->resource.get(),
				main.SRV,
				depthPreparedForFoveatedDispatch);
			if (!dispatched) {
				if (!loggedFoveatedFallback) {
					logger::warn("[Upscaling] Foveated vendor dispatch failed; falling back to full-frame {} dispatch.",
						magic_enum::enum_name(upscaleMethod));
					loggedFoveatedFallback = true;
				}
			} else {
				loggedFoveatedFallback = false;
			}
		} else {
			loggedFoveatedFallback = false;
		}

		if (!dispatched) {
			if (upscaleMethod == UpscaleMethod::kDLSS) {
				streamline.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVectorResource);
			} else if (upscaleMethod == UpscaleMethod::kFSR) {
				fidelityFX.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVectorResource, settings.sharpnessFSR);
			}
		}

		state->EndPerfEvent();
	}
}

void Upscaling::PerformUpscaling()
{
	Upscale();
	UpscaleDepth();

	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();

	// Disable dynamic resolution past this point
	runtimeData.dynamicResolutionLock = 1;

	// Updates the PerFrame constant buffer so that dynamic resolution settings are disabled
	UpdateCameraData();
}

void Upscaling::UpscaleDepth()
{
	// Optimization overview:
	// 1) Early validation exits before issuing GPU work.
	// 2) Wide-kernel depth mode uses hysteresis to avoid frequent toggles.
	// 3) Resource copies are skipped for aliased src/dst to reduce copy churn.

	// (1) Early validation exits
	if (!IsUpscalingActive()) {
		return;
	}

	auto state = globals::state;
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto deferred = globals::deferred;
	if (!state || !renderer || !context || !deferred || !deferred->linearSampler || !jitterCB || !upscaleRasterizerState || !upscaleBlendState || !upscaleDepthStencilState) {
		return;
	}

	auto screenSize = state->screenSize;
	if (screenSize.x <= 0.0f || screenSize.y <= 0.0f) {
		return;
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];
	auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
	auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];
	auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

	if (!depth.texture || !depth.views[0] || !depthCopy.texture || !depthCopy.depthSRV ||
		!refractionNormals.texture || !refractionNormals.textureCopy || !refractionNormals.SRVCopy || !refractionNormals.RTV || !saoCameraZ.RTV ||
		!underwaterMask.texture || !underwaterMask.textureCopy || !underwaterMask.SRVCopy || !underwaterMask.RTV) {
		return;
	}
	if (globals::game::isVR && (!depthCopy.views[0] || !depthCopy.stencilSRV)) {
		return;
	}

	auto* fullscreenVS = GetUpscaleVS();
	auto* depthUpscalePS = GetDepthRefractionUpscalePS();
	auto* underwaterMaskPS = GetUnderwaterMaskUpscalePS();
	if (!fullscreenVS || !depthUpscalePS || !underwaterMaskPS) {
		return;
	}

	state->BeginPerfEvent("Render Target Upscaling");

	// Set up Input Assembler for fullscreen triangle (no vertex/index buffers needed)
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set up vertex shader that generates fullscreen triangle using SV_VertexID
	context->VSSetShader(fullscreenVS, nullptr, 0);

	// Set up viewport for fullscreen rendering
	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;
	viewport.Width = screenSize.x;
	viewport.Height = screenSize.y;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	// Set rasterizer and blend state
	context->RSSetState(upscaleRasterizerState.get());
	context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

	ID3D11SamplerState* samplers[] = { deferred->linearSampler };
	context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

	// Set up jitter/depth-kernel constant buffer for upscaling
	JitterCB jitterData;
	jitterData.jitter = jitter;
	// (2) Wide-kernel hysteresis
	{
		constexpr float kEnterWideKernelRatio = 1.55f;
		constexpr float kExitWideKernelRatio = 1.45f;
		const float minScale = std::max(std::min(resolutionScale.x, resolutionScale.y), FLT_EPSILON);
		const float upscaleRatio = 1.0f / minScale;

		if (depthUpscaleUseWideKernel) {
			if (upscaleRatio < kExitWideKernelRatio) {
				depthUpscaleUseWideKernel = false;
			}
		} else {
			if (upscaleRatio > kEnterWideKernelRatio) {
				depthUpscaleUseWideKernel = true;
			}
		}

		jitterData.useWideKernel = depthUpscaleUseWideKernel ? 1.0f : 0.0f;
		jitterData.pad0 = 0.0f;
	}

	jitterCB->Update(jitterData);
	auto bufferArray = jitterCB->CB();
	context->PSSetConstantBuffers(0, 1, &bufferArray);

	// (3) Skip aliased copies
	const auto copyIfNonAliased = [&](ID3D11Resource* dst, ID3D11Resource* src) {
		if (dst && src && dst != src) {
			context->CopyResource(dst, src);
		}
	};

	{
		// Sometimes this is not already copied e.g. map menu.
		// Skip alias copies to reduce unnecessary copy churn.
		copyIfNonAliased(depthCopy.texture, depth.texture);

		// Clear stencil to be 0xFF
		if (globals::game::isVR) {
			context->ClearDepthStencilView(depthCopy.views[0], D3D11_CLEAR_STENCIL, 1.0f, 0xFF);
		}

		// Set depth stencil state to write 0x00
		context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

		copyIfNonAliased(refractionNormals.textureCopy, refractionNormals.texture);

		ID3D11ShaderResourceView* srvs[] = { refractionNormals.SRVCopy, depthCopy.depthSRV, depthCopy.stencilSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { refractionNormals.RTV, saoCameraZ.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, depth.views[0]);

		context->PSSetShader(depthUpscalePS, nullptr, 0);
		context->Draw(3, 0);

		// Depth copy is also used on VR.
		if (globals::game::isVR) {
			copyIfNonAliased(depthCopy.texture, depth.texture);
		}
	}

	{
		viewport.Width = screenSize.x * 0.5f;
		viewport.Height = screenSize.y * 0.5f;
		context->RSSetViewports(1, &viewport);

		copyIfNonAliased(underwaterMask.textureCopy, underwaterMask.texture);

		context->OMSetDepthStencilState(nullptr, 0x00);

		ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy };
		context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(underwaterMaskPS, nullptr, 0);
		context->Draw(3, 0);
	}

	ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);

	state->EndPerfEvent();
}

void Upscaling::ApplySharpening()
{
	if (settings.sharpnessDLSS <= 0.0f)
		return;

	if (!sharpenerTexture)
		return;

	float currentSharpness = (-2.0f * settings.sharpnessDLSS) + 2.0f;
	currentSharpness = exp2(-currentSharpness);

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	ID3D11Resource* mainResource = nullptr;
	main.SRV->GetResource(&mainResource);

	if (!mainResource)
		return;

	context->OMSetRenderTargets(0, nullptr, nullptr);

	rcas.ApplySharpen(main.SRV, sharpenerTexture->uav.get(), currentSharpness);
	context->CopyResource(mainResource, sharpenerTexture->resource.get());

	mainResource->Release();

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	globals::features::upscaling.ConfigureTAA();
	func(a_state);
	globals::features::upscaling.ConfigureUpscaling(a_state);
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	globals::features::upscaling.PostDisplay();
	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.GetUpscaleMethod();

	if (upscaling.d3d12SwapChainActive && upscaling.settings.frameGenerationMode)
		upscaling.CopySharedD3D12Resources();

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA)
		upscaling.PerformUpscaling();

	if (upscaleMethod == UpscaleMethod::kDLSS)
		upscaling.ApplySharpening();

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	if (upscaleMethod == UpscaleMethod::kNONE) {
		// Keep vanilla TAA/water stabilization state untouched when no upscaler is active.
		func(a_this, a3, a_target, a_4, a_5);
		return;
	}

	BSImagespaceShaderISTemporalAA->taaEnabled = upscaleMethod == UpscaleMethod::kTAA;

	func(a_this, a3, a_target, a_4, a_5);

	BSImagespaceShaderISTemporalAA->taaEnabled = false;
}

void Upscaling::SetScissorRect::thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom)
{
	auto viewport = globals::game::graphicsState;
	auto& runtimeData = viewport->GetRuntimeData();

	if (!runtimeData.dynamicResolutionLock) {
		a_left = static_cast<int>(a_left * runtimeData.dynamicResolutionWidthRatio);
		a_right = static_cast<int>(a_right * runtimeData.dynamicResolutionWidthRatio);

		a_top = static_cast<int>(a_top * runtimeData.dynamicResolutionHeightRatio);
		a_bottom = static_cast<int>(a_bottom * runtimeData.dynamicResolutionHeightRatio);
	}

	func(This, a_left, a_top, a_right, a_bottom);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}
