#include "FidelityFX.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <directx/d3dx12.h>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "../../State.h"
#include "../../Utils/FileSystem.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

ffxFunctions ffxModule;

std::vector<std::pair<std::string, std::string>> FidelityFX::dllVersions = {};

namespace
{
	constexpr wchar_t kFrameGenerationDllName[] = L"amd_fidelityfx_framegeneration_dx12.dll";
	constexpr wchar_t kLoaderDllName[] = L"amd_fidelityfx_loader_dx12.dll";
	constexpr wchar_t kUpscalerDllName[] = L"amd_fidelityfx_upscaler_dx12.dll";
	constexpr uint32_t kAmdVendorId = 0x1002u;
	constexpr uint32_t kNvidiaVendorId = 0x10DEu;

	bool UseSplitPerEyeFSRContexts()
	{
		return globals::game::isVR;
	}

	bool TryGetTexture2DDesc(ID3D11Resource* a_resource, D3D11_TEXTURE2D_DESC& a_outDesc)
	{
		if (!a_resource)
			return false;

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			return false;

		texture->GetDesc(&a_outDesc);
		return true;
	}

	bool TryGetCurrentAdapterDesc(DXGI_ADAPTER_DESC& a_outDesc)
	{
		if (!globals::d3d::device)
			return false;

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
			return false;

		winrt::com_ptr<IDXGIAdapter> adapter;
		if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
			return false;

		a_outDesc = {};
		if (FAILED(adapter->GetDesc(&a_outDesc)))
			return false;

		return true;
	}

	std::string ToUpperAscii(std::string a_value)
	{
		std::transform(a_value.begin(), a_value.end(), a_value.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return a_value;
	}

	bool IsLikelyRDNA4Adapter(const DXGI_ADAPTER_DESC& a_desc)
	{
		if (a_desc.VendorId != kAmdVendorId)
			return false;

		std::wstring wideDescription(a_desc.Description);
		const std::string description = ToUpperAscii(stl::utf16_to_utf8(wideDescription).value_or(""));
		if (description.find("RADEON") == std::string::npos)
			return false;

		size_t searchPosition = 0;
		while (searchPosition < description.length()) {
			const size_t rxPosition = description.find("RX", searchPosition);
			if (rxPosition == std::string::npos)
				break;

			size_t modelStart = rxPosition + 2;
			while (modelStart < description.length() && !std::isdigit(static_cast<unsigned char>(description[modelStart])))
				modelStart++;

			size_t modelEnd = modelStart;
			while (modelEnd < description.length() && std::isdigit(static_cast<unsigned char>(description[modelEnd])))
				modelEnd++;

			if (modelEnd > modelStart) {
				const std::string modelText = description.substr(modelStart, modelEnd - modelStart);
				if (!modelText.empty()) {
					char* parseEnd = nullptr;
					const unsigned long modelNumber = std::strtoul(modelText.c_str(), &parseEnd, 10);
					if (parseEnd != modelText.c_str() && modelNumber >= 9000ul)
						return true;
				}
			}

			searchPosition = rxPosition + 2;
		}

		// Keep fallback for abbreviated naming variants that don't include full numeric model text.
		return description.find("RX 90") != std::string::npos ||
		       description.find("RX90") != std::string::npos;
	}

	std::string UpscalerVersionToString(uint32_t a_version)
	{
		const uint32_t major = (a_version >> 22) & 0x3FFu;
		const uint32_t minor = (a_version >> 12) & 0x3FFu;
		const uint32_t patch = a_version & 0xFFFu;
		return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
	}

	void RuntimeFfxMessage(uint32_t a_type, const wchar_t* a_message)
	{
		const std::string message = stl::utf16_to_utf8(a_message ? a_message : L"").value_or("unknown FidelityFX runtime message");
		if (a_type == FFX_API_MESSAGE_TYPE_ERROR) {
			logger::error("[FidelityFX] {}", message);
		} else {
			logger::warn("[FidelityFX] {}", message);
		}
	}

	D3D11_TEXTURE2D_DESC MakeSharedTextureDesc(const D3D11_TEXTURE2D_DESC& a_sourceDesc, uint32_t a_width, uint32_t a_height, UINT a_bindFlags)
	{
		D3D11_TEXTURE2D_DESC desc = a_sourceDesc;
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.BindFlags = a_bindFlags;
		desc.MiscFlags = 0;
		return desc;
	}

	bool SameTextureDesc(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
	{
		return a_left.Width == a_right.Width &&
		       a_left.Height == a_right.Height &&
		       a_left.MipLevels == a_right.MipLevels &&
		       a_left.ArraySize == a_right.ArraySize &&
		       a_left.Format == a_right.Format &&
		       a_left.SampleDesc.Count == a_right.SampleDesc.Count &&
		       a_left.SampleDesc.Quality == a_right.SampleDesc.Quality &&
		       a_left.Usage == a_right.Usage &&
		       a_left.BindFlags == a_right.BindFlags &&
		       a_left.CPUAccessFlags == a_right.CPUAccessFlags &&
		       a_left.MiscFlags == a_right.MiscFlags;
	}

	template <size_t N>
	void DeleteWrappedResourceArray(WrappedResource* (&a_resources)[N])
	{
		for (auto*& resource : a_resources) {
			delete resource;
			resource = nullptr;
		}
	}

	bool DispatchHostFsr3UpscaleProtected(FfxFsr3Context& a_context, FfxFsr3DispatchUpscaleDescription& a_dispatchParameters, bool& a_crashed)
	{
		a_crashed = false;
		bool dispatchOk = true;

		__try {
			dispatchOk = ffxFsr3ContextDispatchUpscale(&a_context, &a_dispatchParameters) == FFX_OK;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_crashed = true;
			dispatchOk = false;
		}

		return dispatchOk;
	}
}

void FidelityFX::LoadFFX()
{
	const std::filesystem::path pluginDir = std::filesystem::path(FidelityFX::PluginDir);

	ResetRuntimeUpscalerTracking(true);

	const std::filesystem::path framegenPath = pluginDir / kFrameGenerationDllName;
	const std::filesystem::path loaderPath = pluginDir / kLoaderDllName;
	const std::filesystem::path upscalerPath = pluginDir / kUpscalerDllName;

	const bool framegenDllExists = std::filesystem::exists(framegenPath);
	const bool upscalerDllExists = std::filesystem::exists(upscalerPath);
	DWORD framegenLoadError = ERROR_SUCCESS;
	DWORD upscalerLoadError = ERROR_SUCCESS;
	DWORD loaderLoadError = ERROR_SUCCESS;

	if (!module) {
		module = LoadLibraryW(loaderPath.c_str());
		if (!module)
			loaderLoadError = GetLastError();
	}
	if (!frameGenerationModule && framegenDllExists) {
		frameGenerationModule = LoadLibraryW(framegenPath.c_str());
		if (!frameGenerationModule)
			framegenLoadError = GetLastError();
	}
	if (!runtimeUpscalerModule && upscalerDllExists) {
		runtimeUpscalerModule = LoadLibraryW(upscalerPath.c_str());
		if (!runtimeUpscalerModule)
			upscalerLoadError = GetLastError();
	}

	featureFSR3FG = frameGenerationModule != nullptr;
	featureFSR4Upscaler = runtimeUpscalerModule != nullptr;

	FidelityFX::dllVersions = Util::EnumerateDllVersions(pluginDir);

	if (module) {
		ffxLoadFunctions(&ffxModule, module);
		logger::info("[FidelityFX] Loader DLL loaded successfully from plugin directory");
	} else {
		logger::error("[FidelityFX] Failed to load {} from plugin directory (Win32 error {})",
			stl::utf16_to_utf8(kLoaderDllName).value_or("loader DLL"),
			loaderLoadError);
	}

	if (featureFSR3FG) {
		logger::info("[FidelityFX] Frame generation DLL loaded and available");
	} else if (framegenDllExists) {
		logger::warn("[FidelityFX] Frame generation DLL found but failed to load (Win32 error {}) - FSR3 frame generation disabled",
			framegenLoadError);
	} else {
		logger::warn("[FidelityFX] Frame generation DLL not found - FSR3 frame generation disabled");
	}

	if (featureFSR4Upscaler) {
		logger::info("[FidelityFX] Runtime upscaler DLL loaded; runtime availability will be verified during context creation");
	} else if (upscalerDllExists) {
		logger::warn("[FidelityFX] Runtime upscaler DLL found but failed to load (Win32 error {}) - FSR4 runtime path disabled",
			upscalerLoadError);
	} else {
		logger::warn("[FidelityFX] Runtime upscaler DLL not found - FSR4 runtime path disabled");
	}
}

bool FidelityFX::HasRuntimeUpscalerSupportCheckResult() const
{
	return runtimeUpscalerSupportCheckKnown;
}

bool FidelityFX::IsRuntimeUpscalerSupportConfirmed() const
{
	return runtimeUpscalerSupportCheckKnown && runtimeUpscalerSupportConfirmed;
}

bool FidelityFX::IsRuntimeUpscalerFailureLatched() const
{
	return runtimeUpscalerFailureLatched;
}

const char* FidelityFX::GetRuntimeUpscalerLastFramePathLabel() const
{
	if (!runtimeUpscalerLastFramePathValid)
		return "Pending FSR dispatch";

	switch (runtimeUpscalerLastFramePath) {
	case RuntimeUpscalerFramePath::kHostFsr31:
		return "Host FSR 3.1";
	case RuntimeUpscalerFramePath::kRuntimeFsr4:
		return "Runtime FSR4";
	case RuntimeUpscalerFramePath::kHostFsr31Fallback:
		return "Host FSR 3.1 fallback";
	case RuntimeUpscalerFramePath::kInactive:
	default:
		return "Pending FSR dispatch";
	}
}

std::string FidelityFX::GetRuntimeUpscalerProviderName() const
{
	return runtimeUpscalerProviderMatchedVersionName;
}

std::string FidelityFX::GetRuntimeUpscalerRequestedVersionString() const
{
	return UpscalerVersionToString(FFX_UPSCALER_VERSION);
}

void FidelityFX::ResetRuntimeUpscalerTracking(bool a_invalidateProviderCache)
{
	runtimeUpscalerFailureLatched = false;
	runtimeFallbackResetDispatchesRemaining = 0;
	runtimeUpscalerLastFramePathValid = false;
	runtimeUpscalerLastFrameIndex = 0;
	runtimeUpscalerLastFramePath = RuntimeUpscalerFramePath::kInactive;

	if (!a_invalidateProviderCache)
		return;

	runtimeUpscalerSupportCheckKnown = false;
	runtimeUpscalerSupportConfirmed = false;
	runtimeUpscalerProviderMatchedVersionId = 0;
	runtimeUpscalerProviderMatchedVersionName.clear();
}

void FidelityFX::LatchRuntimeUpscalerFailure()
{
	if (runtimeUpscalerFailureLatched)
		return;

	runtimeUpscalerFailureLatched = true;
	logger::warn("[FidelityFX] Runtime upscaler path latched off after failure; using host FSR3.1 until FSR resources are rebuilt or the method changes.");
}

void FidelityFX::RecordRuntimeUpscalerFramePath(RuntimeUpscalerFramePath a_path)
{
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (!runtimeUpscalerLastFramePathValid || runtimeUpscalerLastFrameIndex != frame) {
		runtimeUpscalerLastFramePathValid = true;
		runtimeUpscalerLastFrameIndex = frame;
		runtimeUpscalerLastFramePath = a_path;
		return;
	}

	if (static_cast<uint8_t>(a_path) > static_cast<uint8_t>(runtimeUpscalerLastFramePath))
		runtimeUpscalerLastFramePath = a_path;
}

void FidelityFX::SetupFrameGeneration()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	ffx::CreateContextDescFrameGeneration createFg{};
	createFg.displaySize = { swapChain.swapChainDesc.Width, swapChain.swapChainDesc.Height };
	createFg.maxRenderSize = createFg.displaySize;
	createFg.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
	createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(swapChain.swapChainDesc.Format);

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = swapChain.d3d12Device.get();

	if (ffx::CreateContext(frameGenContext, nullptr, createFg, backendDesc) != ffx::ReturnCode::Ok)
		logger::critical("[FidelityFX] Failed to create frame generation context!");
}

void FidelityFX::Present(bool a_useFrameGeneration)
{
	auto& upscaling = globals::features::upscaling;
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	ffx::ConfigureDescFrameGeneration configParameters{};

	if (a_useFrameGeneration) {
		configParameters.frameGenerationEnabled = true;

		configParameters.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* pUserCtx) -> ffxReturnCode_t {
			return ffxModule.Dispatch(reinterpret_cast<ffxContext*>(pUserCtx), &params->header);
		};

		configParameters.frameGenerationCallbackUserContext = &frameGenContext;
	} else {
		configParameters.frameGenerationEnabled = false;
		configParameters.frameGenerationCallbackUserContext = nullptr;
		configParameters.frameGenerationCallback = nullptr;
	}

	configParameters.HUDLessColor = FfxApiResource({});
	configParameters.presentCallback = nullptr;
	configParameters.presentCallbackUserContext = nullptr;

	static uint64_t frameID = 0;
	configParameters.frameID = frameID;
	configParameters.swapChain = swapChain.swapChain;
	configParameters.onlyPresentGenerated = false;
	configParameters.flags = 0;
	configParameters.allowAsyncWorkloads = true;

	auto state = globals::state;
	auto renderSize = state->screenSize * upscaling.resolutionScale;

	configParameters.generationRect.left = 0;
	configParameters.generationRect.top = 0;
	configParameters.generationRect.width = swapChain.swapChainDesc.Width;
	configParameters.generationRect.height = swapChain.swapChainDesc.Height;

	if (ffx::Configure(frameGenContext, configParameters) != ffx::ReturnCode::Ok)
		logger::critical("[FidelityFX] Failed to configure frame generation!");

	ffx::ConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiConfig{};
	uiConfig.uiResource = ffxApiGetResourceDX12(swapChain.uiBufferWrapped->resource.get());
	uiConfig.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA | FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;

	if (ffx::Configure(swapChainContext, uiConfig) != ffx::ReturnCode::Ok)
		logger::critical("[FidelityFX] Failed to configure UI composition!");

	if (a_useFrameGeneration) {
		ffx::DispatchDescFrameGenerationPrepare dispatchParameters{};

		dispatchParameters.commandList = swapChain.commandLists[swapChain.frameIndex].get();
		dispatchParameters.motionVectorScale.x = renderSize.x;
		dispatchParameters.motionVectorScale.y = renderSize.y;
		dispatchParameters.renderSize.width = static_cast<uint32_t>(renderSize.x);
		dispatchParameters.renderSize.height = static_cast<uint32_t>(renderSize.y);
		dispatchParameters.jitterOffset.x = -upscaling.jitter.x;
		dispatchParameters.jitterOffset.y = -upscaling.jitter.y;
		dispatchParameters.frameTimeDelta = RE::GetSecondsSinceLastFrame() * 1000.f;
		dispatchParameters.cameraFar = *globals::game::cameraFar;
		dispatchParameters.cameraNear = *globals::game::cameraNear;
		dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
		dispatchParameters.frameID = frameID;
		dispatchParameters.depth = ffxApiGetResourceDX12(swapChain.depthBufferShared12->resource.get());
		dispatchParameters.motionVectors = ffxApiGetResourceDX12(swapChain.motionVectorBufferShared12->resource.get());

		ffx::DispatchDescFrameGenerationPrepareCameraInfo cameraConfig{};

		auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();

		cameraConfig.cameraRight[0] = viewMatrix._11;
		cameraConfig.cameraRight[1] = viewMatrix._12;
		cameraConfig.cameraRight[2] = viewMatrix._13;

		cameraConfig.cameraUp[0] = viewMatrix._21;
		cameraConfig.cameraUp[1] = viewMatrix._22;
		cameraConfig.cameraUp[2] = viewMatrix._23;

		cameraConfig.cameraForward[0] = viewMatrix._31;
		cameraConfig.cameraForward[1] = viewMatrix._32;
		cameraConfig.cameraForward[2] = viewMatrix._33;

		cameraConfig.cameraPosition[0] = globals::game::frameBufferCached.GetCameraPosAdjust().x;
		cameraConfig.cameraPosition[1] = globals::game::frameBufferCached.GetCameraPosAdjust().y;
		cameraConfig.cameraPosition[2] = globals::game::frameBufferCached.GetCameraPosAdjust().z;

		if (ffx::Dispatch(frameGenContext, dispatchParameters, cameraConfig) != ffx::ReturnCode::Ok)
			logger::critical("[FidelityFX] Failed to dispatch frame generation!");
	}

	frameID++;
	isFrameGenActive = a_useFrameGeneration;
}

void FidelityFX::CreateFSRResources()
{
	auto state = globals::state;
	if (!state) {
		logger::critical("[FidelityFX] Missing global state when creating FSR resources.");
		fsrContextCount = 0;
		return;
	}

	const bool splitPerEyeContexts = UseSplitPerEyeFSRContexts();

	DestroyRuntimeUpscalerContexts();
	DestroyRuntimeUpscalerResources();

	if (fsrScratchBuffer) {
		logger::warn("[FidelityFX] FSR resources already created, skipping allocation");
		return;
	}

	ResetRuntimeUpscalerTracking(true);

	auto fsrDevice = ffxGetDeviceDX11(globals::d3d::device);

	const uint32_t numContexts = splitPerEyeContexts ? 2u : 1u;
	const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(numContexts);
	fsrScratchBuffer = calloc(scratchBufferSize, 1);
	if (!fsrScratchBuffer) {
		logger::critical("[FidelityFX] Failed to allocate FSR3 scratch buffer memory!");
		fsrContextCount = 0;
		return;
	}
	memset(fsrScratchBuffer, 0, scratchBufferSize);

	FfxInterface fsrInterface{};
	if (ffxGetInterfaceDX11(&fsrInterface, fsrDevice, fsrScratchBuffer, scratchBufferSize, numContexts) != FFX_OK) {
		logger::critical("[FidelityFX] Failed to initialize FSR3 backend interface!");
		free(fsrScratchBuffer);
		fsrScratchBuffer = nullptr;
		fsrContextCount = 0;
		return;
	}

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	const uint32_t displayWidth = static_cast<uint32_t>(splitPerEyeContexts ? screenSize.x / 2 : screenSize.x);
	const uint32_t displayHeight = static_cast<uint32_t>(screenSize.y);
	const uint32_t renderWidth = static_cast<uint32_t>(splitPerEyeContexts ? renderSize.x / 2 : renderSize.x);
	const uint32_t renderHeight = static_cast<uint32_t>(renderSize.y);

	for (uint32_t i = 0; i < numContexts; ++i) {
		FfxFsr3ContextDescription contextDescription{};
		contextDescription.maxRenderSize.width = renderWidth;
		contextDescription.maxRenderSize.height = renderHeight;
		contextDescription.maxUpscaleSize.width = displayWidth;
		contextDescription.maxUpscaleSize.height = displayHeight;
		contextDescription.displaySize.width = displayWidth;
		contextDescription.displaySize.height = displayHeight;
		contextDescription.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_AUTO_EXPOSURE | FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE;
		contextDescription.backendInterfaceUpscaling = fsrInterface;

		if (ffxFsr3ContextCreate(&fsrContext[i], &contextDescription) != FFX_OK) {
			logger::critical("[FidelityFX] Failed to initialize FSR3 context for eye {}!", i);
			for (uint32_t j = 0; j < i; ++j)
				ffxFsr3ContextDestroy(&fsrContext[j]);
			free(fsrScratchBuffer);
			fsrScratchBuffer = nullptr;
			fsrContextCount = 0;
			return;
		}
	}

	fsrContextCount = numContexts;
	logger::info("[FidelityFX] Created {} FSR3 contexts (Display: {}x{}, Render: {}x{}, SplitPerEye={})",
		numContexts, displayWidth, displayHeight, renderWidth, renderHeight, splitPerEyeContexts);
}

void FidelityFX::DestroyRuntimeUpscalerContexts()
{
	for (uint32_t i = 0; i < std::size(runtimeUpscalerContexts); ++i) {
		if (runtimeUpscalerContexts[i] && ffx::DestroyContext(runtimeUpscalerContexts[i]) != ffx::ReturnCode::Ok)
			logger::warn("[FidelityFX] Failed to destroy runtime upscaler context {} cleanly.", i);
		runtimeUpscalerContexts[i] = nullptr;
	}

	runtimeUpscalerContextCount = 0;
	runtimeUpscalerMaxRenderWidth = 0;
	runtimeUpscalerMaxRenderHeight = 0;
	runtimeUpscalerMaxDisplayWidth = 0;
	runtimeUpscalerMaxDisplayHeight = 0;
}

void FidelityFX::DestroyRuntimeUpscalerResources()
{
	DeleteWrappedResourceArray(runtimeColorShared);
	DeleteWrappedResourceArray(runtimeDepthShared);
	DeleteWrappedResourceArray(runtimeMotionShared);
	DeleteWrappedResourceArray(runtimeReactiveShared);
	DeleteWrappedResourceArray(runtimeTransparencyShared);
	DeleteWrappedResourceArray(runtimeOutputShared);

	runtimeColorSharedDesc = {};
	runtimeDepthSharedDesc = {};
	runtimeMotionSharedDesc = {};
	runtimeReactiveSharedDesc = {};
	runtimeTransparencySharedDesc = {};
	runtimeOutputSharedDesc = {};
}

void FidelityFX::DestroyFSRResources()
{
	const uint32_t numContexts = fsrContextCount;
	if (numContexts == 0 && fsrScratchBuffer)
		logger::warn("[FidelityFX] DestroyFSRResources called with unknown context count; skipping context destruction to avoid mismatched teardown.");
	for (uint32_t i = 0; i < numContexts; ++i) {
		if (ffxFsr3ContextDestroy(&fsrContext[i]) != FFX_OK)
			logger::critical("[FidelityFX] Failed to destroy FSR3 context for eye {}!", i);
	}
	fsrContextCount = 0;

	if (fsrScratchBuffer) {
		free(fsrScratchBuffer);
		fsrScratchBuffer = nullptr;
	}

	DestroyRuntimeUpscalerContexts();
	DestroyRuntimeUpscalerResources();

	runtimeD3D11Fence = nullptr;
	runtimeD3D12Fence = nullptr;
	runtimeFenceValue = 1;
	runtimeCommandFrameIndex = 0;
	fsrDispatchCrashLogged = false;
	ResetRuntimeUpscalerTracking(true);
}

bool FidelityFX::IsAmdAdapterDetected() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (TryGetCurrentAdapterDesc(adapterDesc))
		return adapterDesc.VendorId == kAmdVendorId;

	return false;
}

bool FidelityFX::IsNvidiaAdapterDetected() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (TryGetCurrentAdapterDesc(adapterDesc))
		return adapterDesc.VendorId == kNvidiaVendorId;

	return false;
}

bool FidelityFX::IsRuntimeUpscalerPresent() const
{
	if (!featureFSR4Upscaler || !runtimeUpscalerModule || !module)
		return false;
	if (!ffxModule.CreateContext || !ffxModule.DestroyContext || !ffxModule.Dispatch || !ffxModule.Query)
		return false;

	return true;
}

bool FidelityFX::IsRuntimeUpscalerAutoEligible() const
{
	DXGI_ADAPTER_DESC adapterDesc{};
	if (!TryGetCurrentAdapterDesc(adapterDesc))
		return false;

	return adapterDesc.VendorId == kAmdVendorId && IsLikelyRDNA4Adapter(adapterDesc);
}

bool FidelityFX::IsRuntimeUpscalerAvailable() const
{
	if (!IsRuntimeUpscalerPresent())
		return false;

	DXGI_ADAPTER_DESC adapterDesc{};
	if (!TryGetCurrentAdapterDesc(adapterDesc))
		return false;
	if (adapterDesc.VendorId == kAmdVendorId) {
		const bool autoDetectedFsr4ClassGpu = IsLikelyRDNA4Adapter(adapterDesc);
		if (!autoDetectedFsr4ClassGpu &&
		    !globals::features::upscaling.settings.fsr4AllowNonRx90Amd) {
			return false;
		}
		return true;
	}
	if (adapterDesc.VendorId == kNvidiaVendorId)
		return false;

	return false;
}

FfxResource ffxGetResource(ID3D11Resource* dx11Resource,
	[[maybe_unused]] wchar_t const* ffxResName,
	FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
{
	FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
	resource.state = state;
	resource.description = GetFfxResourceDescriptionDX11(dx11Resource);

#ifdef _DEBUG
	if (ffxResName) {
		wcscpy_s(resource.name, ffxResName);
	}
#endif

	return resource;
}

bool FidelityFX::CanUseRuntimeUpscalerPath()
{
	if (!globals::features::upscaling.settings.fsr4RuntimeEnable)
		return false;
	if (runtimeUpscalerFailureLatched)
		return false;
	return true;
}

bool FidelityFX::EnsureRuntimeUpscalerInterop()
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	if (!globals::d3d::device || !globals::d3d::context)
		return false;

	try {
		if (!swapChain.d3d11Device)
			swapChain.SetD3D11Device(globals::d3d::device);
		if (!swapChain.d3d11Context)
			swapChain.SetD3D11DeviceContext(globals::d3d::context);

		if (!swapChain.d3d12Device) {
			winrt::com_ptr<IDXGIDevice> dxgiDevice;
			DX::ThrowIfFailed(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));

			winrt::com_ptr<IDXGIAdapter> adapter;
			DX::ThrowIfFailed(dxgiDevice->GetAdapter(adapter.put()));
			swapChain.CreateD3D12Device(adapter.get());
		}

		if (!runtimeD3D12Fence || !runtimeD3D11Fence) {
			winrt::handle sharedFenceHandle;
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&runtimeD3D12Fence)));
			DX::ThrowIfFailed(swapChain.d3d12Device->CreateSharedHandle(runtimeD3D12Fence.get(), nullptr, GENERIC_ALL, nullptr, sharedFenceHandle.put()));
			DX::ThrowIfFailed(swapChain.d3d11Device->OpenSharedFence(sharedFenceHandle.get(), IID_PPV_ARGS(&runtimeD3D11Fence)));
			runtimeFenceValue = 1;
			runtimeCommandFrameIndex = 0;
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Failed to initialize DX11->DX12 runtime interop: {}", e.what());
		return false;
	} catch (...) {
		logger::error("[FidelityFX] Failed to initialize DX11->DX12 runtime interop.");
		return false;
	}

	return swapChain.d3d11Device.get() &&
	       swapChain.d3d11Context.get() &&
	       swapChain.d3d12Device.get() &&
	       swapChain.commandQueue.get() &&
	       swapChain.commandAllocators[0].get() &&
	       swapChain.commandAllocators[1].get() &&
	       swapChain.commandLists[0].get() &&
	       swapChain.commandLists[1].get() &&
	       runtimeD3D11Fence.get() &&
	       runtimeD3D12Fence.get();
}

bool FidelityFX::EnsureRuntimeUpscalerContexts(uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight, uint32_t a_contextCount)
{
	auto recordRuntimeProviderResult = [&](bool a_supported) {
		runtimeUpscalerSupportCheckKnown = true;
		runtimeUpscalerSupportConfirmed = a_supported;
		runtimeUpscalerProviderMatchedVersionId = 0;
		runtimeUpscalerProviderMatchedVersionName.clear();

		if (!a_supported || !runtimeUpscalerContexts[0] || !ffxModule.Query)
			return;

		ffxQueryGetProviderVersion providerQuery{};
		providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		providerQuery.header.pNext = nullptr;
		providerQuery.versionId = 0;
		providerQuery.versionName = nullptr;

		if (ffxModule.Query(&runtimeUpscalerContexts[0], &providerQuery.header) == FFX_API_RETURN_OK) {
			runtimeUpscalerProviderMatchedVersionId = providerQuery.versionId;
			if (providerQuery.versionName)
				runtimeUpscalerProviderMatchedVersionName = providerQuery.versionName;
		}
	};

	if (!a_fullRenderWidth || !a_fullRenderHeight || !a_fullDisplayWidth || !a_fullDisplayHeight) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (a_contextCount == 0 || a_contextCount > std::size(runtimeUpscalerContexts)) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (!EnsureRuntimeUpscalerInterop()) {
		recordRuntimeProviderResult(false);
		return false;
	}
	if (!ffxModule.CreateContext) {
		recordRuntimeProviderResult(false);
		return false;
	}

	bool allContextsValid = true;
	for (uint32_t i = 0; i < a_contextCount; ++i) {
		if (!runtimeUpscalerContexts[i]) {
			allContextsValid = false;
			break;
		}
	}

	const bool needsRecreate =
		!allContextsValid ||
		runtimeUpscalerContextCount != a_contextCount ||
		runtimeUpscalerMaxRenderWidth != a_fullRenderWidth ||
		runtimeUpscalerMaxRenderHeight != a_fullRenderHeight ||
		runtimeUpscalerMaxDisplayWidth != a_fullDisplayWidth ||
		runtimeUpscalerMaxDisplayHeight != a_fullDisplayHeight;

	if (!needsRecreate && runtimeUpscalerContextCount == a_contextCount)
		return true;

	DestroyRuntimeUpscalerContexts();

	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	ffx::CreateBackendDX12Desc backendDesc{};
	backendDesc.device = swapChain.d3d12Device.get();

	bool createdContextWithoutVersionOverride = false;

	for (uint32_t i = 0; i < a_contextCount; ++i) {
		ffx::CreateContextDescUpscale createDesc{};
		createDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		createDesc.maxRenderSize = { a_fullRenderWidth, a_fullRenderHeight };
		createDesc.maxUpscaleSize = { a_fullDisplayWidth, a_fullDisplayHeight };
		createDesc.fpMessage = RuntimeFfxMessage;

		ffx::CreateContextDescUpscaleVersion versionDesc{};
		versionDesc.version = FFX_UPSCALER_VERSION;

		createDesc.header.pNext = &versionDesc.header;
		versionDesc.header.pNext = &backendDesc.header;
		backendDesc.header.pNext = nullptr;

		const auto versionedCreateResult = ffxModule.CreateContext(&runtimeUpscalerContexts[i], &createDesc.header, nullptr);
		auto createResult = versionedCreateResult;
		bool retriedWithoutVersionOverride = false;
		if (createResult == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE ||
		    createResult == FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE) {
			retriedWithoutVersionOverride = true;
			createDesc.header.pNext = &backendDesc.header;
			backendDesc.header.pNext = nullptr;

			const auto retryResult = ffxModule.CreateContext(&runtimeUpscalerContexts[i], &createDesc.header, nullptr);
			if (retryResult == FFX_API_RETURN_OK) {
				createdContextWithoutVersionOverride = true;
			}
			createResult = retryResult;
		}

		if (createResult != FFX_API_RETURN_OK) {
			if (retriedWithoutVersionOverride) {
				logger::error("[FidelityFX] Failed to create runtime upscaler context {} for FSR version {}. Explicit-version create code {}, no-version retry code {} (Render: {}x{}, Display: {}x{}).",
					i,
					UpscalerVersionToString(FFX_UPSCALER_VERSION),
					static_cast<uint32_t>(versionedCreateResult),
					static_cast<uint32_t>(createResult),
					a_fullRenderWidth,
					a_fullRenderHeight,
					a_fullDisplayWidth,
					a_fullDisplayHeight);
			} else {
				logger::error("[FidelityFX] Failed to create runtime upscaler context {} for FSR version {} with code {} (Render: {}x{}, Display: {}x{}).",
					i,
					UpscalerVersionToString(FFX_UPSCALER_VERSION),
					static_cast<uint32_t>(createResult),
					a_fullRenderWidth,
					a_fullRenderHeight,
					a_fullDisplayWidth,
					a_fullDisplayHeight);
			}
			DestroyRuntimeUpscalerContexts();
			recordRuntimeProviderResult(false);
			return false;
		}
	}

	runtimeUpscalerContextCount = a_contextCount;
	runtimeUpscalerMaxRenderWidth = a_fullRenderWidth;
	runtimeUpscalerMaxRenderHeight = a_fullRenderHeight;
	runtimeUpscalerMaxDisplayWidth = a_fullDisplayWidth;
	runtimeUpscalerMaxDisplayHeight = a_fullDisplayHeight;
	recordRuntimeProviderResult(true);

	if (createdContextWithoutVersionOverride) {
		logger::warn("[FidelityFX] Runtime upscaler context creation succeeded only without the explicit FSR version descriptor; runtime SDK/provider ignored or does not support explicit version override.");
	}

	if (runtimeUpscalerProviderMatchedVersionName.empty()) {
		logger::info("[FidelityFX] Created {} runtime upscaler context(s) for FSR version {} (Render: {}x{}, Display: {}x{}).",
			a_contextCount,
			UpscalerVersionToString(FFX_UPSCALER_VERSION),
			a_fullRenderWidth,
			a_fullRenderHeight,
			a_fullDisplayWidth,
			a_fullDisplayHeight);
	} else {
		logger::info("[FidelityFX] Created {} runtime upscaler context(s) using provider '{}' (id 0x{:X}) for FSR version {} (Render: {}x{}, Display: {}x{}).",
			a_contextCount,
			runtimeUpscalerProviderMatchedVersionName,
			runtimeUpscalerProviderMatchedVersionId,
			UpscalerVersionToString(FFX_UPSCALER_VERSION),
			a_fullRenderWidth,
			a_fullRenderHeight,
			a_fullDisplayWidth,
			a_fullDisplayHeight);
	}
	return true;
}

bool FidelityFX::EnsureRuntimeUpscalerSharedResources(uint32_t a_contextCount, uint32_t a_fullRenderWidth, uint32_t a_fullRenderHeight, uint32_t a_fullDisplayWidth, uint32_t a_fullDisplayHeight,
	const D3D11_TEXTURE2D_DESC& a_colorDesc,
	const D3D11_TEXTURE2D_DESC& a_depthDesc,
	const D3D11_TEXTURE2D_DESC& a_motionDesc,
	const D3D11_TEXTURE2D_DESC& a_reactiveDesc,
	const D3D11_TEXTURE2D_DESC& a_transparencyDesc,
	const D3D11_TEXTURE2D_DESC& a_outputDesc)
{
	if (!EnsureRuntimeUpscalerInterop())
		return false;
	if (a_contextCount == 0 || a_contextCount > std::size(runtimeColorShared))
		return false;

	const D3D11_TEXTURE2D_DESC desiredColorDesc = MakeSharedTextureDesc(a_colorDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredDepthDesc = MakeSharedTextureDesc(a_depthDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredMotionDesc = MakeSharedTextureDesc(a_motionDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredReactiveDesc = MakeSharedTextureDesc(a_reactiveDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredTransparencyDesc = MakeSharedTextureDesc(a_transparencyDesc, a_fullRenderWidth, a_fullRenderHeight, 0);
	const D3D11_TEXTURE2D_DESC desiredOutputDesc = MakeSharedTextureDesc(a_outputDesc, a_fullDisplayWidth, a_fullDisplayHeight, D3D11_BIND_UNORDERED_ACCESS);

	bool missingRequiredResource = false;
	for (uint32_t i = 0; i < a_contextCount; ++i) {
		if (!runtimeColorShared[i] ||
			!runtimeDepthShared[i] ||
			!runtimeMotionShared[i] ||
			!runtimeReactiveShared[i] ||
			!runtimeTransparencyShared[i] ||
			!runtimeOutputShared[i] ||
			!runtimeColorShared[i]->resource11 ||
			!runtimeDepthShared[i]->resource11 ||
			!runtimeMotionShared[i]->resource11 ||
			!runtimeReactiveShared[i]->resource11 ||
			!runtimeTransparencyShared[i]->resource11 ||
			!runtimeOutputShared[i]->resource11 ||
			!runtimeColorShared[i]->resource.get() ||
			!runtimeDepthShared[i]->resource.get() ||
			!runtimeMotionShared[i]->resource.get() ||
			!runtimeReactiveShared[i]->resource.get() ||
			!runtimeTransparencyShared[i]->resource.get() ||
			!runtimeOutputShared[i]->resource.get()) {
			missingRequiredResource = true;
			break;
		}
	}

	const bool needsRecreate =
		missingRequiredResource ||
		!SameTextureDesc(runtimeColorSharedDesc, desiredColorDesc) ||
		!SameTextureDesc(runtimeDepthSharedDesc, desiredDepthDesc) ||
		!SameTextureDesc(runtimeMotionSharedDesc, desiredMotionDesc) ||
		!SameTextureDesc(runtimeReactiveSharedDesc, desiredReactiveDesc) ||
		!SameTextureDesc(runtimeTransparencySharedDesc, desiredTransparencyDesc) ||
		!SameTextureDesc(runtimeOutputSharedDesc, desiredOutputDesc);

	if (!needsRecreate) {
		for (uint32_t i = a_contextCount; i < std::size(runtimeColorShared); ++i) {
			delete runtimeColorShared[i];
			runtimeColorShared[i] = nullptr;
			delete runtimeDepthShared[i];
			runtimeDepthShared[i] = nullptr;
			delete runtimeMotionShared[i];
			runtimeMotionShared[i] = nullptr;
			delete runtimeReactiveShared[i];
			runtimeReactiveShared[i] = nullptr;
			delete runtimeTransparencyShared[i];
			runtimeTransparencyShared[i] = nullptr;
			delete runtimeOutputShared[i];
			runtimeOutputShared[i] = nullptr;
		}
		return true;
	}

	DestroyRuntimeUpscalerResources();

	auto& swapChain = globals::features::upscaling.dx12SwapChain;

	try {
		for (uint32_t i = 0; i < a_contextCount; ++i) {
			runtimeColorShared[i] = new WrappedResource(desiredColorDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeDepthShared[i] = new WrappedResource(desiredDepthDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeMotionShared[i] = new WrappedResource(desiredMotionDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeReactiveShared[i] = new WrappedResource(desiredReactiveDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeTransparencyShared[i] = new WrappedResource(desiredTransparencyDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
			runtimeOutputShared[i] = new WrappedResource(desiredOutputDesc, swapChain.d3d11Device.get(), swapChain.d3d12Device.get());
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Failed to create runtime shared resources: {}", e.what());
		DestroyRuntimeUpscalerResources();
		return false;
	} catch (...) {
		logger::error("[FidelityFX] Failed to create runtime shared resources.");
		DestroyRuntimeUpscalerResources();
		return false;
	}

	runtimeColorSharedDesc = desiredColorDesc;
	runtimeDepthSharedDesc = desiredDepthDesc;
	runtimeMotionSharedDesc = desiredMotionDesc;
	runtimeReactiveSharedDesc = desiredReactiveDesc;
	runtimeTransparencySharedDesc = desiredTransparencyDesc;
	runtimeOutputSharedDesc = desiredOutputDesc;

	return true;
}

bool FidelityFX::DispatchRuntimeUpscalerSingle(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness)
{
	if (a_contextIndex >= runtimeUpscalerContextCount || !runtimeUpscalerContexts[a_contextIndex])
		return false;
	if (!a_color || !a_depth || !a_motionVectors || !a_reactiveMask || !a_transparencyCompositionMask || !a_output)
		return false;
	if (!a_renderWidth || !a_renderHeight || !a_displayWidth || !a_displayHeight)
		return false;

	D3D11_TEXTURE2D_DESC colorDesc{};
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	D3D11_TEXTURE2D_DESC reactiveDesc{};
	D3D11_TEXTURE2D_DESC transparencyDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	if (!TryGetTexture2DDesc(a_color, colorDesc) ||
		!TryGetTexture2DDesc(a_depth, depthDesc) ||
		!TryGetTexture2DDesc(a_motionVectors, motionDesc) ||
		!TryGetTexture2DDesc(a_reactiveMask, reactiveDesc) ||
		!TryGetTexture2DDesc(a_transparencyCompositionMask, transparencyDesc) ||
		!TryGetTexture2DDesc(a_output, outputDesc)) {
		return false;
	}

	if (!EnsureRuntimeUpscalerSharedResources(
			runtimeUpscalerContextCount,
			runtimeUpscalerMaxRenderWidth,
			runtimeUpscalerMaxRenderHeight,
			runtimeUpscalerMaxDisplayWidth,
			runtimeUpscalerMaxDisplayHeight,
			colorDesc,
			depthDesc,
			motionDesc,
			reactiveDesc,
			transparencyDesc,
			outputDesc)) {
		return false;
	}

	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	auto& upscaling = globals::features::upscaling;
	auto state = globals::state;

	if (!swapChain.d3d11Context || !swapChain.commandQueue || !runtimeD3D11Fence || !runtimeD3D12Fence)
		return false;

	auto isValidShared = [](WrappedResource* a_resource) {
		return a_resource && a_resource->resource11 && a_resource->resource.get();
	};
	if (!isValidShared(runtimeColorShared[a_contextIndex]) ||
		!isValidShared(runtimeDepthShared[a_contextIndex]) ||
		!isValidShared(runtimeMotionShared[a_contextIndex]) ||
		!isValidShared(runtimeReactiveShared[a_contextIndex]) ||
		!isValidShared(runtimeTransparencyShared[a_contextIndex]) ||
		!isValidShared(runtimeOutputShared[a_contextIndex])) {
		return false;
	}

	const uint32_t commandIndex = runtimeCommandFrameIndex % std::size(swapChain.commandLists);
	runtimeCommandFrameIndex++;

	auto* commandAllocator = swapChain.commandAllocators[commandIndex].get();
	auto* commandList = swapChain.commandLists[commandIndex].get();
	if (!commandAllocator || !commandList)
		return false;

	const bool annotateDispatch = state && state->frameAnnotations;
	if (annotateDispatch) {
		if (globals::game::isVR) {
			char buf[32];
			snprintf(buf, sizeof(buf), "FSR4 Dispatch Eye %u", a_contextIndex);
			state->BeginPerfEvent(buf);
		} else {
			state->BeginPerfEvent("FSR4 Dispatch");
		}
	}

	bool dispatchOk = false;
	try {
		auto copyIntoShared = [&](ID3D11Resource* a_source, WrappedResource* a_destination, uint32_t a_width, uint32_t a_height, uint32_t a_maxWidth, uint32_t a_maxHeight) {
			if (!a_source || !a_destination || !a_destination->resource11)
				return false;

			const uint32_t copyWidth = std::min(a_width, a_maxWidth);
			const uint32_t copyHeight = std::min(a_height, a_maxHeight);
			if (!copyWidth || !copyHeight)
				return false;

			D3D11_BOX sourceBox{};
			sourceBox.left = 0;
			sourceBox.top = 0;
			sourceBox.front = 0;
			sourceBox.right = copyWidth;
			sourceBox.bottom = copyHeight;
			sourceBox.back = 1;
			swapChain.d3d11Context->CopySubresourceRegion(a_destination->resource11, 0, 0, 0, 0, a_source, 0, &sourceBox);
			return true;
		};

		if (!copyIntoShared(a_color, runtimeColorShared[a_contextIndex], colorDesc.Width, colorDesc.Height, runtimeColorSharedDesc.Width, runtimeColorSharedDesc.Height) ||
			!copyIntoShared(a_depth, runtimeDepthShared[a_contextIndex], depthDesc.Width, depthDesc.Height, runtimeDepthSharedDesc.Width, runtimeDepthSharedDesc.Height) ||
			!copyIntoShared(a_motionVectors, runtimeMotionShared[a_contextIndex], motionDesc.Width, motionDesc.Height, runtimeMotionSharedDesc.Width, runtimeMotionSharedDesc.Height) ||
			!copyIntoShared(a_reactiveMask, runtimeReactiveShared[a_contextIndex], reactiveDesc.Width, reactiveDesc.Height, runtimeReactiveSharedDesc.Width, runtimeReactiveSharedDesc.Height) ||
			!copyIntoShared(a_transparencyCompositionMask, runtimeTransparencyShared[a_contextIndex], transparencyDesc.Width, transparencyDesc.Height, runtimeTransparencySharedDesc.Width, runtimeTransparencySharedDesc.Height)) {
			dispatchOk = false;
		} else {
			const uint64_t d3d11SubmitFence = runtimeFenceValue++;
			DX::ThrowIfFailed(swapChain.d3d11Context->Signal(runtimeD3D11Fence.get(), d3d11SubmitFence));
			DX::ThrowIfFailed(swapChain.commandQueue->Wait(runtimeD3D12Fence.get(), d3d11SubmitFence));

			DX::ThrowIfFailed(commandAllocator->Reset());
			DX::ThrowIfFailed(commandList->Reset(commandAllocator, nullptr));

			std::array<D3D12_RESOURCE_BARRIER, 6> beginBarriers = {
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeColorShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepthShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotionShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactiveShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparencyShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutputShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			};
			commandList->ResourceBarrier(static_cast<UINT>(beginBarriers.size()), beginBarriers.data());

			ffx::DispatchDescUpscale dispatchParameters{};
			dispatchParameters.commandList = commandList;
			dispatchParameters.color = ffxApiGetResourceDX12(runtimeColorShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.depth = ffxApiGetResourceDX12(runtimeDepthShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.motionVectors = ffxApiGetResourceDX12(runtimeMotionShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.exposure = FfxApiResource({});
			dispatchParameters.reactive = ffxApiGetResourceDX12(runtimeReactiveShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.transparencyAndComposition = ffxApiGetResourceDX12(runtimeTransparencyShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
			dispatchParameters.output = ffxApiGetResourceDX12(runtimeOutputShared[a_contextIndex]->resource.get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
			dispatchParameters.jitterOffset = { -upscaling.jitter.x, -upscaling.jitter.y };
			dispatchParameters.motionVectorScale = { a_motionVectorScaleX, a_motionVectorScaleY };
			dispatchParameters.renderSize = { a_renderWidth, a_renderHeight };
			dispatchParameters.upscaleSize = { a_displayWidth, a_displayHeight };
			dispatchParameters.enableSharpening = true;
			dispatchParameters.sharpness = a_sharpness;
			dispatchParameters.frameTimeDelta = *globals::game::deltaTime * 1000.f;
			dispatchParameters.preExposure = 1.0f;
			dispatchParameters.reset = upscaling.ShouldResetHistoryThisFrame();
			dispatchParameters.cameraNear = *globals::game::cameraNear;
			dispatchParameters.cameraFar = *globals::game::cameraFar;
			dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
			dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
			dispatchParameters.flags = 0;

			dispatchOk = ffx::Dispatch(runtimeUpscalerContexts[a_contextIndex], dispatchParameters) == ffx::ReturnCode::Ok;
			if (!dispatchOk) {
				logger::error("[FidelityFX] Runtime upscaler dispatch failed for eye {}.", a_contextIndex);
			}

			std::array<D3D12_RESOURCE_BARRIER, 6> endBarriers = {
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeColorShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeDepthShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeMotionShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeReactiveShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeTransparencyShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				CD3DX12_RESOURCE_BARRIER::Transition(runtimeOutputShared[a_contextIndex]->resource.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
			};
			commandList->ResourceBarrier(static_cast<UINT>(endBarriers.size()), endBarriers.data());

			DX::ThrowIfFailed(commandList->Close());

			ID3D12CommandList* commandListsToExecute[] = { commandList };
			swapChain.commandQueue->ExecuteCommandLists(1, commandListsToExecute);

			const uint64_t d3d12SubmitFence = runtimeFenceValue++;
			DX::ThrowIfFailed(swapChain.commandQueue->Signal(runtimeD3D12Fence.get(), d3d12SubmitFence));
			DX::ThrowIfFailed(swapChain.d3d11Context->Wait(runtimeD3D11Fence.get(), d3d12SubmitFence));

			if (dispatchOk) {
				const uint32_t copyWidth = std::min({ a_displayWidth, outputDesc.Width, runtimeOutputSharedDesc.Width });
				const uint32_t copyHeight = std::min({ a_displayHeight, outputDesc.Height, runtimeOutputSharedDesc.Height });
				if (!copyWidth || !copyHeight) {
					dispatchOk = false;
				} else {
					D3D11_BOX outputBox{};
					outputBox.left = 0;
					outputBox.top = 0;
					outputBox.front = 0;
					outputBox.right = copyWidth;
					outputBox.bottom = copyHeight;
					outputBox.back = 1;
					swapChain.d3d11Context->CopySubresourceRegion(a_output, 0, 0, 0, 0, runtimeOutputShared[a_contextIndex]->resource11, 0, &outputBox);
				}
			}
		}
	} catch (const std::exception& e) {
		logger::error("[FidelityFX] Runtime upscaler dispatch path failed for eye {}: {}", a_contextIndex, e.what());
		dispatchOk = false;
	} catch (...) {
		logger::error("[FidelityFX] Runtime upscaler dispatch path failed for eye {}.", a_contextIndex);
		dispatchOk = false;
	}

	if (annotateDispatch)
		state->EndPerfEvent();

	return dispatchOk;
}

bool FidelityFX::UpscaleRegion(uint32_t a_contextIndex, ID3D11Resource* a_color, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_output,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_displayWidth, uint32_t a_displayHeight,
	float a_motionVectorScaleX, float a_motionVectorScaleY, float a_sharpness)
{
	if (!a_color || !a_depth || !a_motionVectors || !a_reactiveMask || !a_transparencyCompositionMask || !a_output ||
		!a_renderWidth || !a_renderHeight || !a_displayWidth || !a_displayHeight) {
		return false;
	}

	const bool runtimeRequested =
		globals::features::upscaling.settings.fsr4RuntimeEnable &&
		IsRuntimeUpscalerAvailable();
	const uint32_t runtimeContextCount = UseSplitPerEyeFSRContexts() ? 2u : 1u;
	const bool runtimeSelected = runtimeRequested && CanUseRuntimeUpscalerPath();

	if (runtimeSelected) {
		auto state = globals::state;
		if (!state)
			return false;

		const auto renderSize = Util::ConvertToDynamic(state->screenSize);
		const bool splitPerEyeContexts = UseSplitPerEyeFSRContexts();
		const uint32_t fullRenderWidth = static_cast<uint32_t>(splitPerEyeContexts ? renderSize.x / 2.0f : renderSize.x);
		const uint32_t fullRenderHeight = static_cast<uint32_t>(renderSize.y);
		const uint32_t fullDisplayWidth = static_cast<uint32_t>(splitPerEyeContexts ? state->screenSize.x / 2.0f : state->screenSize.x);
		const uint32_t fullDisplayHeight = static_cast<uint32_t>(state->screenSize.y);

		if (EnsureRuntimeUpscalerContexts(fullRenderWidth, fullRenderHeight, fullDisplayWidth, fullDisplayHeight, runtimeContextCount)) {
			try {
				if (DispatchRuntimeUpscalerSingle(
						a_contextIndex,
						a_color,
						a_depth,
						a_motionVectors,
						a_reactiveMask,
						a_transparencyCompositionMask,
						a_output,
						a_renderWidth,
						a_renderHeight,
						a_displayWidth,
						a_displayHeight,
						a_motionVectorScaleX,
						a_motionVectorScaleY,
						a_sharpness)) {
					RecordRuntimeUpscalerFramePath(RuntimeUpscalerFramePath::kRuntimeFsr4);
					return true;
				}
			} catch (const std::exception& e) {
				logger::error("[FidelityFX] Runtime upscaler dispatch threw an exception: {}", e.what());
			} catch (...) {
				logger::error("[FidelityFX] Runtime upscaler dispatch threw an unknown exception.");
			}
		}

		if (!runtimeUpscalerFailureLatched) {
			runtimeFallbackResetDispatchesRemaining = std::max(runtimeFallbackResetDispatchesRemaining, runtimeContextCount);
		}
		LatchRuntimeUpscalerFailure();
	}

	if (!runtimeRequested)
		runtimeFallbackResetDispatchesRemaining = 0;

	if (!fsrScratchBuffer || a_contextIndex >= fsrContextCount)
		return false;

	auto context = globals::d3d::context;
	auto state = globals::state;
	if (!context || !state)
		return false;

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	const auto fallbackFramePath =
		runtimeRequested ? RuntimeUpscalerFramePath::kHostFsr31Fallback : RuntimeUpscalerFramePath::kHostFsr31;
	RecordRuntimeUpscalerFramePath(fallbackFramePath);

	if (state->frameAnnotations) {
		if (globals::game::isVR) {
			char buf[32];
			snprintf(buf, sizeof(buf), "FSR Dispatch Eye %u", a_contextIndex);
			state->BeginPerfEvent(buf);
		} else {
			state->BeginPerfEvent("FSR Dispatch");
		}
	}

	FfxFsr3DispatchUpscaleDescription dispatchParameters{};
	dispatchParameters.commandList = ffxGetCommandListDX11(context);
	dispatchParameters.color = ffxGetResource(a_color, L"FSR3_Input_OutputColor");
	dispatchParameters.depth = ffxGetResource(a_depth, L"FSR3_InputDepth");
	dispatchParameters.motionVectors = ffxGetResource(a_motionVectors, L"FSR3_InputMotionVectors");
	dispatchParameters.exposure = ffxGetResource(nullptr, L"FSR3_InputExposure");
	dispatchParameters.upscaleOutput = ffxGetResource(a_output, L"FSR3_OutputColor");
	dispatchParameters.reactive = ffxGetResource(a_reactiveMask, L"FSR3_InputReactiveMap");
	dispatchParameters.transparencyAndComposition = ffxGetResource(a_transparencyCompositionMask, L"FSR3_TransparencyAndCompositionMap");
	dispatchParameters.motionVectorScale.x = a_motionVectorScaleX;
	dispatchParameters.motionVectorScale.y = a_motionVectorScaleY;
	dispatchParameters.renderSize.width = a_renderWidth;
	dispatchParameters.renderSize.height = a_renderHeight;
	dispatchParameters.jitterOffset.x = -jitter.x;
	dispatchParameters.jitterOffset.y = -jitter.y;
	dispatchParameters.frameTimeDelta = *globals::game::deltaTime * 1000.f;
	dispatchParameters.cameraFar = *globals::game::cameraFar;
	dispatchParameters.cameraNear = *globals::game::cameraNear;
	dispatchParameters.enableSharpening = true;
	dispatchParameters.sharpness = a_sharpness;
	dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
	dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;
	const bool runtimeFallbackReset = runtimeRequested && runtimeFallbackResetDispatchesRemaining > 0;
	if (runtimeFallbackReset)
		runtimeFallbackResetDispatchesRemaining--;
	dispatchParameters.reset = globals::features::upscaling.ShouldResetHistoryThisFrame() || runtimeFallbackReset;
	dispatchParameters.preExposure = 1.0f;
	dispatchParameters.flags = 0;

	bool hostDispatchCrashed = false;
	const bool dispatchOK = DispatchHostFsr3UpscaleProtected(fsrContext[a_contextIndex], dispatchParameters, hostDispatchCrashed);
	if (!dispatchOK && !hostDispatchCrashed) {
		logger::critical("[FidelityFX] Failed to dispatch region upscaling for eye {}!", a_contextIndex);
	}
	if (hostDispatchCrashed) {
		if (!fsrDispatchCrashLogged) {
			logger::critical("[FidelityFX] Region FSR3 dispatch crashed for eye {} - this may be caused by RenderDoc capture interfering with FSR operations. Try disabling RenderDoc capture.", a_contextIndex);
			fsrDispatchCrashLogged = true;
		}
	}

	if (state->frameAnnotations)
		state->EndPerfEvent();

	return dispatchOK;
}

void FidelityFX::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors, float a_sharpness)
{
	auto renderer = globals::game::renderer;
	auto state = globals::state;
	if (!renderer || !state)
		return;

	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	const auto screenSize = state->screenSize;
	const auto renderSize = Util::ConvertToDynamic(screenSize);

	auto& upscaling = globals::features::upscaling;
	const bool splitPerEyeContexts = UseSplitPerEyeFSRContexts();

	if (splitPerEyeContexts) {
		upscaling.PreparePerEyeInputs(a_upscalingTexture, depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask, false);

		const uint32_t eyeDisplayWidth = static_cast<uint32_t>(screenSize.x / 2.0f);
		const uint32_t eyeDisplayHeight = static_cast<uint32_t>(screenSize.y);
		const uint32_t eyeRenderWidth = static_cast<uint32_t>(renderSize.x / 2.0f);
		const uint32_t eyeRenderHeight = static_cast<uint32_t>(renderSize.y);

		for (uint32_t i = 0; i < 2; ++i) {
			if (!UpscaleRegion(
					i,
					upscaling.vrIntermediateColorIn[i]->resource.get(),
					upscaling.vrIntermediateDepth[i]->resource.get(),
					upscaling.vrIntermediateMotionVectors[i]->resource.get(),
					upscaling.vrIntermediateReactiveMask[i]->resource.get(),
					upscaling.vrIntermediateTransparencyMask[i]->resource.get(),
					upscaling.vrIntermediateColorOut[i]->resource.get(),
					eyeRenderWidth,
					eyeRenderHeight,
					eyeDisplayWidth,
					eyeDisplayHeight,
					renderSize.x / 2.0f,
					renderSize.y,
					a_sharpness)) {
				logger::error("[FidelityFX] Upscale dispatch failed for VR eye {}.", i);
			}
		}

		upscaling.FinalizePerEyeOutputs(a_upscalingTexture);
		return;
	}

	if (!UpscaleRegion(
			0,
			a_upscalingTexture,
			depthTexture.texture,
			a_motionVectors,
			a_reactiveMask,
			a_transparencyCompositionMask,
			a_upscalingTexture,
			static_cast<uint32_t>(renderSize.x),
			static_cast<uint32_t>(renderSize.y),
			static_cast<uint32_t>(screenSize.x),
			static_cast<uint32_t>(screenSize.y),
			renderSize.x,
			renderSize.y,
			a_sharpness)) {
		logger::error("[FidelityFX] Upscale dispatch failed.");
	}
}
