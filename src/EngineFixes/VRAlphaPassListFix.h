#pragma once

#include "EngineFix.h"

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <nlohmann/json_fwd.hpp>
#endif

/** Preserve pending linked alpha passes when the VR engine submits a duplicate. */
struct VRAlphaPassListFix : EngineFix
{
	std::string GetName() override { return "VR Alpha Pass List Fix"; }
	/** Install the verified VR append and queue-retirement hooks. */
	void Install() override;

#ifdef DEVBENCH_BRIDGE_ENABLED
	/** Inspect or temporarily disable protection for a controlled runtime comparison. */
	static nlohmann::json Control(const nlohmann::json& a_args);
#endif
};
