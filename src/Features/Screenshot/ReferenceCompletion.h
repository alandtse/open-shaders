#pragma once

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include "Features/Screenshot/SequencePolicy.h"
#	include <functional>
#	include <nlohmann/json.hpp>

namespace OS::Capture
{
	/** Reconstructs the provider notification from an immutable terminal receipt. */
	inline nlohmann::json ReferenceCompletion(const nlohmann::json& receipt)
	{
		const auto& reference = receipt.at("reference");
		const bool success = receipt.at("state") == "completed" && receipt.at("written") == 1 && reference.contains("artifact");
		nlohmann::json result = { { "requestId", reference.at("requestId") }, { "ok", success },
			{ "path", reference.at("path") }, { "uiExcluded", false } };
		if (success) {
			for (const auto* key : { "width", "height", "bytes", "sha256" })
				result[key] = reference.at("artifact").at(key);
		} else {
			const auto error = receipt.value("error", std::string{});
			result["error"] = error.empty() ? "reference capture did not complete" : error;
		}
		return result;
	}

	/** Re-emits completed reference requests without scheduling or republishing an image. */
	inline void ReplayReferenceCompletion(const nlohmann::json& response, const std::function<void(const nlohmann::json&)>& completion)
	{
		if (!response.value("ok", false) || !response.contains("result") || !response.at("result").is_object())
			return;
		const auto& receipt = response.at("result");
		if (!receipt.value("idempotentReplay", false) || !receipt.contains("reference"))
			return;
		const auto state = receipt.value("state", std::string{});
		if (IsTerminalState(state))
			completion(ReferenceCompletion(receipt));
	}
}
#endif
