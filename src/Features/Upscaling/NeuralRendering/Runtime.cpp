#include "Runtime.h"

#include "D3D12Interop.h"
#include "Parameters.h"

#include "Utils/FileSystem.h"
#include "Utils/SehGuard.h"
#include "Utils/WinApi.h"

#include <detours/detours.h>
#include <nvsdk_ngx.h>

#include <array>
#include <cstring>
#include <format>
#include <winrt/base.h>

namespace NR
{
	namespace
	{
		using PfnInitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
		using PfnShutdown1 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
		using PfnAllocParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using PfnDestroyParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using PfnPopulate = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using PfnCreate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using PfnEvaluate = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using PfnRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using PfnIdentity = unsigned int(NVSDK_CONV*)();

		struct ModuleDeleter
		{
			void operator()(HMODULE a_module) const { FreeLibrary(a_module); }
		};
		using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

		/**
		 * @brief Runs one NGX call under SEH: /EHsc catch blocks never see an access violation
		 *        inside the DLL, so a fault caught here is the only thing between it and a crash.
		 * @param a_poisoned Set on a fault; every later NGX call is then skipped.
		 * @param a_sentinel Value to keep when the call never returns one.
		 * @param a_call Performs the call. It may only reference existing objects, because a fault
		 *               skips destructors in its frame.
		 * @return The call's value, or a_sentinel when it faulted.
		 */
		template <class T, class F>
		T GuardNgxCall(bool& a_poisoned, T a_sentinel, F&& a_call)
		{
			T value = a_sentinel;
			DWORD fault = 0;
			if (!Util::SehGuarded([&] { value = a_call(); }, &fault)) {
				a_poisoned = true;
				throw RuntimeError(FailureKind::kSehFault,
					std::format("an NGX call faulted (exception 0x{:08X}) and is never re-entered", fault));
			}
			return value;
		}

		/** @brief NVSDK_NGX_Result form of GuardNgxCall. */
		template <class F>
		NVSDK_NGX_Result GuardNgxResult(bool& a_poisoned, F&& a_call)
		{
			return GuardNgxCall<NVSDK_NGX_Result>(a_poisoned, NVSDK_NGX_Result_Fail, std::forward<F>(a_call));
		}

		/** @brief Void-call form of GuardNgxResult. */
		template <class F>
		void GuardNgxVoid(bool& a_poisoned, F&& a_call)
		{
			(void)GuardNgxResult(a_poisoned, [&] {
				a_call();
				return NVSDK_NGX_Result_Success;
			});
		}

		template <class T>
		T Resolve(HMODULE a_module, const char* a_name)
		{
			auto function = GetProcAddress(a_module, a_name);
			if (!function)
				throw RuntimeError(FailureKind::kExportMissing, std::format("nvngx_dlssnr.dll is missing the export {}", a_name));
			return reinterpret_cast<T>(function);
		}

		void Check(NVSDK_NGX_Result a_result, FailureKind a_kind, const char* a_operation)
		{
			if (NVSDK_NGX_FAILED(a_result))
				throw RuntimeError(a_kind, std::format("{} failed: NGX 0x{:08X}", a_operation, static_cast<uint32_t>(a_result)));
		}

		/** @brief Plain argument block for the creation writes; a fault may not skip a destructor. */
		struct CreationWrites
		{
			uint32_t width = 0, height = 0;
			Tuning tuning;
		};

		/** @brief Plain argument block for the per-frame writes. */
		struct FrameWrites
		{
			ID3D12Resource* color = nullptr;
			ID3D12Resource* depth = nullptr;
			ID3D12Resource* motion = nullptr;
			ID3D12Resource* output = nullptr;
			uint32_t width = 0, height = 0;
			GuideRegion depthRegion, motionRegion;
			float motionScaleX = 1.0f, motionScaleY = 1.0f;
			uint32_t reset = 0;
			Tuning tuning;
		};

		void WriteCreation(NVSDK_NGX_Parameter* a_parameters, const CreationWrites& a_writes)
		{
			for (auto key : { "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width" })
				SetInt(a_parameters, key, a_writes.width);
			for (auto key : { "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height" })
				SetInt(a_parameters, key, a_writes.height);
			SetInt(a_parameters, "Width", a_writes.width);
			SetInt(a_parameters, "Height", a_writes.height);
			SetInt(a_parameters, "PerfQualityValue", static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced));
			SetInt(a_parameters, "CreationNodeMask", 1u);
			SetInt(a_parameters, "VisibilityNodeMask", 1u);
			// Speculative aliases carried from the tested build; a runtime A/B has not shown either
			// spelling to be unread, and dropping one the snippet needs costs the whole feature.
			SetInt(a_parameters, "NVSDK_NGX_Parameter_PerfQualityValue", static_cast<uint32_t>(NVSDK_NGX_PerfQuality_Value_Balanced));
			SetInt(a_parameters, "NVSDK_NGX_Parameter_CreationNodeMask", 1u);
			SetInt(a_parameters, "NVSDK_NGX_Parameter_VisibilityNodeMask", 1u);
			SetFloat(a_parameters, "DLSSNR.Scale", 1.0f);
			SetFloat(a_parameters, "DLSSNR.ScalingRatio", 1.0f);
			SetInt(a_parameters, "DLSSNR.Upscaling", 0u);
			SetInt(a_parameters, "DLSSNR.Hint.Render.Preset", 0u);
			const auto contract = a_writes.tuning.ProxyContractValue();
			const auto flags = ProxyFlags(contract);
			SetInt(a_parameters, "Feature_Flags", flags);
			SetInt(a_parameters, "NVSDK_NGX_Parameter_Feature_Flags", flags);
			SetFloat(a_parameters, "InPreExposure", 1.0f);
			SetFloat(a_parameters, "InExposureScale", 1.0f);
			SetFloat(a_parameters, "NVSDK_NGX_Parameter_PreExposure", 1.0f);
			SetFloat(a_parameters, "NVSDK_NGX_Parameter_ExposureScale", 1.0f);
			SetInt(a_parameters, "DLSSNR.AutoExposure", 1u);
			SetInt(a_parameters, "DLSSNR.Hdr", ProxyIsHdr(contract) ? 1u : 0u);
			SetInt(a_parameters, "DLSSNR.SDR", ProxyIsHdr(contract) ? 0u : 1u);
			SetInt(a_parameters, "DLSSNR.Style", a_writes.tuning.style);
			SetFloat(a_parameters, "DLSSNR.Intensity", a_writes.tuning.intensity);
			SetFloat(a_parameters, "DLSSNR.LocalToneStrength", a_writes.tuning.localToneStrength);
			SetFloat(a_parameters, "DLSSNR.LocalStructureStrength", a_writes.tuning.localStructureStrength);
			SetFloat(a_parameters, "DLSSNR.SkinStructureStrength", a_writes.tuning.skinStructureStrength);
			SetPointer(a_parameters, "DLSSNR.ControlMask", nullptr);
			SetInt(a_parameters, "DLSSNR.UseAutoMask", a_writes.tuning.useAutoMask ? 1u : 0u);
			SetInt(a_parameters, "DLSSNR.UICorrection", 1u);
		}

		void WriteFrame(NVSDK_NGX_Parameter* a_parameters, const FrameWrites& a_writes)
		{
			SetPointer(a_parameters, "DLSSNR.Color", a_writes.color);
			SetPointer(a_parameters, "DLSSNR.Depth", a_writes.depth);
			SetPointer(a_parameters, "DLSSNR.MVec", a_writes.motion);
			SetPointer(a_parameters, "DLSSNR.Output", a_writes.output);
			for (auto key : { "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY" })
				SetInt(a_parameters, key, 0u);
			for (auto key : { "DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth" })
				SetInt(a_parameters, key, a_writes.width);
			for (auto key : { "DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight" })
				SetInt(a_parameters, key, a_writes.height);
			SetInt(a_parameters, "DLSSNR.DepthSubrectBaseX", a_writes.depthRegion.baseX);
			SetInt(a_parameters, "DLSSNR.DepthSubrectBaseY", a_writes.depthRegion.baseY);
			SetInt(a_parameters, "DLSSNR.DepthSubrectWidth", a_writes.depthRegion.width);
			SetInt(a_parameters, "DLSSNR.DepthSubrectHeight", a_writes.depthRegion.height);
			SetInt(a_parameters, "DLSSNR.MVecSubrectBaseX", a_writes.motionRegion.baseX);
			SetInt(a_parameters, "DLSSNR.MVecSubrectBaseY", a_writes.motionRegion.baseY);
			SetInt(a_parameters, "DLSSNR.MVecSubrectWidth", a_writes.motionRegion.width);
			SetInt(a_parameters, "DLSSNR.MVecSubrectHeight", a_writes.motionRegion.height);
			SetFloat(a_parameters, "DLSSNR.MVecScaleX", a_writes.motionScaleX);
			SetFloat(a_parameters, "DLSSNR.MVecScaleY", a_writes.motionScaleY);
			SetInt(a_parameters, "DLSSNR.DepthInverted", 0u);
			SetInt(a_parameters, "DLSSNR.Enabled", 1u);
			SetInt(a_parameters, "DLSSNR.Reset", a_writes.reset);
			// Repeats of the creation writes: the tested runtime was driven with them at both points.
			SetInt(a_parameters, "DLSSNR.Upscaling", 0u);
			SetFloat(a_parameters, "DLSSNR.Scale", 1.0f);
			SetFloat(a_parameters, "DLSSNR.ScalingRatio", 1.0f);
			SetFloat(a_parameters, "DLSSNR.Intensity", a_writes.tuning.intensity);
			SetFloat(a_parameters, "DLSSNR.LocalToneStrength", a_writes.tuning.localToneStrength);
			SetFloat(a_parameters, "DLSSNR.LocalStructureStrength", a_writes.tuning.localStructureStrength);
			SetFloat(a_parameters, "DLSSNR.SkinStructureStrength", a_writes.tuning.skinStructureStrength);
			SetPointer(a_parameters, "DLSSNR.ControlMask", nullptr);
			SetInt(a_parameters, "DLSSNR.UseAutoMask", a_writes.tuning.useAutoMask ? 1u : 0u);
			SetInt(a_parameters, "DLSSNR.Style", a_writes.tuning.style);
			SetFloat(a_parameters, "Sharpness", 0.0f);
		}

		/** @brief Active region of a guide resource; throwing here can only be a pass-integration bug. */
		GuideRegion ClampedGuideRegion(ID3D12Resource* a_resource, const GuideRegion& a_region, const char* a_kind)
		{
			if (!a_resource)
				throw std::runtime_error(std::format("Feature 18 was given no {} guide resource", a_kind));
			const auto desc = a_resource->GetDesc();
			const auto clamped = ClampGuideRegion(static_cast<uint32_t>(desc.Width), desc.Height, a_region);
			if (!clamped)
				throw std::runtime_error(std::format("Feature 18 {} guide subrect covers no texel", a_kind));
			return *clamped;
		}

		/** @brief Directory NGX may write its own log to, beside the plugin log. */
		std::filesystem::path NgxDataDirectory()
		{
			const auto logPath = Util::PathHelpers::GetLogPath();
			if (logPath.empty())
				return std::filesystem::temp_directory_path() / L"OpenShaders-NGX";
			return logPath.parent_path() / "CommunityShaders" / "NGX";
		}

		/** @brief Buffer for GetModuleFileNameW: an extended-length image path can run past MAX_PATH. */
		constexpr DWORD kModuleImageChars = MAX_PATH * 4;

		/** @brief True when a module's image is under the Windows DriverStore, i.e. NVIDIA's own NGX core. */
		bool IsDriverStoreModule(HMODULE a_module)
		{
			wchar_t image[kModuleImageChars]{};
			if (!GetModuleFileNameW(a_module, image, kModuleImageChars))
				return false;
			wchar_t systemDirectory[MAX_PATH]{};
			if (!GetSystemDirectoryW(systemDirectory, MAX_PATH))
				return false;
			const std::wstring prefix = std::wstring(systemDirectory) + L"\\DriverStore\\";
			const std::wstring_view imageView(image);
			if (imageView.size() < prefix.size())
				return false;
			return CompareStringOrdinal(imageView.data(), static_cast<int>(prefix.size()),
					   prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
		}

		struct CoreApi
		{
			Module module;
			PfnAllocParams allocate = nullptr;
			PfnDestroyParams destroy = nullptr;
		};

		/**
		 * @brief Binds the NGX parameter API by name, never by scanning loaded modules.
		 *        A module that only looks like nvngx.dll (a third-party proxy) must not be bound:
		 *        its parameter block is a different ABI, and the resulting handle would be garbage.
		 */
		CoreApi BindCore()
		{
			for (const auto* name : { L"_nvngx.dll", L"nvngx.dll" }) {
				HMODULE module = GetModuleHandleW(name);
				if (!module || !IsDriverStoreModule(module))
					continue;
				auto allocate = GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters");
				auto destroy = GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters");
				if (!allocate || !destroy)
					continue;
				CoreApi api;
				HMODULE retained = nullptr;
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(allocate), &retained))
					continue;
				api.module.reset(retained);
				api.allocate = reinterpret_cast<PfnAllocParams>(allocate);
				api.destroy = reinterpret_cast<PfnDestroyParams>(destroy);
				return api;
			}
			return {};
		}

		/** @brief Makes the snippet see Open Shaders' own module as nvngx.dll inside a Scope. */
		class RuntimePath
		{
		public:
			void Install(HMODULE a_runtime, const std::filesystem::path& a_callerIdentity)
			{
				if (installed)
					return;
				spoofedCallerPath = a_callerIdentity.wstring();
				caller = DetourGetContainingModule(reinterpret_cast<void*>(&Proxy));
				DetourEnumerateImportsEx(a_runtime, this, nullptr, [](void* a_data, DWORD, const char* a_name, void** a_function) -> BOOL {
					if (a_name && std::strcmp(a_name, "GetModuleFileNameW") == 0)
						static_cast<RuntimePath*>(a_data)->slot = a_function;
					return TRUE;
				});
				if (!caller || !slot)
					throw RuntimeError(FailureKind::kLoadFailed, "nvngx_dlssnr.dll does not import GetModuleFileNameW");
				if (*slot == reinterpret_cast<void*>(&Proxy))
					return;  // a patch outlived its owner: re-installing would capture Proxy as the original
				original = reinterpret_cast<decltype(original)>(*slot);
				DWORD protection;
				if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
					throw RuntimeError(FailureKind::kLoadFailed, std::format("could not patch the caller import (Win32 error {})", GetLastError()));
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

			/** @brief Applies the spoof for one thread until it leaves scope; nestable. */
			struct Scope
			{
				explicit Scope(RuntimePath& a_path) :
					previous(active) { active = &a_path; }
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

			static DWORD WINAPI Proxy(HMODULE a_module, LPWSTR a_filename, DWORD a_size)
			{
				if (!active || a_module != active->caller || !a_filename || !a_size)
					return original(a_module, a_filename, a_size);
				const auto length = static_cast<DWORD>(active->spoofedCallerPath.size());
				const auto count = std::min(length, a_size - 1);
				std::memcpy(a_filename, active->spoofedCallerPath.data(), count * sizeof(wchar_t));
				a_filename[count] = L'\0';
				if (count < length)
					SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return count < length ? a_size : length;
			}
		};
	}

	struct Runtime::Impl
	{
		Module module, core;
		RuntimePath compatibility;
		winrt::com_ptr<ID3D12Device> device;
		D3D12Interop* interop = nullptr;
		PfnInitExt initialize = nullptr;
		PfnShutdown1 shutdown = nullptr;
		PfnAllocParams allocate = nullptr;
		PfnDestroyParams destroy = nullptr;
		PfnPopulate populate = nullptr;
		PfnCreate create = nullptr;
		PfnEvaluate evaluate = nullptr;
		PfnRelease release = nullptr;
		bool initialized = false;
		bool poisoned = false;

		struct FeatureDeleter
		{
			Impl* owner = nullptr;
			void operator()(NVSDK_NGX_Handle* a_handle) const { owner->ReleaseFeature(a_handle); }
		};
		struct ParameterDeleter
		{
			Impl* owner = nullptr;
			void operator()(NVSDK_NGX_Parameter* a_parameters) const { owner->DestroyParameters(a_parameters); }
		};
		struct Eye
		{
			std::unique_ptr<NVSDK_NGX_Parameter, ParameterDeleter> parameters{ nullptr, {} };
			std::unique_ptr<NVSDK_NGX_Handle, FeatureDeleter> feature{ nullptr, {} };
		};
		std::array<Eye, 2> eyes;

		/** @brief Releases one handle; never throws, because a deleter runs inside a destructor. */
		void ReleaseFeature(NVSDK_NGX_Handle* a_handle)
		{
			if (poisoned || !a_handle || !release)
				return;
			RuntimePath::Scope scope(compatibility);
			NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
			try {
				result = GuardNgxResult(poisoned, [&] { return release(a_handle); });
			} catch (const std::exception& e) {
				logger::error("[NeuralRendering] {}", e.what());
				return;
			}
			if (NVSDK_NGX_FAILED(result))
				logger::warn("[NeuralRendering] Feature release failed: 0x{:08X}", static_cast<uint32_t>(result));
		}

		/** @brief Destroys one parameter block. Core calls take no spoof scope. */
		void DestroyParameters(NVSDK_NGX_Parameter* a_parameters)
		{
			if (poisoned || !a_parameters || !destroy)
				return;
			try {
				GuardNgxVoid(poisoned, [&] { destroy(a_parameters); });
			} catch (const std::exception& e) {
				logger::error("[NeuralRendering] {}", e.what());
			}
		}

		~Impl()
		{
			if (processTerminating.load(std::memory_order_relaxed)) {
				// Static destruction: NGX may already be unloaded and logger is gone, so the handles,
				// blocks and modules are abandoned instead of released.
				for (auto& eye : eyes) {
					eye.feature.release();
					eye.parameters.release();
				}
				module.release();
				core.release();
				return;
			}
			for (auto& eye : eyes) {
				eye.feature.reset();
				eye.parameters.reset();
			}
			if (poisoned) {
				// A DLL that faulted, here or earlier, is never re-entered, not even to unload it: the
				// module, the handles and the blocks leak until the process exits.
				module.release();
				core.release();
				return;
			}
			if (initialized && shutdown) {
				RuntimePath::Scope scope(compatibility);
				try {
					GuardNgxVoid(poisoned, [&] { shutdown(device.get()); });
				} catch (const std::exception& e) {
					logger::error("[NeuralRendering] NGX shutdown failed: {}", e.what());
				}
			}
		}
	};

	Runtime::Runtime() = default;
	Runtime::~Runtime() = default;

	AttemptResult Runtime::Initialize(D3D12Interop& a_interop, const std::filesystem::path& a_directory)
	{
		AttemptResult result;
		if (impl && (impl->initialized || impl->poisoned)) {
			result.ok = impl->initialized;
			return result;
		}

		auto pending = std::make_unique<Impl>();
		auto& state = *pending;
		state.interop = &a_interop;
		try {
			const auto path = a_directory / kRuntimeDllName;
			std::error_code error;
			if (!std::filesystem::is_regular_file(path, error))
				throw RuntimeError(FailureKind::kDllMissing,
					std::format("{} is not installed in {}", path.filename().string(), a_directory.string()));
			const auto version = Util::GetDllVersion(path.wstring());
			if (!IsSupportedRuntimeVersion(version))
				throw RuntimeError(FailureKind::kVersionRejected,
					VersionRejectedReason(path.filename().string(), a_directory.string(), version ? version->string() : std::string{}));

			state.module.reset(LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
			if (!state.module)
				throw RuntimeError(FailureKind::kLoadFailed,
					std::format("{} failed to load (Win32 error {})", path.filename().string(), GetLastError()));

			state.initialize = Resolve<PfnInitExt>(state.module.get(), "NVSDK_NGX_D3D12_Init_Ext");
			state.shutdown = Resolve<PfnShutdown1>(state.module.get(), "NVSDK_NGX_D3D12_Shutdown1");
			state.create = Resolve<PfnCreate>(state.module.get(), "NVSDK_NGX_D3D12_CreateFeature");
			state.evaluate = Resolve<PfnEvaluate>(state.module.get(), "NVSDK_NGX_D3D12_EvaluateFeature");
			state.release = Resolve<PfnRelease>(state.module.get(), "NVSDK_NGX_D3D12_ReleaseFeature");
			state.populate = Resolve<PfnPopulate>(state.module.get(), "NVSDK_NGX_D3D12_PopulateParameters_Impl");
			// Both identity values are the DLL's own: Open Shaders holds no NGX application id.
			const auto applicationId = Resolve<PfnIdentity>(state.module.get(), "NVSDK_NGX_GetApplicationId");
			const auto apiVersion = Resolve<PfnIdentity>(state.module.get(), "NVSDK_NGX_GetAPIVersion");
			const auto appId = GuardNgxCall<unsigned int>(state.poisoned, 0u, [&] { return applicationId(); });
			const auto api = GuardNgxCall<unsigned int>(state.poisoned, 0u, [&] { return apiVersion(); });

			// The snippet re-reads its caller's module path and only proceeds for one named
			// nvngx.dll; the file itself need not exist.
			state.compatibility.Install(state.module.get(), a_directory / L"nvngx.dll");

			const auto cache = NgxDataDirectory();
			std::filesystem::create_directories(cache);

			state.device.copy_from(a_interop.Device());
			{
				RuntimePath::Scope scope(state.compatibility);
				const auto initialized = GuardNgxResult(state.poisoned, [&] {
					return state.initialize(appId, cache.c_str(), state.device.get(), static_cast<NVSDK_NGX_Version>(api), nullptr);
				});
				Check(initialized, FailureKind::kInitFailed, "NGX initialization");
				state.initialized = true;
			}

			auto core = BindCore();
			if (!core.module)
				throw RuntimeError(FailureKind::kParameterApiUnavailable,
					"no DriverStore nvngx.dll is loaded, so the NGX parameter API is unavailable");
			state.core = std::move(core.module);
			state.allocate = core.allocate;
			state.destroy = core.destroy;

			// Both eyes get a block even in flat: the blocks are allocated with the runtime, never
			// from inside a render frame.
			for (auto& eye : state.eyes) {
				NVSDK_NGX_Parameter* parameters = nullptr;
				const auto allocated = GuardNgxResult(state.poisoned, [&] { return state.allocate(&parameters); });
				Check(allocated, FailureKind::kInitFailed, "NR parameter allocation");
				if (!parameters)
					throw RuntimeError(FailureKind::kInitFailed, "NGX returned a null parameter block");
				eye.parameters = { parameters, { &state } };
				eye.feature = { nullptr, { &state } };
			}
			const auto luid = a_interop.AdapterLuid();
			logger::info("[NeuralRendering] active: runtime {} on adapter LUID {:08X}:{:08X}",
				version->string(), luid.HighPart, luid.LowPart);
		} catch (const RuntimeError& e) {
			result.kind = e.kind;
			result.reason = e.what();
			return result;
		} catch (const std::exception& e) {
			result.kind = FailureKind::kInitFailed;
			result.reason = e.what();
			return result;
		}

		impl = std::move(pending);
		result.ok = true;
		return result;
	}

	void Runtime::ResetFeatures()
	{
		if (!impl)
			return;
		for (auto& eye : impl->eyes)
			eye.feature.reset();
	}

	AttemptResult Runtime::Shutdown()
	{
		AttemptResult result;
		if (!impl || processTerminating.load(std::memory_order_relaxed)) {
			result.ok = true;
			return result;
		}
		if (impl->initialized && impl->interop) {
			// Every handle and shared resource must have retired before NGX and the device go away.
			try {
				impl->interop->Drain();
			} catch (const RuntimeError& e) {
				result.kind = e.kind;
				result.reason = e.what();
				// A removed device fails every later call the same way, so what it holds can never
				// retire: the release below is what keeps the runtime from being retained for good.
				if (e.kind != FailureKind::kDeviceRemoved)
					return result;
			} catch (const std::exception& e) {
				result.kind = FailureKind::kDrainTimeout;
				result.reason = e.what();
				return result;
			}
		}
		impl.reset();
		result.ok = true;
		return result;
	}

	bool Runtime::Evaluate(ID3D12GraphicsCommandList* a_commands, uint32_t a_eyeIndex,
		ID3D12Resource* a_color, ID3D12Resource* a_depth, ID3D12Resource* a_motion, ID3D12Resource* a_output,
		uint32_t a_width, uint32_t a_height, const GuideParameters& a_guides,
		FrameParameters& a_frame, const Tuning& a_tuning)
	{
		a_frame.created = false;
		a_frame.result = 0;
		if (!impl || !impl->initialized || impl->poisoned || a_eyeIndex >= impl->eyes.size())
			return false;

		auto& state = *impl;
		auto& eye = state.eyes[a_eyeIndex];
		const auto depthRegion = ClampedGuideRegion(a_depth, a_guides.depth, "depth");
		const auto motionRegion = ClampedGuideRegion(a_motion, a_guides.motion, "motion");
		auto* parameters = eye.parameters.get();
		RuntimePath::Scope scope(state.compatibility);

		if (!eye.feature) {
			GuardNgxVoid(state.poisoned, [&] { parameters->Reset(); });
			const auto populated = GuardNgxResult(state.poisoned, [&] { return state.populate(parameters); });
			Check(populated, FailureKind::kInitFailed, "NR parameter population");
			const CreationWrites creation{ a_width, a_height, a_tuning };
			GuardNgxVoid(state.poisoned, [&] { WriteCreation(parameters, creation); });
			NVSDK_NGX_Handle* handle = nullptr;
			const auto created = GuardNgxResult(state.poisoned, [&] {
				return state.create(a_commands, NVSDK_NGX_Feature_Reserved18, parameters, &handle);
			});
			a_frame.result = static_cast<uint32_t>(created);
			eye.feature.reset(handle);
			const auto flags = ProxyFlags(a_tuning.ProxyContractValue());
			if (NVSDK_NGX_FAILED(created) || !handle) {
				logger::error("[NeuralRendering] Eye {} Feature 18 creation failed: 0x{:08X} (flags=0x{:X})", a_eyeIndex, a_frame.result, flags);
				return false;
			}
			logger::debug("[NeuralRendering] Eye {} Feature 18 instance created (flags=0x{:X}, {})", a_eyeIndex, flags,
				ProxyIsHdr(a_tuning.ProxyContractValue()) ? "HDR proxy contract" : "SDR proxy contract");
			a_frame.created = true;
			// Creation keys stay in the block, so the first evaluate must still ask for a history reset.
			a_frame.reset = true;
		}

		const FrameWrites writes{
			a_color, a_depth, a_motion, a_output,
			a_width, a_height,
			depthRegion, motionRegion,
			a_guides.motionScaleX, a_guides.motionScaleY,
			a_frame.reset ? 1u : 0u,
			a_tuning
		};
		GuardNgxVoid(state.poisoned, [&] { WriteFrame(parameters, writes); });
		const auto evaluated = GuardNgxResult(state.poisoned, [&] {
			return state.evaluate(a_commands, eye.feature.get(), parameters, nullptr);
		});
		a_frame.result = static_cast<uint32_t>(evaluated);
		if (NVSDK_NGX_FAILED(evaluated)) {
			logger::debug("[NeuralRendering] Eye {} evaluation failed: 0x{:08X}", a_eyeIndex, a_frame.result);
			return false;
		}
		return true;
	}

	bool Runtime::Initialized() const
	{
		return impl && impl->initialized;
	}

	bool Runtime::Poisoned() const
	{
		return impl && impl->poisoned;
	}
}
