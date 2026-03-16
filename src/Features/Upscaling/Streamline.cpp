#include "Streamline.h"

#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_3.h>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

namespace
{
	constexpr UINT NVIDIA_VENDOR_ID = 0x10DE;
}

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

void Streamline::LoadInterposer()
{
	triedInitialization = true;

	std::wstring interposerPath = std::wstring(Streamline::PluginDir) + L"\\sl.interposer.dll";
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline] Failed to load interposer: Error Code {0:x}", errorCode);
		return;
	} else {
		logger::info("[Streamline] Interposer loaded at address: {0:p}", static_cast<void*>(interposer));
	}

	// Dynamically log all DLL versions in the Streamline plugin directory
	std::filesystem::path pluginDir = std::filesystem::path(Streamline::PluginDir);
	Streamline::dllVersions.clear();
	if (std::filesystem::exists(pluginDir)) {
		for (const auto& entry : std::filesystem::directory_iterator(pluginDir)) {
			if (entry.is_regular_file() && entry.path().extension() == L".dll") {
				const auto& path = entry.path();
				auto version = Util::GetDllVersion(path.c_str());
				auto name = path.filename().string();
				std::string versionStr = version ? Util::GetFormattedVersion(*version) : "Unknown";
				Streamline::dllVersions.emplace_back(name, versionStr);
				if (version)
					logger::info("[Streamline] {} version: {}", name, versionStr);
				else
					logger::info("[Streamline] {} version: Unknown", name);
			}
		}
	} else {
		logger::warn("[Streamline] Plugin directory not found: {}", std::filesystem::absolute(pluginDir).string());
	}

	logger::info("[Streamline] Initializing Streamline");

	sl::Preferences pref;

	sl::Feature featuresToLoad[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature featuresToLoadVR[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };

	pref.featuresToLoad = REL::Module::IsVR() ? featuresToLoadVR : featuresToLoad;
	pref.numFeaturesToLoad = REL::Module::IsVR() ? _countof(featuresToLoadVR) : _countof(featuresToLoad);

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;
	std::error_code pluginPathError;
	auto pluginDirAbsolute = std::filesystem::absolute(std::filesystem::path(Streamline::PluginDir), pluginPathError);
	if (pluginPathError)
		pluginDirAbsolute = std::filesystem::path(Streamline::PluginDir);
	static std::wstring pluginDirAbsoluteW;
	pluginDirAbsoluteW = pluginDirAbsolute.wstring();
	static const wchar_t* pluginPaths[1]{};
	pluginPaths[0] = pluginDirAbsoluteW.c_str();
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	logger::info("[Streamline] Plugin search path: {}", pluginDirAbsolute.string());

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = sl::RenderAPI::eD3D11;
	pref.flags = sl::PreferenceFlags::eUseManualHooking;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline] Failed to initialize Streamline");
	} else {
		initialized = true;
		featureDLSS = false;
		featureReflex = false;
		featurePCL = false;
		reflexSupportedOnCurrentAdapter = false;
		dlssOptionsCache[0] = {};
		dlssOptionsCache[1] = {};
		reflexOptionsCache = {};
		lastReflexSleepFrame = UINT32_MAX;
		logger::info("[Streamline] Successfully initialized Streamline");
	}
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline] Checking features");
	DXGI_ADAPTER_DESC adapterDesc;
	a_adapter->GetDesc(&adapterDesc);
	reflexSupportedOnCurrentAdapter = adapterDesc.VendorId == NVIDIA_VENDOR_ID;

	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeatureAvailability = [&](sl::Feature feature, const char* featureName, bool& outAvailable) {
		outAvailable = false;
		bool loaded = false;
		if (SL_FAILED(result, slIsFeatureLoaded(feature, loaded))) {
			logger::warn("[Streamline] {} load-state query failed: {}", featureName, magic_enum::enum_name(result));
			return;
		}
		if (!loaded) {
			logger::info("[Streamline] {} feature is not loaded", featureName);
			sl::FeatureRequirements featureRequirements;
			sl::Result requirementsResult = slGetFeatureRequirements(feature, featureRequirements);
			if (requirementsResult != sl::Result::eOk) {
				logger::info("[Streamline] {} feature failed to load due to: {}", featureName, magic_enum::enum_name(requirementsResult));
			}
			return;
		}

		logger::info("[Streamline] {} feature is loaded", featureName);
		outAvailable = slIsFeatureSupported(feature, adapterInfo) == sl::Result::eOk;
	};

	checkFeatureAvailability(sl::kFeatureDLSS, "DLSS", featureDLSS);
	if (reflexSupportedOnCurrentAdapter) {
		checkFeatureAvailability(sl::kFeatureReflex, "Reflex", featureReflex);
		checkFeatureAvailability(sl::kFeaturePCL, "PCL", featurePCL);
	} else {
		featureReflex = false;
		featurePCL = false;
	}

	if (featureDLSS) {
		isRTXBelow40series = IsRTXAndBelow40Series(a_adapter);

		if (isRTXBelow40series)
			logger::info("[Streamline] Older RTX GPU detected, DLSS 4.0 will be used instead of DLSS 4.5");
		else
			logger::info("[Streamline] Newer RTX GPU detected, DLSS 4.5 will be used instead of DLSS 4.0");
	}

	logger::info("[Streamline] DLSS {} available", featureDLSS ? "is" : "is not");
	if (reflexSupportedOnCurrentAdapter) {
		logger::info("[Streamline] Reflex {} available", featureReflex ? "is" : "is not");
		logger::info("[Streamline] PCL {} available", featurePCL ? "is" : "is not");
	} else {
		logger::info("[Streamline] Reflex/PCL disabled on non-NVIDIA adapter");
	}
	InvalidateDLSSOptionsCache();
	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

void Streamline::PostDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction

	if (featureDLSS) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	}

	slReflexGetState = nullptr;
	slReflexSleep = nullptr;
	slReflexSetOptions = nullptr;
	slPCLSetMarker = nullptr;
	featureReflex = false;
	featurePCL = false;

	if (slGetFeatureFunction && reflexSupportedOnCurrentAdapter) {
		if (slSetFeatureLoaded) {
			const auto requestFeatureLoad = [&](sl::Feature feature, const char* featureName) {
				const sl::Result loadResult = slSetFeatureLoaded(feature, true);
				if (loadResult != sl::Result::eOk)
					logger::warn("[Streamline] Failed to request {} load: {}", featureName, magic_enum::enum_name(loadResult));
			};

			requestFeatureLoad(sl::kFeatureReflex, "Reflex");
			requestFeatureLoad(sl::kFeaturePCL, "PCL");
		}

		const auto bindFeatureFn = [&](sl::Feature feature, const char* functionName, void*& fn) {
			fn = nullptr;
			const sl::Result bindResult = slGetFeatureFunction(feature, functionName, fn);
			if (bindResult != sl::Result::eOk)
				logger::warn("[Streamline] {} bind failed with {}", functionName, magic_enum::enum_name(bindResult));
			return bindResult == sl::Result::eOk && fn != nullptr;
		};

		bool reflexFnsBound = true;
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
		reflexFnsBound &= bindFeatureFn(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);
		featureReflex = reflexFnsBound && slReflexSetOptions && slReflexSleep;

		if (!featureReflex) {
			logger::warn("[Streamline] Reflex functions are missing; Reflex runtime controls will be disabled");
		} else {
			logger::info("[Streamline] Reflex runtime controls are available");
		}

		slPCLSetMarker = nullptr;
		bool pclFnBound = bindFeatureFn(sl::kFeaturePCL, "slPCLSetMarker", (void*&)slPCLSetMarker);
		featurePCL = pclFnBound && slPCLSetMarker;
		if (!featurePCL) {
			logger::warn("[Streamline] PCL marker function is unavailable; marker optimization requests will be ignored");
		} else {
			logger::info("[Streamline] PCL marker interface is available");
		}
	} else if (!reflexSupportedOnCurrentAdapter) {
		logger::info("[Streamline] Skipping Reflex/PCL binding on non-NVIDIA adapter");
	}

	InvalidateDLSSOptionsCache();
	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

/**
 * @brief Updates and sets camera and frame constants for the current Streamline frame.
 *
 * Populates and submits camera parameters, projection matrices, motion vector settings, and other per-frame constants to the Streamline SDK for the current frame. Uses cached framebuffer data and global state to ensure correct configuration for upscaling and frame generation features.
 */
bool Streamline::EnsureFrameToken()
{
	if (!initialized || !slGetNewFrameToken || !globals::state)
		return false;

	if (!frameChecker.IsNewFrame())
		return frameToken != nullptr;

	if (SL_FAILED(result, slGetNewFrameToken(frameToken, &globals::state->frameCount))) {
		logger::error("[Streamline] Could not get frame token: {}", magic_enum::enum_name(result));
		frameToken = nullptr;
		return false;
	}

	return frameToken != nullptr;
}

void Streamline::CheckFrameConstants(sl::ViewportHandle p_viewport, uint32_t eyeIndex, float viewportScaleX, float viewportScaleY)
{
	if (!globals::features::upscaling.streamline.initialized)
		return;

	if (!EnsureFrameToken())
		return;

	// In VR, we need to set constants for each viewport/eye separately
	// In non-VR, this is called once per frame
	auto state = globals::state;
	float clampedViewportScaleX = std::clamp(viewportScaleX, 1e-4f, 1.0f);
	float clampedViewportScaleY = std::clamp(viewportScaleY, 1e-4f, 1.0f);
	if (!globals::game::isVR) {
		clampedViewportScaleX = 1.0f;
		clampedViewportScaleY = 1.0f;
	}

	sl::Constants slConstants = {};

	// Calculate aspect ratio for the SINGLE EYE
	float eyeWidth = state->screenSize.x * (globals::game::isVR ? 0.5f : 1.0f);
	float eyeHeight = state->screenSize.y;
	slConstants.cameraAspectRatio = (eyeWidth * clampedViewportScaleX) / (eyeHeight * clampedViewportScaleY);

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse(eyeIndex).Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered(eyeIndex).Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust(eyeIndex);
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = sl::Boolean::eFalse;

	if (globals::game::isVR) {
		const bool isCroppedViewport = clampedViewportScaleX < 0.999f || clampedViewportScaleY < 0.999f;
		if (isCroppedViewport) {
			const float invScaleX = 1.0f / clampedViewportScaleX;
			const float invScaleY = 1.0f / clampedViewportScaleY;

			// Match projection to the cropped DLSS viewport so temporal reprojection
			// operates in the same clip space as color/depth/mvec inputs.
			slConstants.cameraViewToClip[0].x *= invScaleX;
			slConstants.cameraViewToClip[0].y *= invScaleX;
			slConstants.cameraViewToClip[0].z *= invScaleX;
			slConstants.cameraViewToClip[0].w *= invScaleX;
			slConstants.cameraViewToClip[1].x *= invScaleY;
			slConstants.cameraViewToClip[1].y *= invScaleY;
			slConstants.cameraViewToClip[1].z *= invScaleY;
			slConstants.cameraViewToClip[1].w *= invScaleY;

			// cameraFOV is vertical; scale by cropped Y region.
			slConstants.cameraFOV = 2.0f * atanf(clampedViewportScaleY * tanf(slConstants.cameraFOV * 0.5f));
		}

		// VR: compute clipToCameraView / clipToPrevClip / prevClipToClip from Skyrim's per-eye matrices.
		// recalculateCameraMatrices() uses a single static prev-frame slot -- unusable for two viewports.
		sl::matrixFullInvert(slConstants.clipToCameraView, slConstants.cameraViewToClip);

		auto currViewProj = globals::game::frameBufferCached.GetCameraViewProjUnjittered(eyeIndex).Transpose();
		auto prevViewProj = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered(eyeIndex).Transpose();

		sl::float4x4 currViewProjSL = *(sl::float4x4*)&currViewProj;
		sl::float4x4 prevViewProjSL = *(sl::float4x4*)&prevViewProj;

		sl::float4x4 invCurrViewProj;
		sl::matrixFullInvert(invCurrViewProj, currViewProjSL);
		sl::matrixMul(slConstants.clipToPrevClip, invCurrViewProj, prevViewProjSL);

		if (isCroppedViewport) {
			const float invScaleX = 1.0f / clampedViewportScaleX;
			const float invScaleY = 1.0f / clampedViewportScaleY;
			const float leftFactors[4] = { clampedViewportScaleX, clampedViewportScaleY, 1.0f, 1.0f };
			const float rightFactors[4] = { invScaleX, invScaleY, 1.0f, 1.0f };

			// Conjugate clipToPrevClip into cropped clip-space basis:
			// CTP_cropped = inv(S) * CTP * S
			float* ctpValues = &slConstants.clipToPrevClip[0].x;
			for (uint32_t row = 0; row < 4; ++row) {
				for (uint32_t col = 0; col < 4; ++col) {
					ctpValues[row * 4 + col] *= leftFactors[row] * rightFactors[col];
				}
			}
		}

		sl::matrixFullInvert(slConstants.prevClipToClip, slConstants.clipToPrevClip);
	} else {
		recalculateCameraMatrices(slConstants);
	}

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };
	const bool requestHistoryReset = upscaling.settings.dlssUseHistoryReset && upscaling.ShouldResetHistoryThisFrame();
	slConstants.reset = requestHistoryReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

	if (globals::game::isVR && (clampedViewportScaleX < 0.999f || clampedViewportScaleY < 0.999f)) {
		slConstants.mvecScale = { 1.0f / clampedViewportScaleX, 1.0f / clampedViewportScaleY };
	} else {
		slConstants.mvecScale = { 1.0f, 1.0f };
	}
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, p_viewport))) {
		logger::error("[Streamline] Could not set constants for eye {}", eyeIndex);
	}
}

bool Streamline::IsRTXAndBelow40Series(IDXGIAdapter* a_adapter)
{
	DXGI_ADAPTER_DESC adapterDesc = {};

	a_adapter->GetDesc(&adapterDesc);

	UINT vendorId = adapterDesc.VendorId;
	UINT deviceId = adapterDesc.DeviceId;

	// Check if NVIDIA
	if (vendorId != 0x10DE)
		return false;

	// RTX 30 series (Ampere) - 0x2200-0x25FF
	if (deviceId >= 0x2200 && deviceId <= 0x2600)
		return true;

	// RTX 20 series (Turing with RT cores) - 0x1E00-0x1FFF
	if (deviceId >= 0x1E00 && deviceId <= 0x1FFF)
		return true;

	return false;
}

void Streamline::SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t eyeIndex, uint32_t width, uint32_t height)
{
	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	uint32_t dlssPreset = std::min(globals::features::upscaling.settings.dlssPreset, Upscaling::kDLSSPresetMaxIndex);

	// Detect HDR from kMAIN format at runtime -- VR kMAIN may be 8-bit while SE is FP16
	bool isHDR = false;
	{
		auto renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
		isHDR = mainDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	uint32_t cacheIndex = globals::game::isVR ? (eyeIndex > 0 ? 1u : 0u) : 0u;
	bool useLegacyProfile = isRTXBelow40series;
	auto& cache = dlssOptionsCache[cacheIndex];
	if (cache.valid &&
		cache.outputWidth == width &&
		cache.outputHeight == height &&
		cache.qualityMode == qualityMode &&
		cache.dlssPreset == dlssPreset &&
		cache.isHDR == isHDR &&
		cache.useLegacyProfile == useLegacyProfile) {
		return;
	}

	sl::DLSSOptions dlssOptions{};
	switch (qualityMode) {
	case 1:
		dlssOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	dlssOptions.outputWidth = width;
	dlssOptions.outputHeight = height;
	dlssOptions.colorBuffersHDR = isHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	sl::DLSSPreset selectedPreset = sl::DLSSPreset::ePresetK;
	switch (dlssPreset) {
	case 0:
		selectedPreset = sl::DLSSPreset::ePresetJ;
		break;
	case 1:
		selectedPreset = sl::DLSSPreset::ePresetK;
		break;
	case 2:
		selectedPreset = sl::DLSSPreset::ePresetL;
		break;
	case 3:
		selectedPreset = sl::DLSSPreset::ePresetM;
		break;
	case 4:
		selectedPreset = sl::DLSSPreset::ePresetF;
		break;
	default:
		selectedPreset = sl::DLSSPreset::ePresetK;
		break;
	}

	dlssOptions.dlaaPreset = selectedPreset;
	dlssOptions.ultraQualityPreset = selectedPreset;
	dlssOptions.qualityPreset = selectedPreset;
	dlssOptions.balancedPreset = selectedPreset;
	dlssOptions.performancePreset = selectedPreset;
	dlssOptions.ultraPerformancePreset = selectedPreset;

	dlssOptions.preExposure = 1.0f;
	dlssOptions.sharpness = 0.0f;

	if (SL_FAILED(result, slDLSSSetOptions(p_viewport, dlssOptions))) {
		logger::critical("[Streamline] Could not enable DLSS");
		cache.valid = false;
		return;
	}

	cache.valid = true;
	cache.outputWidth = width;
	cache.outputHeight = height;
	cache.qualityMode = qualityMode;
	cache.dlssPreset = dlssPreset;
	cache.isHDR = isHDR;
	cache.useLegacyProfile = useLegacyProfile;
}

void Streamline::InvalidateDLSSOptionsCache()
{
	dlssOptionsCache[0] = {};
	dlssOptionsCache[1] = {};
}

bool Streamline::EvaluateDLSS(sl::ViewportHandle vp, uint32_t eyeIndex,
	ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
	ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
	const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth)
{
	auto context = globals::d3d::context;

	sl::Resource colorInRes = { sl::ResourceType::eTex2d, colorIn, 0 };
	sl::Resource colorOutRes = { sl::ResourceType::eTex2d, colorOut, 0 };
	sl::Resource depthRes = { sl::ResourceType::eTex2d, depth, 0 };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, mvec, 0 };
	sl::Resource reactiveMaskRes = { sl::ResourceType::eTex2d, reactiveMask, 0 };
	sl::Resource transparencyMaskRes = { sl::ResourceType::eTex2d, transparencyMask, 0 };

	float viewportScaleX = 1.0f;
	float viewportScaleY = 1.0f;
	if (auto state = globals::state) {
		const float fullOutputWidth = globals::game::isVR ? (state->screenSize.x * 0.5f) : state->screenSize.x;
		const float fullOutputHeight = state->screenSize.y;
		if (fullOutputWidth > 0.0f && fullOutputHeight > 0.0f) {
			viewportScaleX = std::clamp(static_cast<float>(extentOut.width) / fullOutputWidth, 1e-4f, 1.0f);
			viewportScaleY = std::clamp(static_cast<float>(extentOut.height) / fullOutputHeight, 1e-4f, 1.0f);
		}
	}

	CheckFrameConstants(vp, eyeIndex, viewportScaleX, viewportScaleY);
	SetDLSSOptions(vp, eyeIndex, outputWidth, extentOut.height);

	sl::ResourceTag tags[] = {
		{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentOut },
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &reactiveMaskRes, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &transparencyMaskRes, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn }
	};

	slSetTag(vp, tags, _countof(tags), context);

	sl::ViewportHandle view(vp);
	const sl::BaseStructure* inputs[] = { &view };

	auto state = globals::state;
	if (state->frameAnnotations) {
		if (globals::game::isVR) {
			char buf[32];
			snprintf(buf, sizeof(buf), "DLSS Evaluate Eye %u", eyeIndex);
			state->BeginPerfEvent(buf);
		} else {
			state->BeginPerfEvent("DLSS Evaluate");
		}
	}

	sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), context);

	if (state->frameAnnotations)
		state->EndPerfEvent();

	if (evalResult != sl::Result::eOk) {
		static bool evalErrorLogged[2] = { false, false };
		uint32_t logIdx = globals::game::isVR ? eyeIndex : 0;
		if (!evalErrorLogged[logIdx]) {
			evalErrorLogged[logIdx] = true;
			logger::error("[Streamline] slEvaluateFeature failed{} result={}", globals::game::isVR ? std::format(" for eye {}", eyeIndex) : "", (int)evalResult);
		}
	}

	return evalResult == sl::Result::eOk;
}

bool Streamline::UpscaleRegion(uint32_t eyeIndex, ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
	ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
	uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight)
{
	if (!initialized || !featureDLSS || !colorIn || !colorOut || !depth || !mvec || !reactiveMask || !transparencyMask)
		return false;

	sl::ViewportHandle vp = (globals::game::isVR && eyeIndex == 1) ? viewportRight : viewport;
	sl::Extent extentIn{ 0u, 0u, renderWidth, renderHeight };
	sl::Extent extentOut{ 0u, 0u, outputWidth, outputHeight };

	return EvaluateDLSS(vp, eyeIndex, colorIn, colorOut, depth, mvec, reactiveMask, transparencyMask, extentIn, extentOut, outputWidth);
}

void Streamline::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors)
{
	auto state = globals::state;

	auto renderer = globals::game::renderer;
	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// VR: Combined-buffer mode with extent offsets causes temporal ghosting on the right eye
	// because DLSS's internal history buffers use extent offsets as indices.
	// Per-eye isolation with extents at {0,0} is required.
	if (globals::game::isVR) {
		auto& upscaling = globals::features::upscaling;
		uint32_t eyeWidthOut = (uint32_t)(screenSize.x / 2);
		uint32_t eyeHeightOut = (uint32_t)screenSize.y;
		uint32_t eyeWidthIn = (uint32_t)(renderSize.x / 2);
		uint32_t eyeHeightIn = (uint32_t)renderSize.y;

		upscaling.PreparePerEyeInputs(a_upscalingTexture, depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask, false);

		for (uint32_t i = 0; i < 2; ++i) {
			sl::ViewportHandle vp = (i == 1) ? viewportRight : viewport;
			sl::Extent extentIn{ 0, 0, eyeWidthIn, eyeHeightIn };
			sl::Extent extentOut{ 0, 0, eyeWidthOut, eyeHeightOut };

			EvaluateDLSS(vp, i,
				upscaling.vrIntermediateColorIn[i]->resource.get(), upscaling.vrIntermediateColorOut[i]->resource.get(),
				upscaling.vrIntermediateDepth[i]->resource.get(), upscaling.vrIntermediateMotionVectors[i]->resource.get(),
				upscaling.vrIntermediateReactiveMask[i]->resource.get(), upscaling.vrIntermediateTransparencyMask[i]->resource.get(),
				extentIn, extentOut, eyeWidthOut);
		}

		upscaling.FinalizePerEyeOutputs(a_upscalingTexture);
	} else {
		// Non-VR: Simple full-texture upscale
		sl::Extent extentIn{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
		sl::Extent extentOut{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

		EvaluateDLSS(viewport, 0,
			a_upscalingTexture, a_upscalingTexture,
			depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask,
			extentIn, extentOut, (uint)screenSize.x);
	}
}
/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources()
{
	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = sl::DLSSMode::eOff;

	slDLSSSetOptions(viewport, dlssOptions);
	slFreeResources(sl::kFeatureDLSS, viewport);

	if (globals::game::isVR) {
		slDLSSSetOptions(viewportRight, dlssOptions);
		slFreeResources(sl::kFeatureDLSS, viewportRight);
	}

	InvalidateDLSSOptionsCache();
}

void Streamline::UpdateReflex()
{
	if (!initialized || !reflexSupportedOnCurrentAdapter || !featureReflex || !slReflexSetOptions)
		return;

	const auto& upscaling = globals::features::upscaling;
	const bool reflexBlockedByFrameGeneration = upscaling.IsFrameGenerationDx12PathActive();
	if (reflexBlockedByFrameGeneration) {
		const bool reflexAlreadyOff = reflexOptionsCache.valid &&
			reflexOptionsCache.mode == sl::ReflexMode::eOff &&
			reflexOptionsCache.frameLimitUs == 0 &&
			!reflexOptionsCache.useMarkersToOptimize;
		if (!reflexAlreadyOff) {
			sl::ReflexOptions disableOptions{};
			disableOptions.mode = sl::ReflexMode::eOff;
			disableOptions.frameLimitUs = 0;
			disableOptions.useMarkersToOptimize = false;
			if (SL_FAILED(result, slReflexSetOptions(disableOptions))) {
				logger::error("[Streamline] Failed to disable Reflex while Frame Generation is active: {}", magic_enum::enum_name(result));
			} else {
				reflexOptionsCache.valid = true;
				reflexOptionsCache.mode = disableOptions.mode;
				reflexOptionsCache.frameLimitUs = disableOptions.frameLimitUs;
				reflexOptionsCache.useMarkersToOptimize = disableOptions.useMarkersToOptimize;
			}
		}
		lastReflexSleepFrame = UINT32_MAX;
		return;
	}

	auto& settings = globals::features::upscaling.settings;

	sl::ReflexOptions options{};
	if (!settings.reflexLowLatencyMode) {
		options.mode = sl::ReflexMode::eOff;
	} else {
		options.mode = settings.reflexLowLatencyBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
	}

	const float fpsLimit = std::clamp(settings.reflexFPSLimit, 1.0f, 1000.0f);
	options.frameLimitUs = settings.reflexUseFPSLimit ? static_cast<uint32_t>(std::round(1000000.0f / fpsLimit)) : 0u;
	options.useMarkersToOptimize = settings.reflexUseMarkersToOptimize && featurePCL && slPCLSetMarker;

	if (!reflexOptionsCache.valid ||
		reflexOptionsCache.mode != options.mode ||
		reflexOptionsCache.frameLimitUs != options.frameLimitUs ||
		reflexOptionsCache.useMarkersToOptimize != options.useMarkersToOptimize) {
		if (SL_FAILED(result, slReflexSetOptions(options))) {
			logger::error("[Streamline] Failed to apply Reflex options: {}", magic_enum::enum_name(result));
		} else {
			reflexOptionsCache.valid = true;
			reflexOptionsCache.mode = options.mode;
			reflexOptionsCache.frameLimitUs = options.frameLimitUs;
			reflexOptionsCache.useMarkersToOptimize = options.useMarkersToOptimize;
		}
	}

	if (!slReflexSleep)
		return;

	if (options.mode == sl::ReflexMode::eOff && options.frameLimitUs == 0)
		return;

	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	if (lastReflexSleepFrame == currentFrame)
		return;

	if (!EnsureFrameToken())
		return;

	lastReflexSleepFrame = currentFrame;
	if (SL_FAILED(result, slReflexSleep(*frameToken))) {
		logger::warn("[Streamline] Reflex sleep call failed: {}", magic_enum::enum_name(result));
	}
}
