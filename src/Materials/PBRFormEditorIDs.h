#pragma once

#include <cstdint>

namespace PBRRecords
{
	/** @brief Resolve the Skyrim form editor IDs observed by the common PBR hooks. */
	const char* GetFormEditorID(uint32_t a_formID);
	/** @brief Record an engine-provided editor ID for the common PBR hooks. */
	void SetFormEditorID(uint32_t a_formID, const char* a_editorID);
}
