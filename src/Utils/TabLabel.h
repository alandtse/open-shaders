#pragma once

#include "StringUtils.h"

#include <string_view>

namespace Util
{
	/**
	 * Whether an ImGui tab's full name answers to a label a caller gave.
	 * A tab name may carry a hidden id after "###" and extra text after "##". The label matches the id
	 * exactly, or the visible text ignoring case and any trailing " [...]" suffix such as a release
	 * stage tag, so "Neural Rendering" finds "Neural Rendering [ALPHA]###NeuralRendering".
	 *
	 * @param tabName  The tab's full name as ImGui stores it.
	 * @param label  Visible text or "###" id to look for.
	 */
	inline bool TabLabelMatches(std::string_view tabName, std::string_view label)
	{
		if (label.empty())
			return false;
		const auto hashes = tabName.find("###");
		if (hashes != std::string_view::npos && tabName.substr(hashes + 3) == label)
			return true;
		auto visible = tabName.substr(0, tabName.find("##"));
		if (const auto bracket = visible.find(" ["); bracket != std::string_view::npos)
			visible = visible.substr(0, bracket);
		return IEquals(visible, label);
	}
}
