#include "RuntimeResources.h"

#include "Globals.h"
#include "Menu.h"
#include <DirectXTex.h>
#include <atomic>
#include <chrono>
#include <dxgi1_4.h>
#include <unordered_map>

namespace Util
{
	extern GUID WKPDID_D3DDebugObjectNameT;

	namespace
	{
		constexpr GUID kResourceLifetime = { 0x97a2e619, 0x18af, 0x4b18, { 0x9c, 0x31, 0xbc, 0xde, 0x68, 0x40, 0x6c, 0x9f } };
		constexpr uint32_t kFirstSampleFrames = 1;
		constexpr uint32_t kSecondSampleFrames = 120;
		constexpr uint32_t kPendingSampleFrames = 600;
		constexpr size_t kMaxPendingTransitions = 256;
		constexpr size_t kMaxPendingNames = 16;

		struct ResourceLifetime
		{
			std::string name;
			uint64_t bytes = 0;
			std::atomic<bool> destroyed{ false };
		};

		class ResourceLifetimeToken final : public IUnknown
		{
		public:
			explicit ResourceLifetimeToken(std::shared_ptr<ResourceLifetime> value) : record(std::move(value)) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
			{
				if (!out)
					return E_POINTER;
				*out = nullptr;
				if (iid != __uuidof(IUnknown))
					return E_NOINTERFACE;
				*out = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const auto remaining = --references;
				if (!remaining) {
					record->destroyed.store(true, std::memory_order_release);
					delete this;
				}
				return remaining;
			}
			std::shared_ptr<ResourceLifetime> record;

		private:
			std::atomic<ULONG> references{ 1 };
		};

		struct MemoryUsage
		{
			int64_t local = -1;
			int64_t nonlocal = -1;
		};

		MemoryUsage GetMemoryUsage()
		{
			MemoryUsage result;
			winrt::com_ptr<IDXGIAdapter3> adapter;
			if (auto* menu = Menu::GetSingleton())
				adapter = menu->GetDXGIAdapter3();
			if (!adapter && globals::d3d::device) {
				winrt::com_ptr<IDXGIDevice> device;
				winrt::com_ptr<IDXGIAdapter> base;
				if (SUCCEEDED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(device.put()))) &&
					SUCCEEDED(device->GetAdapter(base.put())))
					base->QueryInterface(IID_PPV_ARGS(adapter.put()));
			}
			if (adapter) {
				DXGI_QUERY_VIDEO_MEMORY_INFO info{};
				if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
					result.local = static_cast<int64_t>(info.CurrentUsage);
				if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
					result.nonlocal = static_cast<int64_t>(info.CurrentUsage);
			}
			return result;
		}

		uint64_t GetResourcePayload(ID3D11Resource* resource)
		{
			D3D11_RESOURCE_DIMENSION dimension{};
			resource->GetType(&dimension);
			UINT width = 1, height = 1, depth = 1, arrays = 1, mips = 1, samples = 1;
			DXGI_FORMAT format{};
			switch (dimension) {
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				{
					D3D11_BUFFER_DESC desc{};
					static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
					return desc.ByteWidth;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				{
					D3D11_TEXTURE1D_DESC desc{};
					static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
					width = desc.Width;
					arrays = desc.ArraySize;
					mips = desc.MipLevels;
					format = desc.Format;
					break;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				{
					D3D11_TEXTURE2D_DESC desc{};
					static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
					width = desc.Width;
					height = desc.Height;
					arrays = desc.ArraySize;
					mips = desc.MipLevels;
					samples = desc.SampleDesc.Count;
					format = desc.Format;
					break;
				}
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				{
					D3D11_TEXTURE3D_DESC desc{};
					static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
					width = desc.Width;
					height = desc.Height;
					depth = desc.Depth;
					mips = desc.MipLevels;
					format = desc.Format;
					break;
				}
			default:
				return 0;
			}
			uint64_t bytes = 0;
			for (UINT mip = 0; mip < mips; ++mip) {
				size_t rowPitch{}, slicePitch{};
				if (FAILED(DirectX::ComputePitch(format, width, height, rowPitch, slicePitch)))
					return 0;
				bytes += static_cast<uint64_t>(slicePitch) * depth * arrays * samples;
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
				depth = std::max(1u, depth / 2);
			}
			return bytes;
		}

		std::shared_ptr<ResourceLifetime> ObserveResource(ID3D11Resource* resource)
		{
			IUnknown* existing = nullptr;
			UINT size = sizeof(existing);
			if (SUCCEEDED(resource->GetPrivateData(kResourceLifetime, &size, &existing)) && existing) {
				auto record = static_cast<ResourceLifetimeToken*>(existing)->record;
				existing->Release();
				return record;
			}
			auto record = std::make_shared<ResourceLifetime>();
			char name[1024]{};
			size = sizeof(name) - 1;
			if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectNameT, &size, name)))
				record->name.assign(name, size);
			else
				record->name = "unnamed";
			record->bytes = GetResourcePayload(resource);
			auto* token = new ResourceLifetimeToken(record);
			const auto hr = resource->SetPrivateDataInterface(kResourceLifetime, token);
			token->Release();
			return SUCCEEDED(hr) ? record : nullptr;
		}
	}

	struct RuntimeResourceDiagnosticState
	{
		uint64_t id = 0;
		std::string feature, operation;
		std::shared_ptr<RuntimeResourceDiagnosticState> previous;
		std::vector<std::shared_ptr<ResourceLifetime>> allocations, releases, knownBefore;
		MemoryUsage before;
		std::chrono::steady_clock::time_point started;
		uint32_t ageFrames = 0;
		uint32_t nextSample = kFirstSampleFrames;
		uint32_t observationFailures = 0;
	};

	namespace
	{
		thread_local std::shared_ptr<RuntimeResourceDiagnosticState> active;
		std::vector<std::shared_ptr<RuntimeResourceDiagnosticState>> pending;
		std::atomic<uint64_t> nextTransition{ 0 };
		std::unordered_map<std::string, std::vector<std::weak_ptr<ResourceLifetime>>> knownResources;

		bool Report(const RuntimeResourceDiagnosticState& state, std::string_view phase)
		{
			uint64_t allocatedBytes = 0, releaseBytes = 0, destroyedBytes = 0;
			size_t destroyed = 0, unknownSizes = 0, knownSurvivors = 0;
			uint64_t survivorBytes = 0;
			for (const auto& record : state.knownBefore) {
				if (!record->destroyed.load(std::memory_order_acquire)) {
					++knownSurvivors;
					survivorBytes += record->bytes;
				}
			}
			for (const auto& record : state.allocations)
				allocatedBytes += record->bytes;
			for (const auto& record : state.releases) {
				releaseBytes += record->bytes;
				unknownSizes += record->bytes == 0;
				if (record->destroyed.load(std::memory_order_acquire)) {
					++destroyed;
					destroyedBytes += record->bytes;
				}
			}
			const auto memory = GetMemoryUsage();
			logger::info("[ResourceLifetime] id={} feature={} operation={} phase={} age_frames={} allocated_observed={} allocated_payload_bytes={} release_observed={} release_payload_bytes={} destroyed={} destroyed_payload_bytes={} pending={} known_before_survivors={} survivor_payload_bytes={} unknown_release_sizes={} observation_failures={} dxgi_local_bytes={} dxgi_nonlocal_bytes={} dxgi_local_delta_bytes={}",
				state.id, state.feature, state.operation, phase, state.ageFrames, state.allocations.size(), allocatedBytes,
				state.releases.size(), releaseBytes, destroyed, destroyedBytes, state.releases.size() - destroyed, knownSurvivors, survivorBytes, unknownSizes,
				state.observationFailures, memory.local, memory.nonlocal,
				memory.local >= 0 && state.before.local >= 0 ? memory.local - state.before.local : 0);
			if (state.ageFrames >= kSecondSampleFrames) {
				size_t shown = 0;
				for (const auto& record : state.releases)
					if (!record->destroyed.load(std::memory_order_acquire) && shown++ < kMaxPendingNames)
						logger::info("[ResourceLifetime] id={} pending_resource={} payload_bytes={}", state.id, record->name, record->bytes);
			}
			if (state.ageFrames >= kSecondSampleFrames && state.operation.starts_with("disable")) {
				size_t shown = 0;
				for (const auto& record : state.knownBefore)
					if (!record->destroyed.load(std::memory_order_acquire) && shown++ < kMaxPendingNames)
						logger::info("[ResourceLifetime] id={} surviving_known_resource={} payload_bytes={} release_observed={}",
							state.id, record->name, record->bytes, std::find(state.releases.begin(), state.releases.end(), record) != state.releases.end());
			}
			return destroyed == state.releases.size() && (!state.operation.starts_with("disable") || knownSurvivors == 0);
		}

		void TrackResource(ID3D11Resource* resource, bool allocation)
		{
			if (!active || !resource)
				return;
			auto record = ObserveResource(resource);
			for (auto scope = active; scope; scope = scope->previous) {
				if (!record) {
					++scope->observationFailures;
					continue;
				}
				auto& records = allocation ? scope->allocations : scope->releases;
				if (std::find(records.begin(), records.end(), record) == records.end())
					records.push_back(record);
				auto& known = knownResources[scope->feature];
				std::erase_if(known, [](const auto& weak) { auto value = weak.lock(); return !value || value->destroyed.load(std::memory_order_acquire); });
				if (std::none_of(known.begin(), known.end(), [&](const auto& weak) { return weak.lock() == record; }))
					known.push_back(record);
			}
		}
	}

	RuntimeResourceDiagnostics::RuntimeResourceDiagnostics(std::string_view feature, std::string_view operation) :
		state(std::make_shared<RuntimeResourceDiagnosticState>())
	{
		state->id = ++nextTransition;
		state->feature = feature;
		state->operation = operation;
		for (const auto& weak : knownResources[state->feature])
			if (auto record = weak.lock(); record && !record->destroyed.load(std::memory_order_acquire))
				state->knownBefore.push_back(std::move(record));
		state->previous = active;
		state->before = GetMemoryUsage();
		state->started = std::chrono::steady_clock::now();
		active = state;
		logger::info("[ResourceLifetime] id={} feature={} operation={} phase=begin dxgi_local_bytes={} dxgi_nonlocal_bytes={}",
			state->id, state->feature, state->operation, state->before.local, state->before.nonlocal);
	}

	RuntimeResourceDiagnostics::~RuntimeResourceDiagnostics()
	{
		active = state->previous;
		state->previous.reset();
		try {
			Report(*state, "end");
			const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - state->started).count();
			logger::info("[ResourceLifetime] id={} transition_ms={:.3f}", state->id, elapsed);
			if (state->allocations.empty() && state->releases.empty() && state->knownBefore.empty())
				return;
			if (pending.size() == kMaxPendingTransitions) {
				logger::warn("[ResourceLifetime] dropping delayed monitoring for id={} at transition limit", pending.front()->id);
				pending.erase(pending.begin());
			}
			pending.push_back(state);
		} catch (...) {}
	}

	void TrackRuntimeResourceAllocation(ID3D11DeviceChild* object)
	{
		if (!active || !object)
			return;
		winrt::com_ptr<ID3D11Resource> resource;
		if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(resource.put()))))
			TrackResource(resource.get(), true);
	}

	void TrackRuntimeResourceRelease(ID3D11Resource* resource)
	{
		TrackResource(resource, false);
	}

	void PollRuntimeResourceDiagnostics()
	{
		if (pending.empty() || !globals::game::graphicsState)
			return;
		static uint32_t lastFrame = UINT32_MAX;
		const auto frame = globals::game::graphicsState->GetFrameCount();
		if (lastFrame == frame)
			return;
		lastFrame = frame;
		for (auto it = pending.begin(); it != pending.end();) {
			auto& state = **it;
			if (++state.ageFrames < state.nextSample) {
				++it;
				continue;
			}
			const bool complete = Report(state, "delayed");
			if (complete && state.ageFrames >= kSecondSampleFrames) {
				it = pending.erase(it);
				continue;
			}
			state.nextSample = state.ageFrames < kSecondSampleFrames ? kSecondSampleFrames : state.ageFrames + kPendingSampleFrames;
			++it;
		}
	}
}
