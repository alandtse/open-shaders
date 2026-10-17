#pragma once

#include "StringUtils.h"

#include <string_view>

namespace Util
{
	/**
	 * Whether an ImGui tab's full name answers to a label a caller gave.
	 * A tab name may carry a hidden id after "###" and extra text after "##". Ignoring case, the label
	 * matches the id (with or without a leading "###") or the visible text without any trailing " [...]"
	 * suffix such as a release stage tag, so "Neural Rendering" and "NeuralRendering" both find
	 * "Neural Rendering [ALPHA]###NeuralRendering".
	 *
	 * @param tabName  The tab's full name as ImGui stores it.
	 * @param label  Visible text or id to look for.
	 */
	inline bool TabLabelMatches(std::string_view tabName, std::string_view label)
	{
		constexpr std::string_view kIdMarker = "###";
		if (label.starts_with(kIdMarker))
			label.remove_prefix(kIdMarker.size());
		if (label.empty())
			return false;
		const auto hashes = tabName.find(kIdMarker);
		if (hashes != std::string_view::npos && IEquals(tabName.substr(hashes + kIdMarker.size()), label))
			return true;
		auto visible = tabName.substr(0, tabName.find("##"));
		if (const auto bracket = visible.find(" ["); bracket != std::string_view::npos)
			visible = visible.substr(0, bracket);
		return IEquals(visible, label);
	}
}
