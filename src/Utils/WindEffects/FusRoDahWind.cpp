#include "FusRoDahWind.h"

#include "Features/CSUtility.h"
#include "State.h"

#include <array>
#include <cmath>
#include <numbers>

namespace FusRoDahWind
{
	namespace
	{
		constexpr RE::FormID kUnrelentingForceFormID = 0x13E07;
		constexpr std::string_view kSkyrimMaster = "Skyrim.esm";

		struct RankProfile
		{
			float strength;
			float maxDistance;
			float waveHalfWidth;
			float propagationSpeed;
		};

		constexpr std::array<RankProfile, RE::TESShout::VariationIDs::kTotal> kRankProfiles{
			RankProfile{ 0.9f, 1000.0f, 180.0f, 1500.0f },
			RankProfile{ 1.5f, 1800.0f, 260.0f, 1800.0f },
			RankProfile{ 2.3f, 2800.0f, 360.0f, 2100.0f }
		};

		class SpellCastEventSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
				RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!a_event || !a_event->object || !unrelentingForce) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto& settings = CSUtility::GetSingleton()->settings;
				if (!settings.enableFusRoDahWind || settings.fusRoDahIntensity <= 0.0f) {
					return RE::BSEventNotifyControl::kContinue;
				}

				std::size_t rank = kRankProfiles.size();
				for (std::size_t index = 0; index < kRankProfiles.size(); ++index) {
					const auto* spell = unrelentingForce->variations[index].spell;
					if (spell && spell->GetFormID() == a_event->spell) {
						rank = index;
						break;
					}
				}
				if (rank == kRankProfiles.size()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				auto* actor = a_event->object->As<RE::Actor>();
				if (!actor) {
					return RE::BSEventNotifyControl::kContinue;
				}

				RE::NiPoint3 origin = actor->GetPosition();
				bool foundMagicNode = false;
				if (auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kOther)) {
					if (auto* magicNode = caster->GetMagicNode()) {
						origin = magicNode->world.translate;
						foundMagicNode = true;
					}
				}
				if (!foundMagicNode) {
					origin.z += (actor->GetBoundMax().z - actor->GetBoundMin().z) * 0.7f;
				}

				float aimAngle = actor->GetAimAngle();
				float aimHeading = actor->GetAimHeading();
				if (!std::isfinite(aimAngle)) {
					aimAngle = actor->GetAngleX();
				}
				if (!std::isfinite(aimHeading)) {
					aimHeading = actor->GetAngleZ();
				}
				const float horizontalScale = std::cos(aimAngle);
				const RE::NiPoint3 forward{
					horizontalScale * std::sin(aimHeading),
					horizontalScale * std::cos(aimHeading),
					-std::sin(aimAngle)
				};
				const auto& profile = kRankProfiles[rank];
				const float coneHalfAngleRadians =
					settings.fusRoDahConeHalfAngle * (std::numbers::pi_v<float> / 180.0f);
				State::GetSingleton()->QueueTransientWindImpulse({ { origin.x, origin.y, origin.z },
					0.0f,
					{ forward.x, forward.y, forward.z },
					profile.strength * settings.fusRoDahIntensity,
					profile.maxDistance * settings.fusRoDahDistanceMultiplier,
					profile.waveHalfWidth * settings.fusRoDahWidthMultiplier,
					profile.propagationSpeed * settings.fusRoDahSpeedMultiplier,
					std::cos(coneHalfAngleRadians),
					settings.fusRoDahDecayTime,
					{} });
				logger::info(
					"Queued Unrelenting Force wind impulse: rank {}, strength {:.3f}, origin ({:.1f}, {:.1f}, {:.1f}), "
					"direction ({:.3f}, {:.3f}, {:.3f}), tree influence {:.2f}, transient bend limit {:.2f}",
					rank + 1, profile.strength * settings.fusRoDahIntensity,
					origin.x, origin.y, origin.z, forward.x, forward.y, forward.z,
					settings.treeTransientWindInfluence, settings.treeTransientMaximumBendMultiplier);
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::TESShout* unrelentingForce{};
		};
	}

	void Register()
	{
		static bool registered = false;
		if (registered) {
			return;
		}

		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		auto* eventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!dataHandler || !eventSourceHolder) {
			logger::warn("Unable to register Unrelenting Force wind impulses: game data is unavailable");
			return;
		}

		static SpellCastEventSink sink;
		sink.unrelentingForce = dataHandler->LookupForm<RE::TESShout>(kUnrelentingForceFormID, kSkyrimMaster);
		if (!sink.unrelentingForce) {
			logger::warn("Unable to register Unrelenting Force wind impulses: shout record not found");
			return;
		}

		eventSourceHolder->AddEventSink<RE::TESSpellCastEvent>(&sink);
		registered = true;
		logger::info("Registered Unrelenting Force wind impulses");
	}
}
