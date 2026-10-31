#include "Utils/ImGuiLayout.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <memory>

TEST_CASE("Deactivated text inputs can apply their final resized buffer", "[imguilayout][inputtext]")
{
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(1280.0f, 720.0f);
	io.DeltaTime = 1.0f / 60.0f;
	io.Fonts->AddFontDefault();
	REQUIRE(io.Fonts->Build());
	ImGui::NewFrame();
	ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
	ImGui::Begin("DeferredInputTest", nullptr, ImGuiWindowFlags_NoSavedSettings);
	const auto id = ImGui::GetID("Deferred");
	context->InputTextState.ID = ImGui::GetID("Other");
	context->InputTextDeactivatedState.ID = id;
	const char finalText[] = "Final edit";
	context->InputTextDeactivatedState.TextA.resize(sizeof(finalText));
	std::memcpy(context->InputTextDeactivatedState.TextA.Data, finalText, sizeof(finalText));
	context->DeactivatedItemData.ID = id;
	context->DeactivatedItemData.ElapseFrame = context->FrameCount;
	context->DeactivatedItemData.HasBeenEditedBefore = true;
	char buffer[32] = "Previous";
	bool callbackCalled = false;
	const bool changed = ImGui::InputText("Deferred", buffer, sizeof(buffer), ImGuiInputTextFlags_CallbackResize, [](ImGuiInputTextCallbackData* data) {
			*static_cast<bool*>(data->UserData) = true;
			CHECK(data->EventFlag == ImGuiInputTextFlags_CallbackResize);
			CHECK_FALSE(data->EventActivated);
			return 0; }, &callbackCalled);
	ImGui::End();
	ImGui::EndFrame();
	CHECK(changed);
	CHECK(callbackCalled);
	CHECK(std::string(buffer) == finalText);
}

TEST_CASE("Legacy ImGui migration preserves non-layout settings", "[imguilayout]")
{
	const std::string legacy =
		"[Window][CommunityShaders]\r\nPos=10,20\r\nSize=1200,900\r\n\r\n"
		"[Table][0x1234,2]\r\nColumn 0 Width=90\r\n\r\n"
		"[Docking][Data]\r\nDockSpace ID=0x1234\r\n\r\n"
		"[Custom][Data]\r\nOption=retained\r\n\r\n"
		"[CommunityShaders][Data]\r\nDisplaySize=3840,2160\r\nCustom=retained\r\n";
	const auto migrated = Util::ImGuiLayout::Prepare(legacy);
	REQUIRE(migrated.changed);
	REQUIRE(migrated.version == Util::ImGuiLayout::kVersion);
	REQUIRE(migrated.contents ==
			"[Custom][Data]\r\nOption=retained\r\n\r\n"
			"[CommunityShaders][Data]\r\nLayoutVersion=2\r\nDisplaySize=3840,2160\r\nCustom=retained\r\n");
	REQUIRE_FALSE(Util::ImGuiLayout::Prepare(migrated.contents).changed);
}

TEST_CASE("Version one ImGui layouts discard stale menu table weights", "[imguilayout]")
{
	const auto prepared = Util::ImGuiLayout::Prepare(
		"[CommunityShaders][Data]\nLayoutVersion=1\nDisplaySize=3840,2160\n"
		"[Table][0x14483800,2]\nColumn 0 Weight=2\nColumn 1 Weight=1\n"
		"[Custom][Data]\nOption=retained\n");
	REQUIRE(prepared.changed);
	REQUIRE(prepared.version == 2);
	REQUIRE(prepared.contents ==
			"[CommunityShaders][Data]\nLayoutVersion=2\nDisplaySize=3840,2160\n"
			"[Custom][Data]\nOption=retained\n");
	REQUIRE_FALSE(Util::ImGuiLayout::Prepare(prepared.contents).changed);
}

TEST_CASE("Current and future ImGui layouts keep their geometry", "[imguilayout]")
{
	for (unsigned version : { Util::ImGuiLayout::kVersion, Util::ImGuiLayout::kVersion + 1 }) {
		const auto original =
			"[Window][Viewport]\nSize=900,700\n"
			"[Table][0x14483800,2]\nColumn 0 Weight=2\nColumn 1 Weight=8\n"
			"[Docking][Data]\nDockSpace ID=0x1234\n"
			"[CommunityShaders][Data]\nLayoutVersion=" +
			std::to_string(version) + "\n";
		const auto prepared = Util::ImGuiLayout::Prepare(original);
		REQUIRE_FALSE(prepared.changed);
		REQUIRE(prepared.version == version);
	}
}

TEST_CASE("Floating windows preserve user sizes across both DPI directions", "[imguilayout][dpi]")
{
	const ImRect workArea(ImVec2(100.0f, 50.0f), ImVec2(2020.0f, 1130.0f));
	const ImRect original(ImVec2(200.0f, 100.0f), ImVec2(600.0f, 400.0f));
	const auto reduced = Util::ImGuiLayout::ScaleWindowRect(original, 0.707f, workArea);
	CHECK(reduced.GetWidth() == Catch::Approx(original.GetWidth() * 0.707f));
	CHECK(reduced.GetHeight() == Catch::Approx(original.GetHeight() * 0.707f));
	CHECK(reduced.Min.x == original.Min.x);
	CHECK(reduced.Min.y == original.Min.y);
	const auto enlarged = Util::ImGuiLayout::ScaleWindowRect(reduced, 1.414f / 0.707f, workArea);
	CHECK(enlarged.GetWidth() == Catch::Approx(original.GetWidth() * 1.414f));
	CHECK(enlarged.GetHeight() == Catch::Approx(original.GetHeight() * 1.414f));
	const auto stable = Util::ImGuiLayout::ScaleWindowRect(enlarged, 1.0f, workArea);
	CHECK(stable.Min.x == enlarged.Min.x);
	CHECK(stable.Min.y == enlarged.Min.y);
	CHECK(stable.Max.x == enlarged.Max.x);
	CHECK(stable.Max.y == enlarged.Max.y);
}

TEST_CASE("Floating windows fit within a reduced viewport work area", "[imguilayout][dpi]")
{
	const ImRect workArea(ImVec2(100.0f, 50.0f), ImVec2(900.0f, 650.0f));
	const ImRect original(ImVec2(700.0f, 500.0f), ImVec2(1100.0f, 800.0f));
	const auto fitted = Util::ImGuiLayout::ScaleWindowRect(original, 1.0f, workArea);
	CHECK(fitted.GetWidth() == original.GetWidth());
	CHECK(fitted.GetHeight() == original.GetHeight());
	CHECK(fitted.Max.x == workArea.Max.x);
	CHECK(fitted.Max.y == workArea.Max.y);
	const auto oversized = Util::ImGuiLayout::ScaleWindowRect(original, 4.0f, workArea);
	CHECK(oversized.Min.x == workArea.Min.x);
	CHECK(oversized.Min.y == workArea.Min.y);
	CHECK(oversized.Max.x == workArea.Max.x);
	CHECK(oversized.Max.y == workArea.Max.y);
}

TEST_CASE("Auto-sized anchors release dragging after native geometry settles", "[imguilayout][dpi]")
{
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	auto* viewport = static_cast<ImGuiViewportP*>(ImGui::GetMainViewport());
	viewport->WorkPos = ImVec2(0.0f, 0.0f);
	viewport->WorkSize = ImVec2(1920.0f, 1080.0f);
	const ImRect workArea(viewport->WorkPos, viewport->WorkSize);
	ImGuiWindow window(context.get(), "AutoResizeAnchorTest");
	window.Flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar;
	window.Viewport = viewport;
	window.WindowPadding = ImGui::GetStyle().WindowPadding;
	window.DC.CursorStartPos = ImVec2(0.0f, 0.0f);
	window.DC.CursorMaxPos = ImVec2(300.0f, 100.0f);
	window.DC.IdealMaxPos = window.DC.CursorMaxPos;
	window.Size = ImGui::CalcWindowNextAutoFitSize(&window);
	Util::ImGuiLayout::AutoResizeAnchor anchor;
	REQUIRE(anchor.Update(1.0f, workArea, false, &window));
	REQUIRE(anchor.Update(1.0f, workArea, false, &window));
	CHECK_FALSE(anchor.Update(1.0f, workArea, false, &window));
	window.DC.CursorMaxPos = ImVec2(600.0f, 200.0f);
	window.DC.IdealMaxPos = window.DC.CursorMaxPos;
	REQUIRE(anchor.Update(2.0f, workArea, false, &window));
	REQUIRE(anchor.Update(2.0f, workArea, false, &window));
	CHECK(Util::ImGuiLayout::AutoResizeAnchor::IsSettling(&window));
	window.Size = ImGui::CalcWindowNextAutoFitSize(&window);
	REQUIRE(anchor.Update(2.0f, workArea, false, &window, true));
	CHECK(Util::ImGuiLayout::AutoResizeAnchor::IsSettling(&window));
	REQUIRE(anchor.Update(2.0f, workArea, false, &window));
	CHECK_FALSE(Util::ImGuiLayout::AutoResizeAnchor::IsSettling(&window));
	CHECK_FALSE(anchor.Update(2.0f, workArea, false, &window));
	CHECK(anchor.Update(2.0f, workArea, true, &window));
	window.DockIsActive = true;
	CHECK_FALSE(anchor.Update(1.0f, workArea, false, &window));
	CHECK_FALSE(Util::ImGuiLayout::AutoResizeAnchor::IsSettling(&window));
	window.DockIsActive = false;
	CHECK_FALSE(anchor.Update(1.0f, workArea, false, &window));
	ImGui::GcCompactTransientWindowBuffers(&window);
	REQUIRE(window.IDStack.empty());
	CHECK(anchor.Update(0.707f, workArea, false, &window));
	CHECK(Util::ImGuiLayout::AutoResizeAnchor::IsSettling(&window));
}

TEST_CASE("Record geometry keeps manual sizes and resets its DPI history", "[imguilayout][dpi]")
{
	std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	ImGuiWindow window(context.get(), "RecordGeometryTest");
	window.Pos = ImVec2(200.0f, 100.0f);
	window.SizeFull = ImVec2(400.0f, 300.0f);
	const ImRect workArea(ImVec2(0.0f, 0.0f), ImVec2(1920.0f, 1080.0f));
	Util::ImGuiLayout::PrepareScaledWindow(&window, 1.0f, workArea, false);
	CHECK_FALSE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	window.SizeFull = ImVec2(700.0f, 500.0f);
	Util::ImGuiLayout::PrepareScaledWindow(&window, 1.0f, workArea, false);
	CHECK_FALSE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	Util::ImGuiLayout::PrepareScaledWindow(&window, 0.707f, workArea, false);
	REQUIRE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	CHECK(context->NextWindowData.SizeVal.x == Catch::Approx(700.0f * 0.707f));
	CHECK(context->NextWindowData.SizeVal.y == Catch::Approx(500.0f * 0.707f));
	context->NextWindowData.ClearFlags();
	window.SizeFull = ImVec2(820.0f, 820.0f);
	Util::ImGuiLayout::PrepareScaledWindow(&window, 1.414f, workArea, true);
	CHECK_FALSE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	Util::ImGuiLayout::PrepareScaledWindow(&window, 1.414f, workArea, false);
	CHECK_FALSE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	const ImRect reducedWorkArea(ImVec2(100.0f, 50.0f), ImVec2(900.0f, 650.0f));
	Util::ImGuiLayout::PrepareScaledWindow(&window, 1.414f, reducedWorkArea, false);
	REQUIRE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	CHECK(context->NextWindowData.SizeVal.x == reducedWorkArea.GetWidth());
	CHECK(context->NextWindowData.SizeVal.y == reducedWorkArea.GetHeight());
	REQUIRE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasPos);
	CHECK(context->NextWindowData.PosVal.x == reducedWorkArea.Min.x);
	CHECK(context->NextWindowData.PosVal.y == reducedWorkArea.Min.y);
	context->NextWindowData.ClearFlags();
	ImGui::GcCompactTransientWindowBuffers(&window);
	REQUIRE(window.IDStack.empty());
	Util::ImGuiLayout::PrepareScaledWindow(&window, 0.707f, workArea, false);
	REQUIRE(context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSize);
	CHECK(context->NextWindowData.SizeVal.x == Catch::Approx(410.0f));
	CHECK(context->NextWindowData.SizeVal.y == Catch::Approx(410.0f));
}

TEST_CASE("Only valid owned ImGui layout markers prevent migration", "[imguilayout]")
{
	const auto prepared = Util::ImGuiLayout::Prepare(
		"[Other][Data]\nLayoutVersion=99\n"
		"[CommunityShaders][Other]\nLayoutVersion=99\n"
		"[CommunityShaders][Data]\nLayoutVersion=2garbage\nDisplaySize=1920,1080\n"
		"[Window][Palette]\nSize=1200,800\n");
	REQUIRE(prepared.changed);
	REQUIRE(prepared.contents.find("[Window]") == std::string::npos);
	REQUIRE(prepared.contents.find("LayoutVersion=2garbage") == std::string::npos);
	REQUIRE(prepared.contents.starts_with("[Other][Data]\nLayoutVersion=99\n[CommunityShaders][Other]\nLayoutVersion=99\n"));
	REQUIRE(prepared.contents.find("DisplaySize=1920,1080") != std::string::npos);
}

TEST_CASE("An ImGui layout without custom settings gains one version marker", "[imguilayout]")
{
	const auto prepared = Util::ImGuiLayout::Prepare("[Window][Viewport]\nSize=900,700");
	REQUIRE(prepared.changed);
	REQUIRE(prepared.contents == "[CommunityShaders][Data]\nLayoutVersion=2\n");
}

TEST_CASE("The highest owned ImGui layout marker survives duplicate sections", "[imguilayout]")
{
	const auto prepared = Util::ImGuiLayout::Prepare(
		"[CommunityShaders][Data]\nLayoutVersion=3\n"
		"[CommunityShaders][Data]\nLayoutVersion=0\n"
		"[Window][Viewport]\nSize=900,700\n");
	REQUIRE_FALSE(prepared.changed);
	REQUIRE(prepared.version == 3);
}

TEST_CASE("Rounded popup scrollbars keep their width and reserve corner space", "[imguilayout][imgui-style]")
{
	const std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	ImGuiWindow window(context.get(), "PopupScrollbar");
	window.Flags = ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoTitleBar;
	for (const float scale : { 0.707f, 1.0f, 1.414f }) {
		ImGui::GetStyle().GrabMinSize = 8.0f * scale;
		window.Pos = ImVec2(10.0f, 20.0f);
		window.Size = ImVec2(100.0f * scale, 200.0f * scale);
		window.InnerRect = ImRect(window.Pos, ImVec2(window.Pos.x + window.Size.x, window.Pos.y + window.Size.y));
		window.ScrollbarSizes = ImVec2(10.0f * scale, 0.0f);
		window.WindowBorderSize = 2.0f * scale;
		window.WindowRounding = 0.0f;
		const auto square = ImGui::GetWindowScrollbarRect(&window, ImGuiAxis_Y);
		window.WindowRounding = 8.0f * scale;
		const auto rounded = ImGui::GetWindowScrollbarRect(&window, ImGuiAxis_Y);
		CHECK(rounded.Min.x == square.Min.x);
		CHECK(rounded.Max.x == square.Max.x);
		CHECK(rounded.Min.y > square.Min.y);
		CHECK(rounded.Max.y < square.Max.y);
		CHECK(rounded.GetHeight() >= ImGui::GetStyle().GrabMinSize);
		window.Flags = ImGuiWindowFlags_NoTitleBar;
		const auto ordinary = ImGui::GetWindowScrollbarRect(&window, ImGuiAxis_Y);
		CHECK(ordinary.Min.y == square.Min.y);
		CHECK(ordinary.Max.y == square.Max.y);
		window.Flags = ImGuiWindowFlags_Popup | ImGuiWindowFlags_NoTitleBar;
		window.Size.y = ImGui::GetStyle().GrabMinSize;
		window.InnerRect.Max.y = window.Pos.y + window.Size.y;
		const auto shortTrack = ImGui::GetWindowScrollbarRect(&window, ImGuiAxis_Y);
		CHECK(shortTrack.GetHeight() > 0.0f);
	}
}

TEST_CASE("Every floating window corner uses native resize constraints", "[imguilayout][resize]")
{
	for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex) {
		INFO("Corner " << cornerIndex);
		const std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1280.0f, 720.0f);
		io.DeltaTime = 1.0f / 60.0f;
		io.ConfigWindowsResizeFromEdges = true;
		io.Fonts->AddFontDefault();
		REQUIRE(io.Fonts->Build());
		bool open = true;
		const auto frame = [&open]() {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(300.0f, 200.0f), ImGuiCond_Once);
			ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Once);
			ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 150.0f), ImVec2(600.0f, 450.0f));
			ImGui::Begin("CornerResizeTest", &open, ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("Native resize");
			ImGui::End();
			ImGui::EndFrame();
		};
		frame();
		frame();
		auto* window = ImGui::FindWindowByName("CornerResizeTest");
		REQUIRE(window);
		const ImRect original = window->Rect();
		const bool right = cornerIndex == 0 || cornerIndex == 3;
		const bool bottom = cornerIndex < 2;
		const float innerReach = window->FontRefSize * 0.5f;
		const ImVec2 corner(right ? original.Max.x - innerReach : original.Min.x + innerReach,
			bottom ? original.Max.y - 1.0f : original.Min.y + 1.0f);
		io.AddMousePosEvent(corner.x, corner.y);
		frame();
		const ImGuiID cornerID = ImGui::GetWindowResizeCornerID(window, cornerIndex);
		REQUIRE(ImGui::GetHoveredID() == cornerID);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		frame();
		REQUIRE(ImGui::GetActiveID() == cornerID);
		io.AddMousePosEvent(corner.x + (right ? 300.0f : -300.0f), corner.y + (bottom ? 250.0f : -250.0f));
		frame();
		CHECK(window->Size.x == 600.0f);
		CHECK(window->Size.y == 450.0f);
		CHECK((right ? window->Pos.x : window->Rect().Max.x) == (right ? original.Min.x : original.Max.x));
		CHECK((bottom ? window->Pos.y : window->Rect().Max.y) == (bottom ? original.Min.y : original.Max.y));
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame();
		CHECK(ImGui::GetActiveID() != cornerID);
		const auto& style = ImGui::GetStyle();
		const float fontSize = window->FontRefSize;
		const float buttonY = window->Pos.y + style.FramePadding.y + fontSize * 0.5f;
		io.AddMousePosEvent(window->Pos.x + style.FramePadding.x + fontSize * 0.5f, buttonY);
		frame();
		CHECK(ImGui::GetHoveredID() == window->GetID("#COLLAPSE"));
		io.AddMousePosEvent(window->Rect().Max.x - style.FramePadding.x - fontSize * 0.5f, buttonY);
		frame();
		REQUIRE(ImGui::GetHoveredID() == window->GetID("#CLOSE"));
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		frame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame();
		CHECK_FALSE(open);
	}
}

TEST_CASE("Header buttons take corner clicks while active resizing retains ownership", "[imguilayout][resize]")
{
	const std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(1280.0f, 720.0f);
	io.DeltaTime = 1.0f / 60.0f;
	io.ConfigWindowsResizeFromEdges = true;
	io.ConfigInputTrickleEventQueue = false;
	io.Fonts->AddFontDefault();
	REQUIRE(io.Fonts->Build());
	bool disabled = true;
	bool clicked = false;
	const auto frame = [&]() {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(300.0f, 200.0f), ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Once);
		ImGui::Begin("HeaderCornerTest", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
		const auto& style = ImGui::GetStyle();
		const float fontSize = ImGui::GetFontSize();
		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - fontSize * 2.0f, 0.0f));
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##HeaderBackground", ImVec2(fontSize * 2.0f, fontSize * 2.0f));
		ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - style.FramePadding.x - fontSize, style.FramePadding.y));
		ImGui::BeginDisabled(disabled);
		clicked |= ImGui::Button("##HeaderClose", ImVec2(fontSize, fontSize));
		ImGui::EndDisabled();
		ImGui::End();
		ImGui::EndFrame();
	};
	frame();
	frame();
	auto* window = ImGui::FindWindowByName("HeaderCornerTest");
	REQUIRE(window);
	const auto& style = ImGui::GetStyle();
	const float fontSize = window->FontRefSize;
	const ImVec2 buttonCenter(window->Rect().Max.x - style.FramePadding.x - fontSize * 0.5f,
		window->Pos.y + style.FramePadding.y + fontSize * 0.5f);
	const ImGuiID cornerID = ImGui::GetWindowResizeCornerID(window, 3);
	io.AddMousePosEvent(buttonCenter.x, buttonCenter.y);
	frame();
	REQUIRE(ImGui::GetHoveredID() == cornerID);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	REQUIRE(ImGui::GetActiveID() == cornerID);
	disabled = false;
	frame();
	CHECK(ImGui::GetActiveID() == cornerID);
	CHECK_FALSE(clicked);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
	io.AddMousePosEvent(window->Rect().Max.x - 1.0f, window->Pos.y + 1.0f);
	frame();
	REQUIRE(ImGui::GetHoveredID() == cornerID);
	io.AddMousePosEvent(buttonCenter.x, buttonCenter.y);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	REQUIRE(io.MouseDoubleClicked[ImGuiMouseButton_Left]);
	REQUIRE(ImGui::GetActiveID() == window->GetID("##HeaderClose"));
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
	CHECK(clicked);
}

TEST_CASE("Unobstructed corner double-clicks still auto-fit after control arbitration", "[imguilayout][resize]")
{
	const std::unique_ptr<ImGuiContext, void (*)(ImGuiContext*)> context(ImGui::CreateContext(), &ImGui::DestroyContext);
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(1280.0f, 720.0f);
	io.DeltaTime = 1.0f / 60.0f;
	io.ConfigWindowsResizeFromEdges = true;
	io.ConfigInputTrickleEventQueue = false;
	io.Fonts->AddFontDefault();
	REQUIRE(io.Fonts->Build());
	const auto frame = []() {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(300.0f, 200.0f), ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Once);
		ImGui::Begin("CornerAutoFitTest", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
		ImGui::TextUnformatted("Auto-fit content");
		ImGui::End();
		ImGui::EndFrame();
	};
	frame();
	frame();
	auto* window = ImGui::FindWindowByName("CornerAutoFitTest");
	REQUIRE(window);
	const auto originalSize = window->Size;
	const auto cornerID = ImGui::GetWindowResizeCornerID(window, 0);
	io.AddMousePosEvent(window->Rect().Max.x - 1.0f, window->Rect().Max.y - 1.0f);
	frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	frame();
	REQUIRE(io.MouseDoubleClicked[ImGuiMouseButton_Left]);
	CHECK(ImGui::GetActiveID() == cornerID);
	CHECK(window->Size.x == originalSize.x);
	CHECK(window->Size.y == originalSize.y);
	frame();
	CHECK(ImGui::GetActiveID() == 0);
	CHECK(window->Size.x < originalSize.x);
	CHECK(window->Size.y < originalSize.y);
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	frame();
}
