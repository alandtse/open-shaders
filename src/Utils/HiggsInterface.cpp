#include "HiggsInterface.h"

#include "Globals.h"

namespace Util::Higgs
{
	namespace
	{
		constexpr std::uint32_t GetInterfaceMessage = 0xF9279A57;
		constexpr unsigned int InterfaceRevision = 1;
		constexpr const char* PluginName = "HIGGS";
		constexpr unsigned int MinBuildNumber = 1060000;

		struct Message
		{
			void* (*GetApiFunction)(unsigned int a_revision) = nullptr;
		};
	}

	Interface001* GetInterface()
	{
		static Interface001* higgs = nullptr;
		static bool fetched = false;
		if (fetched || !globals::game::isVR)
			return higgs;
		fetched = true;

		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging)
			return nullptr;

		Message message;
		if (!messaging->Dispatch(GetInterfaceMessage, &message, sizeof(Message*), PluginName) || !message.GetApiFunction) {
			logger::info("[BodyCapsules] HIGGS is not available");
			return nullptr;
		}

		higgs = static_cast<Interface001*>(message.GetApiFunction(InterfaceRevision));
		if (higgs && higgs->GetBuildNumber() < MinBuildNumber) {
			logger::warn("[BodyCapsules] HIGGS build {} is older than {}, so its held weapons and objects are not used", higgs->GetBuildNumber(), MinBuildNumber);
			higgs = nullptr;
		}
		if (higgs)
			logger::info("[BodyCapsules] Using HIGGS build {} for held weapons and objects", higgs->GetBuildNumber());
		return higgs;
	}
}
