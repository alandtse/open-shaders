#include "Runtime.h"

#include "Utils/WinApi.h"

#include <detours/detours.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>

namespace NR
{
	namespace
	{
		using Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
		using Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
		using Allocate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using Destroy = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using Populate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using Evaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using Release = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using Identity = unsigned int(NVSDK_CONV*)();

		struct ModuleDeleter
		{
			void operator()(HMODULE module) const { FreeLibrary(module); }
		};
		using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;
		template <class T>
		T Resolve(HMODULE module, const char* name)
		{
			auto function = GetProcAddress(module, name);
			if (!function)
				throw std::runtime_error(std::format("Missing NGX export {}", name));
			return reinterpret_cast<T>(function);
		}

		void Check(NVSDK_NGX_Result result, const char* operation)
		{
			if (NVSDK_NGX_FAILED(result))
				throw std::runtime_error(std::format("{} failed: NGX 0x{:08X}", operation, static_cast<uint32_t>(result)));
		}

		// The NR import checks this synthetic caller identity during scoped NGX calls.
		class RuntimePath
		{
		public:
			void Install(HMODULE runtime, const std::filesystem::path& callerIdentity)
			{
				spoofedCallerPath = callerIdentity.wstring();
				caller = DetourGetContainingModule(reinterpret_cast<void*>(&Proxy));
				DetourEnumerateImportsEx(runtime, this, nullptr, [](void* data, DWORD, const char* name, void** function) -> BOOL {
					if (name && std::strcmp(name, "GetModuleFileNameW") == 0)
						static_cast<RuntimePath*>(data)->slot = function;
					return TRUE;
				});
				if (!caller || !slot)
					throw std::runtime_error("NR caller-path import unavailable");
				original = reinterpret_cast<decltype(original)>(*slot);
				DWORD protection;
				if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
					winrt::throw_last_error();
				InterlockedExchangePointer(slot, reinterpret_cast<void*>(&Proxy));
				DWORD ignored;
				VirtualProtect(slot, sizeof(*slot), protection, &ignored);
				installed = true;
			}
			~RuntimePath()
			{
				if (!installed)
					return;
				DWORD protection;
				if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
					InterlockedExchangePointer(slot, reinterpret_cast<void*>(original));
					DWORD ignored;
					VirtualProtect(slot, sizeof(*slot), protection, &ignored);
				}
			}
			struct Scope
			{
				explicit Scope(RuntimePath& path) : previous(active) { active = &path; }
				~Scope() { active = previous; }
				Scope(const Scope&) = delete;
				Scope& operator=(const Scope&) = delete;
				RuntimePath* previous;
			};

		private:
			static inline thread_local RuntimePath* active = nullptr;
			static inline decltype(&GetModuleFileNameW) original = &GetModuleFileNameW;
			HMODULE caller = nullptr;
			std::wstring spoofedCallerPath;
			void** slot = nullptr;
			bool installed = false;
			static DWORD WINAPI Proxy(HMODULE module, LPWSTR filename, DWORD size)
			{
				if (!active || module != active->caller || !filename || !size)
					return original(module, filename, size);
				const auto length = static_cast<DWORD>(active->spoofedCallerPath.size());
				const auto count = std::min(length, size - 1);
				std::memcpy(filename, active->spoofedCallerPath.data(), count * sizeof(wchar_t));
				filename[count] = L'\0';
				if (count < length)
					SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return count < length ? size : length;
			}
		};
	}

	struct Runtime::Impl
	{
		Module module, core;
		RuntimePath compatibility;
		winrt::com_ptr<ID3D12Device> device;
		Init initialize = nullptr;
		Shutdown shutdown = nullptr;
		Allocate allocate = nullptr;
		Destroy destroy = nullptr;
		Populate populate = nullptr;
		Create create = nullptr;
		NR::Evaluate evaluate = nullptr;
		Release release = nullptr;
		bool initialized = false;
		struct FeatureDeleter
		{
			Impl* owner = nullptr;
			void operator()(NVSDK_NGX_Handle* handle) const
			{
				RuntimePath::Scope scope(owner->compatibility);
				const auto result = owner->release(handle);
				if (NVSDK_NGX_FAILED(result))
					logger::warn("[NeuralRendering] Feature release failed: 0x{:08X}", static_cast<uint32_t>(result));
			}
		};
		struct ParameterDeleter
		{
			Impl* owner = nullptr;
			void operator()(NVSDK_NGX_Parameter* parameters) const { owner->destroy(parameters); }
		};
		struct Eye
		{
			std::unique_ptr<NVSDK_NGX_Parameter, ParameterDeleter> parameters{ nullptr, {} };
			std::unique_ptr<NVSDK_NGX_Handle, FeatureDeleter> feature{ nullptr, {} };
		};
		std::array<Eye, 2> eyes;

		~Impl()
		{
			for (auto& eye : eyes) {
				eye.feature.reset();
				eye.parameters.reset();
			}
			if (initialized) {
				RuntimePath::Scope scope(compatibility);
				shutdown(device.get());
			}
		}
	};

	Runtime::Runtime() : impl(std::make_unique<Impl>()) {}
	Runtime::~Runtime() = default;

	void Runtime::Initialize(ID3D12Device* device, const std::filesystem::path& directory)
	{
		if (impl->initialized)
			return;
		auto pending = std::make_unique<Impl>();
		auto& state = *pending;
		const auto path = directory / L"nvngx_dlssnr.dll";
		const auto version = Util::GetDllVersion(path.wstring());
		if (!version || version->major() != 310 || version->minor() != 8)
			throw std::runtime_error(std::format("Install nvngx_dlssnr.dll 310.8.x in {}", directory.string()));
		state.module.reset(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
		if (!state.module)
			winrt::throw_last_error();
		state.initialize = Resolve<Init>(state.module.get(), "NVSDK_NGX_D3D12_Init_Ext");
		state.shutdown = Resolve<Shutdown>(state.module.get(), "NVSDK_NGX_D3D12_Shutdown1");
		state.create = Resolve<Create>(state.module.get(), "NVSDK_NGX_D3D12_CreateFeature");
		state.evaluate = Resolve<NR::Evaluate>(state.module.get(), "NVSDK_NGX_D3D12_EvaluateFeature");
		state.release = Resolve<Release>(state.module.get(), "NVSDK_NGX_D3D12_ReleaseFeature");
		state.populate = Resolve<Populate>(state.module.get(), "NVSDK_NGX_D3D12_PopulateParameters_Impl");
		const auto appId = Resolve<Identity>(state.module.get(), "NVSDK_NGX_GetApplicationId")();
		const auto api = Resolve<Identity>(state.module.get(), "NVSDK_NGX_GetAPIVersion")();
		// This is an NGX caller identity string only; nvngx.dll need not exist on disk.
		const auto spoofedCallerIdentity = directory / L"nvngx.dll";
		state.compatibility.Install(state.module.get(), spoofedCallerIdentity);
		const auto cache = std::filesystem::temp_directory_path() / L"OpenShaders-NGX";
		std::filesystem::create_directories(cache);
		state.device.copy_from(device);
		{
			RuntimePath::Scope scope(state.compatibility);
			Check(state.initialize(appId, cache.c_str(), device, static_cast<NVSDK_NGX_Version>(api), nullptr), "NR initialization");
			state.initialized = true;
		}
		for (HMODULE module = DetourEnumerateModules(nullptr); module; module = DetourEnumerateModules(module)) {
			auto allocate = GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters");
			auto destroy = GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters");
			if (!allocate || !destroy)
				continue;
			HMODULE retained = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(allocate), &retained))
				winrt::throw_last_error();
			state.core.reset(retained);
			state.allocate = reinterpret_cast<Allocate>(allocate);
			state.destroy = reinterpret_cast<Destroy>(destroy);
			break;
		}
		if (!state.core)
			throw std::runtime_error("NGX parameter API unavailable");
		for (auto& eye : state.eyes) {
			NVSDK_NGX_Parameter* parameters = nullptr;
			const auto result = state.allocate(&parameters);
			eye.parameters = { parameters, { &state } };
			eye.feature = { nullptr, { &state } };
			Check(result, "NR parameter allocation");
			if (!parameters)
				throw std::runtime_error("NGX returned null parameters");
		}
		logger::info("[NeuralRendering] Feature 18 runtime initialized ({})", version->string());
		impl = std::move(pending);
	}

	void Runtime::ResetFeatures()
	{
		for (auto& eye : impl->eyes)
			eye.feature.reset();
	}

	bool Runtime::Evaluate(ID3D12GraphicsCommandList* commands, uint32_t eyeIndex,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
		uint32_t width, uint32_t height, uint32_t guideWidth, uint32_t guideHeight, FrameParameters& frame, const Tuning& tuning)
	{
		auto& state = *impl;
		auto& eye = state.eyes.at(eyeIndex);
		frame.created = false;
		frame.result = 0;
		auto* parameters = eye.parameters.get();
		RuntimePath::Scope scope(state.compatibility);
		if (!eye.feature) {
			parameters->Reset();
			Check(state.populate(parameters), "NR parameter population");
			for (auto key : { "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width" })
				parameters->Set(key, width);
			for (auto key : { "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height" })
				parameters->Set(key, height);
			parameters->Set("Width", width);
			parameters->Set("Height", height);
			parameters->Set("PerfQualityValue", static_cast<unsigned int>(NVSDK_NGX_PerfQuality_Value_Balanced));
			parameters->Set("CreationNodeMask", 1u);
			parameters->Set("VisibilityNodeMask", 1u);
			parameters->Set("NVSDK_NGX_Parameter_PerfQualityValue", static_cast<unsigned int>(NVSDK_NGX_PerfQuality_Value_Balanced));
			parameters->Set("NVSDK_NGX_Parameter_CreationNodeMask", 1u);
			parameters->Set("NVSDK_NGX_Parameter_VisibilityNodeMask", 1u);
			parameters->Set("DLSSNR.Scale", 1.0f);
			parameters->Set("DLSSNR.ScalingRatio", 1.0f);
			parameters->Set("DLSSNR.Upscaling", 0u);
			parameters->Set("DLSSNR.Hint.Render.Preset", 0u);
			const auto flags = static_cast<unsigned int>(
				NVSDK_NGX_DLSS_Feature_Flags_DoSharpening | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
			parameters->Set("Feature_Flags", flags);
			parameters->Set("NVSDK_NGX_Parameter_Feature_Flags", flags);
			parameters->Set("InPreExposure", 1.0f);
			parameters->Set("InExposureScale", 1.0f);
			parameters->Set("NVSDK_NGX_Parameter_PreExposure", 1.0f);
			parameters->Set("NVSDK_NGX_Parameter_ExposureScale", 1.0f);
			parameters->Set("DLSSNR.AutoExposure", 1u);
			parameters->Set("DLSSNR.Hdr", 0u);
			parameters->Set("DLSSNR.SDR", 1u);
			parameters->Set("DLSSNR.Style", tuning.style);
			parameters->Set("DLSSNR.Intensity", tuning.intensity);
			parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
			parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
			parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
			parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
			NVSDK_NGX_Handle* handle = nullptr;
			const auto result = state.create(commands, static_cast<NVSDK_NGX_Feature>(18), parameters, &handle);
			frame.result = static_cast<uint32_t>(result);
			eye.feature.reset(handle);
			if (NVSDK_NGX_FAILED(result) || !handle) {
				logger::error("[NeuralRendering] Eye {} SDR-proxy creation failed: 0x{:08X} (flags=0x{:X})", eyeIndex, static_cast<uint32_t>(result), flags);
				return false;
			}
			logger::info("[NeuralRendering] Eye {} SDR-proxy Feature 18 created (flags=0x{:X}, Upscaling=0, populated parameters)", eyeIndex, flags);
			frame.created = true;
			frame.reset = true;
		}
		parameters->Set("DLSSNR.Color", color);
		parameters->Set("DLSSNR.Depth", depth);
		parameters->Set("DLSSNR.MVec", motion);
		parameters->Set("DLSSNR.Output", output);
		for (auto key : { "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
				 "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY" })
			parameters->Set(key, 0u);
		for (auto key : { "DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth" })
			parameters->Set(key, width);
		for (auto key : { "DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight" })
			parameters->Set(key, height);
		for (auto key : { "DLSSNR.DepthSubrectWidth", "DLSSNR.MVecSubrectWidth" })
			parameters->Set(key, guideWidth);
		for (auto key : { "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectHeight" })
			parameters->Set(key, guideHeight);
		parameters->Set("DLSSNR.MVecScaleX", 1.0f);
		parameters->Set("DLSSNR.MVecScaleY", 1.0f);
		parameters->Set("DLSSNR.DepthInverted", 0u);
		parameters->Set("DLSSNR.Enabled", 1u);
		parameters->Set("DLSSNR.Reset", frame.reset ? 1u : 0u);
		parameters->Set("DLSSNR.Upscaling", 0u);
		parameters->Set("DLSSNR.Scale", 1.0f);
		parameters->Set("DLSSNR.ScalingRatio", 1.0f);
		parameters->Set("DLSSNR.Intensity", tuning.intensity);
		parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
		parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
		parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
		parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
		parameters->Set("DLSSNR.Style", tuning.style);
		parameters->Set("DLSSNR.UICorrection", 0u);
		parameters->Set("Sharpness", 0.0f);
		const auto result = state.evaluate(commands, eye.feature.get(), parameters, nullptr);
		frame.result = static_cast<uint32_t>(result);
		if (NVSDK_NGX_FAILED(result)) {
			logger::error("[NeuralRendering] Eye {} evaluation failed: 0x{:08X}", eyeIndex, static_cast<uint32_t>(result));
			return false;
		}
		return true;
	}
}
