#include "Features/LightLimitFix/LightAttach.h"

#include "Features/LightLimitFix.h"
#include "Features/LightLimitFix/LightAttachGate.h"
#include "Globals.h"
#include "State.h"
#include "Utils/VersionedRelocation.h"

#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace LightAttach
{
	namespace
	{
		// The per-frame dynamic light call: inlined in ShadowSceneNode::ProcessQueuedLights on AE (same offset on 1.6 and 1.7).
		constexpr std::uintptr_t kDynamicEnableCallOffset = 0x6E2;
		// On SE 1.5.97 and VR 1.4.15 the same call sits in ShadowSceneNode::PurgeStaleLights, which the address library does not cover on VR.
		constexpr std::uintptr_t kSePurgeEnableCallRva = 0x12BE843;
		constexpr std::uintptr_t kVrPurgeEnableCallRva = 0x12FC2C3;
		// BSLight::frustrumCull value that marks a parabolic shadow caster.
		constexpr std::uint32_t kParabolicShadowCaster = 0xFF;

		std::mutex trackerLock;
		LightAttachGate::Tracker tracker;
		std::atomic<bool> tracking{ false };
		std::atomic<bool> installed{ false };
		std::atomic<std::uint64_t> walked{ 0 };
		std::atomic<std::uint64_t> skipped{ 0 };
		std::atomic<bool> auditDetail{ false };

		/** @brief Captures what the attach walk reads from the light and its scene node. */
		LightAttachGate::Signature MakeSignature(RE::ShadowSceneNode& a_node, RE::BSLight& a_light)
		{
			LightAttachGate::Signature signature{};
			std::size_t next = 0;
			const auto append = [&](const auto& a_value) {
				static_assert(sizeof(a_value) % sizeof(std::uint32_t) == 0);
				std::memcpy(&signature[next], &a_value, sizeof(a_value));
				next += sizeof(a_value) / sizeof(std::uint32_t);
			};

			RE::NiLight* niLight = a_light.light.get();
			append(niLight);
			if (niLight) {
				const auto& runtime = niLight->GetLightRuntimeData();
				append(niLight->world.translate);
				append(runtime.radius);
				append(runtime.fade);
				append(niLight->GetFlags());
			}
			append(a_light.worldTranslate);
			append(a_light.portalGraph);
			append(a_node.GetRuntimeData().portalGraph);
			return signature;
		}

		std::uint64_t Mix(std::uintptr_t a_value)
		{
			auto x = static_cast<std::uint64_t>(a_value) + 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		/** @brief Order-independent digests of one group of lights and what the engine attached to them. */
		struct Digest
		{
			std::uint64_t set = 0;
			std::uint64_t roomsAndPortals = 0;
			std::uint64_t geometry = 0;
			std::uint64_t accumulation = 0;
			std::uint64_t geometryEntries = 0;
			std::uint64_t accumulationEntries = 0;
			std::uint32_t lights = 0;
			std::uint32_t emptyGeometryLists = 0;
		};

		void AddToDigest(Digest& a_digest, RE::BSLight& a_light, const RE::BSTArray<RE::NiPointer<RE::NiAVObject>>* a_accumulation, json* a_details)
		{
			++a_digest.lights;
			const auto key = Mix(reinterpret_cast<std::uintptr_t>(&a_light));
			a_digest.set ^= key;

			auto rooms = key;
			for (const auto* room : a_light.rooms)
				rooms += Mix(reinterpret_cast<std::uintptr_t>(room));
			for (const auto* portal : a_light.portals)
				rooms += 3 * Mix(reinterpret_cast<std::uintptr_t>(portal));
			const auto roomsHash = Mix(rooms);
			a_digest.roomsAndPortals ^= roomsHash;

			auto attached = key;
			std::uint32_t count = 0;
			for (const auto& geometryNode : a_light.geomList) {
				if (geometryNode) {
					attached += Mix(reinterpret_cast<std::uintptr_t>(geometryNode.get()));
					++count;
				}
			}
			a_digest.geometry ^= Mix(attached);
			a_digest.geometryEntries += count;
			a_digest.emptyGeometryLists += count == 0 ? 1 : 0;

			if (a_accumulation) {
				auto accumulated = key;
				for (const auto& root : *a_accumulation) {
					if (root) {
						accumulated += Mix(reinterpret_cast<std::uintptr_t>(root.get()));
						++a_digest.accumulationEntries;
					}
				}
				a_digest.accumulation ^= Mix(accumulated);
			}

			if (a_details) {
				const auto* niLight = a_light.light.get();
				const auto position = niLight ? niLight->world.translate : RE::NiPoint3{};
				a_details->push_back(json::array({ std::format("{:x}", reinterpret_cast<std::uintptr_t>(&a_light)), std::format("{:016x}", roomsHash), position.x, position.y, position.z, a_light.rooms.size(), a_light.portals.size(), count }));
			}
		}

		json DigestToJson(const Digest& a_digest)
		{
			return json{
				{ "lights", a_digest.lights },
				{ "emptyGeometryLists", a_digest.emptyGeometryLists },
				{ "geometryEntries", a_digest.geometryEntries },
				{ "accumulationEntries", a_digest.accumulationEntries },
				{ "setDigest", std::format("{:016x}", a_digest.set) },
				{ "roomsAndPortalsDigest", std::format("{:016x}", a_digest.roomsAndPortals) },
				{ "geometryDigest", std::format("{:016x}", a_digest.geometry) },
				{ "accumulationDigest", std::format("{:016x}", a_digest.accumulation) },
			};
		}

		/** @brief Digests of the world scene node's non-shadow and shadow lights, for comparing the gate on and off. */
		json AuditLights()
		{
			Digest nonShadow;
			Digest shadow;
			json details = json::array();
			json shadowDetails = json::array();
			const bool detailed = auditDetail.load(std::memory_order_relaxed);
			json* detailsOut = detailed ? &details : nullptr;
			const auto node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			if (node) {
				auto& runtime = node->GetRuntimeData();
				for (const auto& light : runtime.activeLights) {
					if (light && !light->IsShadowLight())
						AddToDigest(nonShadow, *light, nullptr, detailsOut);
				}
				for (const auto& light : runtime.activeShadowLights) {
					if (light)
						AddToDigest(shadow, *light, globals::game::isVR ? nullptr : &light->GetRuntimeData().sceneAccumArray, detailed ? &shadowDetails : nullptr);
				}
			}
			json audit{ { "nonShadow", DigestToJson(nonShadow) }, { "shadow", DigestToJson(shadow) } };
			if (detailed) {
				audit["nonShadowLights"] = std::move(details);
				audit["shadowLights"] = std::move(shadowDetails);
			}
			return audit;
		}

		struct DynamicLightEnable
		{
			static void thunk(RE::ShadowSceneNode* a_node, RE::BSLight* a_light)
			{
				auto& feature = globals::features::lightLimitFix;
				if (!feature.loaded || !feature.settings.SkipRedundantLightAttach) {
					if (tracking.exchange(false)) {
						std::lock_guard lock(trackerLock);
						tracker.Clear();
					}
					func(a_node, a_light);
					return;
				}

				if (!a_node || !a_light || a_light->frustrumCull == kParabolicShadowCaster || a_light->IsShadowLight()) {
					if (a_light) {
						std::lock_guard lock(trackerLock);
						tracker.Forget(a_light);
					}
					func(a_node, a_light);
					return;
				}

				tracking = true;
				bool walk;
				{
					const auto frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
					const auto refreshFrames = static_cast<std::uint32_t>(feature.settings.LightAttachRefreshFrames);
					const auto before = MakeSignature(*a_node, *a_light);
					std::lock_guard lock(trackerLock);
					walk = tracker.ShouldWalk(a_light, before, frame, refreshFrames);
				}
				if (!walk) {
					skipped.fetch_add(1, std::memory_order_relaxed);
					return;
				}

				func(a_node, a_light);
				walked.fetch_add(1, std::memory_order_relaxed);
				const auto after = MakeSignature(*a_node, *a_light);
				std::lock_guard lock(trackerLock);
				tracker.RecordWalk(a_light, after);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool IsSupported()
	{
		return installed.load(std::memory_order_relaxed);
	}

	void Install()
	{
		const auto callSite = REL::Module::IsAE() ?
		                          REL::RelocationID(99701, 106335).address() + kDynamicEnableCallOffset :
		                          REL::Offset(REL::Module::IsVR() ? kVrPurgeEnableCallRva : kSePurgeEnableCallRva).address();
		if (!Util::VersionedRelocation::IsCallTo(callSite, REL::RelocationID(99708, 106342).address())) {
			logger::warn("[LLF] Light attach gate not applied: unexpected code at the per-frame light enable (another mod or an unverified runtime)");
			return;
		}
		stl::write_thunk_call<DynamicLightEnable>(callSite);
		installed = true;
		logger::info("[LLF] Installed light attach gate");
	}

	void ResetTracking()
	{
		std::lock_guard lock(trackerLock);
		tracker.Clear();
	}

	bool GetAuditDetail()
	{
		return auditDetail.load(std::memory_order_relaxed);
	}

	void SetAuditDetail(bool a_enabled)
	{
		auditDetail.store(a_enabled, std::memory_order_relaxed);
	}

	json GetDiagnostics()
	{
		std::size_t trackedLights;
		{
			std::lock_guard lock(trackerLock);
			trackedLights = tracker.Size();
		}
		return json{
			{ "lightAttachSupported", IsSupported() },
			{ "lightAttachWalked", walked.load(std::memory_order_relaxed) },
			{ "lightAttachSkipped", skipped.load(std::memory_order_relaxed) },
			{ "lightAttachTracked", trackedLights },
			{ "lightAttachAudit", AuditLights() },
		};
	}
}
