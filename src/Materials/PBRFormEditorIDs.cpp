#include "PBRFormEditorIDs.h"

#include <string>
#include <unordered_map>

namespace
{
	std::unordered_map<uint32_t, std::string> formEditorIDs;
}

const char* PBRRecords::GetFormEditorID(uint32_t a_formID)
{
	auto it = formEditorIDs.find(a_formID);
	return it == formEditorIDs.end() ? "" : it->second.c_str();
}

void PBRRecords::SetFormEditorID(uint32_t a_formID, const char* a_editorID)
{
	formEditorIDs[a_formID] = a_editorID;
}
