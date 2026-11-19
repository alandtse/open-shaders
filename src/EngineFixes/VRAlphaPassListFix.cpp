#include "VRAlphaPassListFix.h"

#include "EngineFixes/AlphaPassListHooks.h"
#include "EngineFixes/AlphaPassListPolicy.h"
#include "Globals.h"
#include "State.h"

#include <RE/B/BSBatchRenderer.h>
#include <RE/B/BSRenderPass.h>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <nlohmann/json.hpp>
#endif

namespace
{
	using Group = RE::BSBatchRenderer::GeometryGroup;
	using Append = void (*)(Group*, RE::BSRenderPass*, std::uint8_t);
	using Consume = void (*)(void*, const RE::NiPoint3*, float, float, std::uint32_t);
	using RenderGroup = void (*)(Group*, std::uint32_t);
	using Destroy = void (*)(RE::BSBatchRenderer*);
	Append original{};
	Consume originalConsume{};
	RenderGroup originalRenderGroup{};
	Destroy originalDestroy{};
	std::atomic_uint64_t generation{};
	bool installed{};
	std::string installationError;
	std::atomic_flag allocationWarning{}, membershipWarning{};
#ifdef DEVBENCH_BRIDGE_ENABLED
	std::atomic_bool enabled{ true };
	std::atomic_uint64_t duplicates{}, allocationFailures{}, unverifiableMemberships{};
#endif

	AlphaPassList::Queue Inspect(Group* a_group)
	{
		const auto epoch = generation.load(std::memory_order_acquire);
		return { reinterpret_cast<std::uintptr_t>(a_group), reinterpret_cast<std::uintptr_t>(a_group->passList.head),
			reinterpret_cast<std::uintptr_t>(a_group->passList.tail), epoch,
			globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0, a_group->count, a_group->flags };
	}

	void ConsumeQueue(void* a_renderer, const RE::NiPoint3* a_direction, float a_closest, float a_minimum, std::uint32_t a_flags)
	{
		AlphaPassList::RetirementScope lifetime{ generation };
		originalConsume(a_renderer, a_direction, a_closest, a_minimum, a_flags);
	}

	void RenderQueue(Group* a_group, std::uint32_t a_flags)
	{
		if (!(a_group->flags & AlphaPassList::LinkedGroup)) {
			originalRenderGroup(a_group, a_flags);
			return;
		}
		AlphaPassList::RetirementScope lifetime{ generation };
		originalRenderGroup(a_group, a_flags);
	}

	void DestroyRenderer(RE::BSBatchRenderer* a_renderer)
	{
		AlphaPassList::RetirementScope lifetime{ generation };
		originalDestroy(a_renderer);
	}

	bool ReadNext(std::uintptr_t a_node, std::uintptr_t& a_next) noexcept
	{
		__try {
			std::memcpy(&a_next, reinterpret_cast<const std::byte*>(a_node) + offsetof(RE::BSRenderPass, passGroupNext), sizeof(a_next));
			return true;
		} __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			return false;
		}
	}

	void ReportAllocationFailure() noexcept
	{
#ifdef DEVBENCH_BRIDGE_ENABLED
		allocationFailures.fetch_add(1, std::memory_order_relaxed);
#endif
		if (!allocationWarning.test_and_set(std::memory_order_relaxed))
			logger::warn("[VR alpha list] Membership cache allocation failed; using bounded native-list scans");
	}

	void GuardedAppend(Group* a_group, RE::BSRenderPass* a_pass, std::uint8_t a_front)
	{
		if (!a_group || !a_pass || !(a_group->flags & AlphaPassList::LinkedGroup)
#ifdef DEVBENCH_BRIDGE_ENABLED
		    || !enabled.load(std::memory_order_acquire)
#endif
		) {
			original(a_group, a_pass, a_front);
			return;
		}
		thread_local std::unique_ptr<AlphaPassList::Cache> cache;
		if (!cache) {
			try {
				cache = std::make_unique<AlphaPassList::Cache>();
			} catch (const std::bad_alloc&) {
				ReportAllocationFailure();
			}
		}
		const auto result = AlphaPassList::AppendUnique(cache.get(), Inspect(a_group), reinterpret_cast<std::uintptr_t>(a_pass), a_front != 0, ReadNext, [&] { original(a_group, a_pass, a_front); }, [&] { return Inspect(a_group); }, ReportAllocationFailure);
#ifdef DEVBENCH_BRIDGE_ENABLED
		if (!result.forwarded)
			duplicates.fetch_add(1, std::memory_order_relaxed);
		if (!result.membershipKnown)
			unverifiableMemberships.fetch_add(1, std::memory_order_relaxed);
#endif
		if (!result.membershipKnown && !membershipWarning.test_and_set(std::memory_order_relaxed))
			logger::warn("[VR alpha list] Membership is unreadable or bounded; retaining the native append");
	}
}

void VRAlphaPassListFix::Install()
{
	static_assert(sizeof(Group) == 0x28 && offsetof(Group, passList) == 0x08);
	static_assert(offsetof(Group, count) == 0x24 && offsetof(Group, flags) == 0x26);
	static_assert(offsetof(RE::BSRenderPass, passGroupNext) == 0x30);
	if (installed)
		return;
	const std::array hooks{
		AlphaPassList::Hook{ reinterpret_cast<void**>(&original), reinterpret_cast<void*>(&GuardedAppend) },
		AlphaPassList::Hook{ reinterpret_cast<void**>(&originalConsume), reinterpret_cast<void*>(&ConsumeQueue) },
		AlphaPassList::Hook{ reinterpret_cast<void**>(&originalRenderGroup), reinterpret_cast<void*>(&RenderQueue) },
		AlphaPassList::Hook{ reinterpret_cast<void**>(&originalDestroy), reinterpret_cast<void*>(&DestroyRenderer) }
	};
	struct Operations
	{
		bool Read(std::uintptr_t a_address, std::span<std::uint8_t> a_output) const
		{
			SIZE_T read{};
			return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a_address), a_output.data(), a_output.size(), &read) && read == a_output.size();
		}
		long InvalidOperation() const { return ERROR_INVALID_OPERATION; }
		long Begin() const { return DetourTransactionBegin(); }
		long UpdateThread() const { return DetourUpdateThread(GetCurrentThread()); }
		long Attach(void** a_original, void* a_hook) const { return DetourAttach(a_original, a_hook); }
		long Abort() const { return DetourTransactionAbort(); }
		long Commit() const { return DetourTransactionCommit(); }
	};
	const auto result = AlphaPassList::InstallHooks(globals::game::isVR,
		REL::Module::get().version() == SKSE::RUNTIME_VR_1_4_15, REL::Module::get().base(), hooks, Operations{});
	switch (result.status) {
	case AlphaPassList::InstallStatus::inactive:
		return;
	case AlphaPassList::InstallStatus::unsupported:
		installationError = "Verified alpha-list entrypoints require Skyrim VR 1.4.15";
		break;
	case AlphaPassList::InstallStatus::modified:
		installationError = "Native alpha-list entrypoint is modified or unsupported; existing hooks retained";
		break;
	case AlphaPassList::InstallStatus::failed:
		installationError = std::format("Atomic alpha-list hook installation failed ({})", result.error);
		break;
	case AlphaPassList::InstallStatus::installed:
		installationError.clear();
		installed = true;
		logger::info("[VR alpha list] Installed linked-pass duplicate protection");
		return;
	}
	logger::warn("[VR alpha list] {}", installationError);
}

#ifdef DEVBENCH_BRIDGE_ENABLED
nlohmann::json VRAlphaPassListFix::Control(const nlohmann::json& a_args)
{
	if (!a_args.is_object())
		throw std::invalid_argument("Alpha-pass arguments must be an object");
	for (const auto& [name, value] : a_args.items()) {
		(void)value;
		if (name != "action" && name != "enabled")
			throw std::invalid_argument("Unknown alpha-pass argument");
	}
	const auto action = a_args.value("action", std::string{ "status" });
	if (action != "status" && action != "set")
		throw std::invalid_argument("Expected alpha-pass action status or set");
	if (action == "status" && a_args.contains("enabled"))
		throw std::invalid_argument("Only alpha-pass action set accepts enabled");
	if (action == "set") {
		if (!a_args.contains("enabled") || !a_args.at("enabled").is_boolean())
			throw std::invalid_argument("Alpha-pass action set requires a boolean enabled field");
		const bool requested = a_args.at("enabled").get<bool>();
		if (requested && !installed)
			throw std::invalid_argument("Alpha-list protection is unavailable on this runtime or hook owner");
		if (enabled.load(std::memory_order_relaxed) != requested) {
			generation.fetch_add(1, std::memory_order_acq_rel);
			enabled.store(requested, std::memory_order_release);
		}
	}
	return { { "installed", installed }, { "enabled", enabled.load() }, { "active", installed && enabled.load() },
		{ "installationError", installationError }, { "duplicatesSuppressed", duplicates.load() },
		{ "cacheAllocationFailures", allocationFailures.load() }, { "unverifiableMemberships", unverifiableMemberships.load() },
		{ "maximumCachedGroups", AlphaPassList::MaximumCachedGroups }, { "maximumCachedPasses", AlphaPassList::MaximumCachedPasses },
		{ "maximumWalkNodes", AlphaPassList::MaximumWalkNodes }, { "generation", generation.load() } };
}
#endif
