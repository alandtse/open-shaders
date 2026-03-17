#include "VRAPI/CSinterface001.h"

// Stores the API after it has already been fetched.
CSPluginAPI::ICSInterface001* g_CSInterface = nullptr;

// Fetches the interface to use from Community Shaders.
CSPluginAPI::ICSInterface001* CSPluginAPI::GetCSInterface001()
{
	// If the interface has already been fetched, return the same object.
	if (g_CSInterface) {
		return g_CSInterface;
	}

	// Dispatch a message to get the plugin interface from Community Shaders.
	CSMessage csMessage;
	const auto skseMessaging = SKSE::GetMessagingInterface();
	if (!skseMessaging) {
		return nullptr;
	}

	if (!skseMessaging->Dispatch(CSMessage::kMessage_GetInterface, static_cast<void*>(&csMessage), sizeof(CSMessage), CSPluginName)) {
		return nullptr;
	}

	if (!csMessage.GetApiFunction) {
		return nullptr;
	}

	// Fetch the API for this version of the CS interface.
	void* api = csMessage.GetApiFunction(CSInterfaceRevision);
	if (!api) {
		return nullptr;
	}

	g_CSInterface = static_cast<ICSInterface001*>(api);
	return g_CSInterface;
}
