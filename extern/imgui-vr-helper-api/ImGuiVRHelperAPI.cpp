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
		// One handshake per process; both GetImGuiVRHelperInterface00N
		// share it. The helper's GetApiFunction is called once per
		// requested revision and the results cached.
		bool g_handshake_attempted = false;
		IImGuiVRHelperInterface001* g_interface001 = nullptr;
		IImGuiVRHelperInterface002* g_interface002 = nullptr;

		typedef void* (*GetApiFunctionFn)(uint32_t);
		GetApiFunctionFn g_get_api_function = nullptr;

		// Performs the SKSE messaging dispatch and caches the helper's
		// GetApiFunction pointer. No-op after first call.
		bool RunHandshake()
		{
			if (g_handshake_attempted) {
				return g_get_api_function != nullptr;
			}
			g_handshake_attempted = true;

			const auto* messaging = SKSE::GetMessagingInterface();
			if (!messaging) {
				return false;
			}

			Message msg{};
			messaging->Dispatch(
				Message::kMessage_GetInterface,
				static_cast<void*>(&msg),
				sizeof(Message*),
				kPluginName);

			g_get_api_function = msg.GetApiFunction;
			return g_get_api_function != nullptr;
		}
	}

	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001()
	{
		if (!g_interface001 && RunHandshake()) {
			g_interface001 = static_cast<IImGuiVRHelperInterface001*>(g_get_api_function(1));
		}
		return g_interface001;
	}

	IImGuiVRHelperInterface002* GetImGuiVRHelperInterface002()
	{
		if (!g_interface002 && RunHandshake()) {
			g_interface002 = static_cast<IImGuiVRHelperInterface002*>(g_get_api_function(2));
		}
		return g_interface002;
	}
}
