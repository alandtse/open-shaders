// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Client-side handshake stub. Compiled into the client mod's binary.
// Sends one SKSE message at first call, caches the resulting interface
// pointer, and returns it on every subsequent call.

#include "ImGuiVRHelperAPI.h"

namespace ImGuiVRHelperPluginAPI
{
	namespace
	{
		IImGuiVRHelperInterface001* g_interface001 = nullptr;
		bool g_handshake_attempted = false;
	}

	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001()
	{
		if (g_interface001 || g_handshake_attempted) {
			return g_interface001;
		}
		g_handshake_attempted = true;

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
			return nullptr;
		}

		g_interface001 = static_cast<IImGuiVRHelperInterface001*>(msg.GetApiFunction(1));
		return g_interface001;
	}
}
