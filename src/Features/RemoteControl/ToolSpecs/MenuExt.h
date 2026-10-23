#pragma once

namespace DevBenchBridge::ToolSpecs
{
	/** Devbench descriptor JSON for menu extension CommunityShaders. {brand} is expanded at registration. */
	inline constexpr const char* kMenuExt =
		R"({"description":")"
		R"(Open, close, or toggle the {brand} in-game settings menu headlessly, )"
		R"(the same window the ToggleKey (default End) shows. op: open|close|toggle (default toggle). page: OPTIONAL built-in page name (e.g. \"Performance\", )"
		R"(\"Home\") or a feature's shortName (see openshaders.feature list) to navigate to on the next frame, same as clicking it in the left pane. tab: OPTIONAL tab label, )"
		R"(at most 128 characters (visible text such as \"Neural Rendering\", or the id after ### with or without the ###; )"
		R"(case is ignored and so is any [Alpha]-style suffix on the visible text) to select in whichever tab bar the menu draws, same as clicking it; )"
		R"(give page too, and a tab inside another tab is found once that one is selected (call again with the inner tab). A request that finds no such tab within 600 menu frames is dropped with a warning in the log; )"
		R"(the call still reports queued. sidebarVisible: OPTIONAL boolean to show or hide the sidebar with its slide animation without saving settings; )"
		R"(false suppresses hover auto-hide expansion, true restores the configured auto-hide behavior. Use op=open when changing sidebar visibility. editorMode: OPTIONAL browser|menu to switch the OS Editor panel without saving settings; )"
		R"(use op=open and page=CSEditor to open the editor first. )"
		R"(Sidebar visibility only affects the standalone menu. Returns {op,page,tab,queued:true}; the change is applied on the render thread on the next frame (open is a no-op while first-time setup is pending).")"
		R"(,"inputSchema":{
	"type": "object",
	"properties": {
		"op": {
			"type": "string",
			"enum": ["open","close","toggle"]
		},
		"page": {"type":"string"},
		"tab": {"type":"string","maxLength":128},
		"sidebarVisible": {"type":"boolean"},
		"editorMode": {
			"type": "string",
			"enum": ["browser","menu"]
		}
	}
})"
		R"(})";
}
