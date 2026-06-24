// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Client-side handshake stub. Compiled into the client mod's binary. Dispatches
// an SKSE message to the helper and caches the resulting interface pointer.
// The handshake is RETRYABLE: an early call can return null if the helper's
// messaging listener isn't registered yet (plugin load-order race), so failure
// is NOT latched — keep calling (e.g. once per frame until Connect succeeds).

#include "ImGuiVRHelperAPI.h"

namespace ImGuiVRHelperPluginAPI
{
	namespace
	{
		IImGuiVRHelperInterface001* g_interface001 = nullptr;
	}

	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001()
	{
		if (g_interface001) {
			return g_interface001;
		}

		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			return nullptr;
		}

		Message msg{};
		messaging->Dispatch(
			Message::kMessage_GetInterface,
			static_cast<void*>(&msg),
			sizeof(Message*),
			kPluginName);

		if (!msg.GetApiFunction) {
			return nullptr;  // helper not ready yet — safe to retry next call
		}

		g_interface001 = static_cast<IImGuiVRHelperInterface001*>(msg.GetApiFunction(1));
		return g_interface001;
	}
}
