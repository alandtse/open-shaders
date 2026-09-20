#include "../../Globals.h"
#include "../../State.h"
#include "ShadowCasterInternal.h"

#include "PassChainGuard.h"

#include <Windows.h>  // SEH (__try) for reads and writes through a possibly torn pass chain

namespace ShadowCasterManager
{
	std::atomic<bool> s_shadowRenderWindow{ false };
	std::atomic<uint32_t> s_passGuardTripsThisRender{ 0 };
	std::atomic<RE::BSShadowLight*> s_renderingLight{ nullptr };
	std::atomic<uint64_t> s_passGuardChecksTotal{ 0 };
	std::atomic<uint64_t> s_passGuardCycleRepairsTotal{ 0 };
	std::atomic<uint64_t> s_passGuardCycleSkipsTotal{ 0 };
	std::atomic<uint64_t> s_passGuardFaultSkipsTotal{ 0 };
	std::atomic<uint64_t> s_passGuardCapExceededTotal{ 0 };
	std::atomic<uint64_t> s_staleAccumulateTotal{ 0 };
	std::atomic<uint64_t> s_staleAfterRenderSkipTotal{ 0 };
	std::atomic<uint64_t> s_stalePassClearsTotal{ 0 };
	std::atomic<uint32_t> s_lastRenderSkipFrame{ 0 };
	std::atomic<size_t> s_lastRenderSkipReason{ 0 };
	std::atomic<uint64_t> s_renderSkipByReason[kRenderSkipReasonCount]{};
	std::atomic<uint64_t> s_passRegChecksTotal{ 0 };
	std::atomic<uint64_t> s_passRegRingsTotal{ 0 };
	std::atomic<uint64_t> s_stalePromotedTotal{ 0 };
	std::atomic<uint64_t> s_passGuardRepairsPromotedTotal{ 0 };
	std::atomic<uint64_t> s_passRegRingsPromotedTotal{ 0 };
	std::unordered_map<RE::BSShadowLight*, uint32_t> s_lightRenderFrame;

	namespace
	{
		constexpr uint32_t kPassChainWalkCap = 8192;
		constexpr uint32_t kRegistrationWalkCap = 4096;
		constexpr uint32_t kGuardLogLimit = 6;
		constexpr uint32_t kGuardDumpLimit = 3;
		constexpr uint32_t kRegistrationLogLimit = 24;
		constexpr size_t kRegistrationHistoryCap = 1u << 17;
		constexpr uint32_t kGuardLogNodes = 12;
		constexpr size_t kPassBytes = 0x48;

		std::atomic<uint32_t> s_forcedSkips{ 0 };
		std::atomic<uint32_t> s_forcedRings{ 0 };
		std::atomic<uint32_t> s_guardLogCount{ 0 };
		std::atomic<uint32_t> s_registrationLogCount{ 0 };
		std::atomic<bool> s_traceRegistration{ false };

		// The engine calls the guard once per pass of a group chain; following a verified chain by
		// cursor keeps the check linear instead of re-walking the chain from every pass.
		const RE::BSRenderPass* s_cursorExpected = nullptr;
		uint32_t s_cursorRemaining = 0;

		enum class LinkResult
		{
			Clean,         ///< acyclic, or too long to judge (allowed)
			Repaired,      ///< a cycle was cut and the chain re-verified acyclic
			Unrepairable,  ///< a cycle or unreadable link that could not be fixed
		};

		uint32_t CurrentFrame()
		{
			return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
		}

		const RE::BSRenderPass* LoadLink(const RE::BSRenderPass* a_pass, bool a_groupLink, bool& a_fault) noexcept
		{
			__try {
				return a_groupLink ? a_pass->passGroupNext : a_pass->next;
			} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				a_fault = true;
				return nullptr;
			}
		}

		bool StoreLink(const RE::BSRenderPass* a_pass, bool a_groupLink, const RE::BSRenderPass* a_target) noexcept
		{
			__try {
				auto* pass = const_cast<RE::BSRenderPass*>(a_pass);
				(a_groupLink ? pass->passGroupNext : pass->next) = const_cast<RE::BSRenderPass*>(a_target);
				return true;
			} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				return false;
			}
		}

		bool CopyPassBytes(const RE::BSRenderPass* a_pass, uint8_t* a_out) noexcept
		{
			__try {
				memcpy(a_out, a_pass, kPassBytes);
				return true;
			} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				return false;
			}
		}

		bool IsPromoted(const RE::BSShadowLight* a_light)
		{
			return a_light && IsPromotedLight(const_cast<RE::BSShadowLight*>(a_light)->light.get());
		}

		std::string ModuleRelative(const void* a_address)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &module) ||
				!module)
				return std::format("{}", a_address);
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			return std::format("{}+0x{:X}", std::filesystem::path(path).filename().string(),
				reinterpret_cast<uintptr_t>(a_address) - reinterpret_cast<uintptr_t>(module));
		}

		std::string CaptureStack(ULONG a_skip)
		{
			void* frames[24]{};
			const USHORT count = CaptureStackBackTrace(a_skip, 24, frames, nullptr);
			std::string out;
			for (USHORT i = 0; i < count; ++i)
				out += std::format(" {}", ModuleRelative(frames[i]));
			return out;
		}

		void LogNodes(const RE::BSRenderPass* a_head, bool a_groupLink, bool a_rawBytes)
		{
			bool fault = false;
			const RE::BSRenderPass* pass = a_head;
			for (uint32_t i = 0; pass && i < kGuardLogNodes; ++i) {
				alignas(RE::BSRenderPass) uint8_t bytes[kPassBytes]{};
				if (!CopyPassBytes(pass, bytes)) {
					logger::warn("[SCM]   node[{}] {} unreadable", i, (const void*)pass);
					break;
				}
				const auto& snapshot = *reinterpret_cast<const RE::BSRenderPass*>(bytes);
				logger::warn("[SCM]   node[{}] {} enum={:X} geom={} prop={} pad44={:X}", i, (const void*)pass, snapshot.passEnum,
					(const void*)snapshot.geometry, (const void*)snapshot.shaderProperty, snapshot.pad44);
				if (a_rawBytes) {
					std::string hex;
					for (const uint8_t b : bytes)
						hex += std::format("{:02X}", b);
					logger::warn("[SCM]   node[{}] raw {}", i, hex);
				}
				pass = LoadLink(pass, a_groupLink, fault);
			}
		}

		void LogGuardEvent(const RE::BSRenderPass* a_head, bool a_groupLink, const char* a_what, uint32_t a_steps)
		{
			const uint32_t index = s_guardLogCount.fetch_add(1, std::memory_order_relaxed);
			if (index >= kGuardLogLimit)
				return;

			logger::warn("[SCM] Shadow render pass chain guard: {} ({} link, steps={}): light={} promoted={} head={} frame={}",
				a_what, a_groupLink ? "passGroupNext" : "next", a_steps, (void*)s_renderingLight.load(std::memory_order_relaxed),
				IsPromoted(s_renderingLight.load(std::memory_order_relaxed)), (const void*)a_head, CurrentFrame());
			logger::warn("[SCM]   stack:{}", CaptureStack(1));
			LogNodes(a_head, a_groupLink, index < kGuardDumpLimit);
		}

		void InjectRing(const RE::BSRenderPass* a_head)
		{
			bool fault = false;
			const RE::BSRenderPass* tail = a_head;
			for (uint32_t i = 0; i < kPassChainWalkCap; ++i) {
				const auto* next = LoadLink(tail, true, fault);
				if (!next)
					break;
				tail = next;
			}
			if (!fault && tail != a_head)
				StoreLink(tail, true, a_head);
		}

		LinkResult ValidateLink(const RE::BSRenderPass* a_head, bool a_groupLink, uint32_t& a_steps, uint32_t& a_length)
		{
			bool fault = false;
			const auto nextOf = [&](const RE::BSRenderPass* pass) { return LoadLink(pass, a_groupLink, fault); };

			uint32_t steps = 0;
			const auto verdict = PassChainGuard::Walk(a_head, nextOf, kPassChainWalkCap, &steps);
			a_steps += steps;
			a_length = steps;
			if (fault) {
				LogGuardEvent(a_head, a_groupLink, "unreadable link, skipped", steps);
				s_passGuardFaultSkipsTotal.fetch_add(1, std::memory_order_relaxed);
				return LinkResult::Unrepairable;
			}
			if (verdict == PassChainGuard::Verdict::CapExceeded) {
				s_passGuardCapExceededTotal.fetch_add(1, std::memory_order_relaxed);
				return LinkResult::Clean;
			}
			if (verdict == PassChainGuard::Verdict::Clean)
				return LinkResult::Clean;

			LogGuardEvent(a_head, a_groupLink, "cycle found", steps);
			const auto* closing = PassChainGuard::FindCycleClosingNode(a_head, nextOf, kPassChainWalkCap);
			uint32_t repairedSteps = 0;
			if (closing && !fault && StoreLink(closing, a_groupLink, nullptr) &&
				PassChainGuard::Walk(a_head, nextOf, kPassChainWalkCap, &repairedSteps) != PassChainGuard::Verdict::Cycle && !fault) {
				logger::warn("[SCM]   repaired: cut the link of {} ({} nodes now reachable)", (const void*)closing, repairedSteps);
				a_length = repairedSteps;
				s_passGuardCycleRepairsTotal.fetch_add(1, std::memory_order_relaxed);
				if (IsPromoted(s_renderingLight.load(std::memory_order_relaxed)))
					s_passGuardRepairsPromotedTotal.fetch_add(1, std::memory_order_relaxed);
				return LinkResult::Repaired;
			}
			s_passGuardCycleSkipsTotal.fetch_add(1, std::memory_order_relaxed);
			return LinkResult::Unrepairable;
		}

		struct ShadowRenderWindowScope
		{
			explicit ShadowRenderWindowScope(RE::BSShadowLight* a_light)
			{
				s_passGuardTripsThisRender.store(0, std::memory_order_relaxed);
				s_renderingLight.store(a_light, std::memory_order_relaxed);
				s_cursorExpected = nullptr;
				s_cursorRemaining = 0;
				s_shadowRenderWindow.store(true, std::memory_order_relaxed);
			}
			~ShadowRenderWindowScope()
			{
				s_shadowRenderWindow.store(false, std::memory_order_relaxed);
				s_renderingLight.store(nullptr, std::memory_order_relaxed);
			}
		};

		struct Registration
		{
			const void* renderer;
			const void* light;
			uint32_t frame;
			DWORD thread;
		};
		// Registrations arrive from several job threads at once.
		std::mutex s_registrationsMutex;
		std::unordered_map<const RE::BSRenderPass*, Registration> s_registrations;

		void CheckRegistration(const RE::BSRenderPass* a_pass, const RE::BSRenderPass* a_preLinked, const char* a_what,
			const void* a_renderer, uint32_t a_technique)
		{
			if (!a_pass)
				return;
			s_passRegChecksTotal.fetch_add(1, std::memory_order_relaxed);

			const DWORD thread = GetCurrentThreadId();
			std::string history = "no earlier registration seen";
			{
				std::scoped_lock lock(s_registrationsMutex);
				if (const auto it = s_registrations.find(a_pass); it != s_registrations.end()) {
					const Registration& previous = it->second;
					history = std::format("registered into {} renderer {} {} frames ago by thread {} (light {})",
						previous.renderer == a_renderer ? "THIS" : "ANOTHER", previous.renderer, CurrentFrame() - previous.frame, previous.thread, previous.light);
				}
				s_registrations[a_pass] = { a_renderer, CurrentCullLight(), CurrentFrame(), thread };
				PruneIfOversized(s_registrations, kRegistrationHistoryCap);
			}

			bool fault = false;
			const auto nextOf = [&](const RE::BSRenderPass* pass) { return LoadLink(pass, true, fault); };
			uint32_t steps = 0;
			if (PassChainGuard::Walk(a_pass, nextOf, kRegistrationWalkCap, &steps) != PassChainGuard::Verdict::Cycle)
				return;

			s_passRegRingsTotal.fetch_add(1, std::memory_order_relaxed);
			if (IsPromoted(CurrentCullLight()))
				s_passRegRingsPromotedTotal.fetch_add(1, std::memory_order_relaxed);
			if (s_registrationLogCount.fetch_add(1, std::memory_order_relaxed) >= kRegistrationLogLimit)
				return;
			logger::warn("[SCM] {} formed a passGroupNext ring: renderer={} technique={:X} pass={} previouslyLinkedTo={} accumulatingLight={} (promoted={}) renderingLight={} (promoted={}) frame={} sessionResetPending={}",
				a_what, a_renderer, a_technique, (const void*)a_pass, (const void*)a_preLinked, (void*)CurrentCullLight(),
				IsPromoted(CurrentCullLight()), (void*)s_renderingLight.load(std::memory_order_relaxed),
				IsPromoted(s_renderingLight.load(std::memory_order_relaxed)), CurrentFrame(), s_pendingSessionReset.load(std::memory_order_relaxed));
			logger::warn("[SCM]   history: {}; thread now={}; rebuildAttach={} cullPassMode={}", history, thread,
				s_accumRebuildAttach.load(std::memory_order_relaxed), s_cullPassMode.load(std::memory_order_relaxed));
			logger::warn("[SCM]   stack:{}", CaptureStack(2));
			LogNodes(a_pass, true, false);
		}

		struct Hook_RegisterPassSorted
		{
			static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_technique)
			{
				if (!s_traceRegistration.load(std::memory_order_relaxed))
					return func(a_this, a_pass, a_technique);
				const auto* previouslyLinked = a_pass ? a_pass->passGroupNext : nullptr;
				func(a_this, a_pass, a_technique);
				CheckRegistration(a_pass, previouslyLinked, "RegisterPassSorted", a_this, a_technique);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Hook_RegisterPass
		{
			static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_technique)
			{
				if (!s_traceRegistration.load(std::memory_order_relaxed))
					return func(a_this, a_pass, a_technique);
				const auto* previouslyLinked = a_pass ? a_pass->passGroupNext : nullptr;
				func(a_this, a_pass, a_technique);
				CheckRegistration(a_pass, previouslyLinked, "RegisterPass", a_this, a_technique);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		std::pair<std::uintptr_t, std::uintptr_t> GameImageRange()
		{
			const auto base = REL::Module::get().base();
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
			return { base, base + nt->OptionalHeader.SizeOfImage };
		}

		bool PassShaderVtableInModule(const RE::BSRenderPass* a_pass, std::uintptr_t base, std::uintptr_t end)
		{
			__try {
				const auto* shader = *reinterpret_cast<const std::uintptr_t* const*>(a_pass);
				const auto vtable = shader ? *shader : 0;
				return vtable >= base && vtable < end;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct Hook_SetupAndDrawPass
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
			{
				static const auto [imageBase, imageEnd] = GameImageRange();
				if (InShadowRenderWindow() && a_pass && !PassShaderVtableInModule(a_pass, imageBase, imageEnd)) {
					s_passGuardFaultSkipsTotal.fetch_add(1, std::memory_order_relaxed);
					s_passGuardTripsThisRender.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallPassRegistrationHooks()
	{
		if (const long rc = stl::detour_thunk<Hook_SetupAndDrawPass>(REL::RelocationID(100854, 107644)); rc != 0)
			logger::error("[SCM] SetupAndDrawPass entry guard not installed (Detours error {})", rc);
		stl::write_vfunc<0x01, Hook_RegisterPassSorted>(RE::VTABLE_BSBatchRenderer[0]);
		stl::write_vfunc<0x02, Hook_RegisterPass>(RE::VTABLE_BSBatchRenderer[0]);
	}

	void SetPassRegistrationTrace(bool a_enabled)
	{
		s_traceRegistration.store(a_enabled, std::memory_order_relaxed);
	}

	void NoteRenderSkipped(RenderSkipReason a_reason)
	{
		s_renderSkipByReason[static_cast<size_t>(a_reason)].fetch_add(1, std::memory_order_relaxed);
		s_lastRenderSkipFrame.store(CurrentFrame(), std::memory_order_relaxed);
		s_lastRenderSkipReason.store(static_cast<size_t>(a_reason), std::memory_order_relaxed);
	}

	bool InShadowRenderWindow()
	{
		return s_shadowRenderWindow.load(std::memory_order_relaxed);
	}

	void ForcePassGuardTrips(uint32_t a_count)
	{
		s_forcedSkips.store((std::min)(a_count, 1000u), std::memory_order_relaxed);
	}

	void ForcePassGuardRings(uint32_t a_count)
	{
		s_forcedRings.store((std::min)(a_count, 1000u), std::memory_order_relaxed);
	}

	bool RejectCyclicPassChain(const RE::BSRenderPass* a_head)
	{
		if (!a_head)
			return false;
		s_passGuardChecksTotal.fetch_add(1, std::memory_order_relaxed);

		if (s_cursorRemaining > 0 && a_head == s_cursorExpected) {
			bool fault = false;
			s_cursorExpected = LoadLink(a_head, true, fault);
			s_cursorRemaining = (s_cursorExpected && !fault) ? s_cursorRemaining - 1 : 0;
			return false;
		}

		bool skip = false;
		if (auto forced = s_forcedSkips.load(std::memory_order_relaxed); forced > 0) {
			s_forcedSkips.store(forced - 1, std::memory_order_relaxed);
			s_passGuardCycleSkipsTotal.fetch_add(1, std::memory_order_relaxed);
			LogGuardEvent(a_head, true, "forced skip", 0);
			skip = true;
		} else {
			if (auto rings = s_forcedRings.load(std::memory_order_relaxed); rings > 0) {
				s_forcedRings.store(rings - 1, std::memory_order_relaxed);
				InjectRing(a_head);
			}
			// Cut cycles in place: skipping a cyclic chain would only make the engine's per-pass loop spin faster.
			uint32_t steps = 0;
			uint32_t groupLength = 0;
			for (const bool groupLink : { true, false }) {
				uint32_t length = 0;
				if (ValidateLink(a_head, groupLink, steps, length) == LinkResult::Unrepairable)
					skip = true;
				else if (groupLink)
					groupLength = length;
			}
			if (!skip && groupLength > 1) {
				bool fault = false;
				s_cursorExpected = LoadLink(a_head, true, fault);
				s_cursorRemaining = fault ? 0 : (std::min)(groupLength, kPassChainWalkCap) - 1;
			}
		}

		if (skip)
			s_passGuardTripsThisRender.fetch_add(1, std::memory_order_relaxed);
		return skip;
	}

	static bool ClearRendererGuarded(RE::BSBatchRenderer* a_renderer)
	{
		__try {
			GameClearAllRenderPasses(a_renderer);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			s_passGuardFaultSkipsTotal.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	bool ClearStaleAccumulatedPasses(RE::BSShadowLight* a_light)
	{
		s_stalePassClearsTotal.fetch_add(1, std::memory_order_relaxed);
		bool cleared = true;
		const auto clearAccumulator = [&cleared](RE::BSShaderAccumulator* a_accumulator) {
			if (a_accumulator)
				if (auto* renderer = a_accumulator->GetRuntimeData().batchRenderer)
					cleared &= ClearRendererGuarded(renderer);
		};
		if (globals::game::isVR) {
			for (auto& desc : a_light->GetVRRuntimeData().shadowmapDescriptors)
				for (auto& accumulator : desc.shaderAccumulator)
					clearAccumulator(accumulator.get());
		} else {
			for (auto& desc : a_light->GetRuntimeData().shadowmapDescriptors)
				clearAccumulator(desc.shaderAccumulator.get());
		}
		return cleared;
	}

	bool RenderLightGuarded(RE::BSShadowLight* a_light, uint32_t& a_index)
	{
		ShadowRenderWindowScope scope(a_light);
		a_light->Render(a_index);
		const bool complete = s_passGuardTripsThisRender.load(std::memory_order_relaxed) == 0;
		if (complete) {
			s_lightRenderFrame[a_light] = CurrentFrame();
			PruneIfOversized(s_lightRenderFrame, 512);
		}
		return complete;
	}
}
