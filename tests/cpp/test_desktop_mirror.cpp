// Unit tests for the VR desktop-mirror draw-data fit: affine mapping, clipping, source immutability,
// input validation and callback discovery. They exercise the production DesktopMirrorDrawData only.

#include "Menu/DesktopMirrorDrawData.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
	void check(bool condition, const char* message)
	{
		INFO(message);
		CHECK(condition);
	}
	bool close(float a, float b, float epsilon = 0.01f)
	{
		return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= epsilon;
	}
	void checkVec(ImVec2 actual, ImVec2 expected, const char* message)
	{
		check(close(actual.x, expected.x) && close(actual.y, expected.y), message);
	}
	// Distinct bodies keep the optimizer from folding the callbacks into one address.
	volatile int callbackSink = 0;
	void callback(const ImDrawList*, const ImDrawCmd*) { callbackSink = 1; }
	void otherCallback(const ImDrawList*, const ImDrawCmd*) { callbackSink = 2; }
	void markedCallback(const ImDrawList*, const ImDrawCmd*) { callbackSink = 3; }

	struct Source
	{
		ImDrawList list{ ImGui::GetDrawListSharedData() };
		ImDrawData data;

		Source(ImVec2 displayPos, ImVec2 displaySize, ImVec2 framebufferScale,
			bool addCallback = true, ImDrawCallback drawCallback = callback)
		{
			list._ResetForNewFrame();
			list.AddRectFilled(ImVec2(displayPos.x - 20.0f, displayPos.y - 10.0f),
				ImVec2(displayPos.x + 60.0f, displayPos.y + 30.0f), 0xFFFFFFFF);
			list.AddRectFilled(ImVec2(displayPos.x + 10.0f, displayPos.y + 10.0f),
				ImVec2(displayPos.x + 80.0f, displayPos.y + 50.0f), 0xFF00FFFF);
			if (addCallback)
				list.AddCallback(drawCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678)));
			list.PushClipRect(ImVec2(displayPos.x + 380.0f, displayPos.y + 180.0f),
				ImVec2(displayPos.x + 420.0f, displayPos.y + 240.0f), false);
			list.AddRectFilled(ImVec2(displayPos.x + 380.0f, displayPos.y + 180.0f),
				ImVec2(displayPos.x + 420.0f, displayPos.y + 240.0f), 0xFF00FFFF);
			list.PopClipRect();
			list.PushClipRect(ImVec2(displayPos.x + 600.0f, displayPos.y + 20.0f),
				ImVec2(displayPos.x + 700.0f, displayPos.y + 80.0f), false);
			list.AddRectFilled(ImVec2(displayPos.x + 600.0f, displayPos.y + 20.0f),
				ImVec2(displayPos.x + 700.0f, displayPos.y + 80.0f), 0xFF00FFFF);
			list.PopClipRect();
			data.Valid = true;
			data.DisplayPos = displayPos;
			data.DisplaySize = displaySize;
			data.FramebufferScale = framebufferScale;
			data.AddDrawList(&list);
		}
	};

	struct Snapshot
	{
		std::vector<ImDrawVert> vertices;
		std::vector<ImDrawIdx> indices;
		std::vector<ImDrawCmd> commands;
		ImVec2 displayPos;
		ImVec2 displaySize;
		ImVec2 framebufferScale;

		explicit Snapshot(const Source& source) : vertices(source.list.VtxBuffer.begin(), source.list.VtxBuffer.end()),
												  indices(source.list.IdxBuffer.begin(), source.list.IdxBuffer.end()),
												  commands(source.list.CmdBuffer.begin(), source.list.CmdBuffer.end()),
												  displayPos(source.data.DisplayPos), displaySize(source.data.DisplaySize),
												  framebufferScale(source.data.FramebufferScale) {}

		void Verify(const Source& source) const
		{
			INFO("source vertex count is unchanged");
			REQUIRE(source.list.VtxBuffer.Size == static_cast<int>(vertices.size()));
			INFO("source index count is unchanged");
			REQUIRE(source.list.IdxBuffer.Size == static_cast<int>(indices.size()));
			INFO("source command count is unchanged");
			REQUIRE(source.list.CmdBuffer.Size == static_cast<int>(commands.size()));
			checkVec(source.data.DisplayPos, displayPos, "source DisplayPos is unchanged");
			checkVec(source.data.DisplaySize, displaySize, "source DisplaySize is unchanged");
			checkVec(source.data.FramebufferScale, framebufferScale, "source FramebufferScale is unchanged");
			for (int i = 0; i < source.list.VtxBuffer.Size; ++i) {
				const auto& actual = source.list.VtxBuffer[i];
				const auto& expected = vertices[static_cast<size_t>(i)];
				checkVec(actual.pos, expected.pos, "source vertex position is unchanged");
				checkVec(actual.uv, expected.uv, "source vertex UV is unchanged");
				check(actual.col == expected.col, "source vertex color is unchanged");
			}
			for (int i = 0; i < source.list.IdxBuffer.Size; ++i)
				check(source.list.IdxBuffer[i] == indices[static_cast<size_t>(i)], "source index data is unchanged");
			for (int i = 0; i < source.list.CmdBuffer.Size; ++i) {
				const auto& actual = source.list.CmdBuffer[i];
				const auto& expected = commands[static_cast<size_t>(i)];
				check(close(actual.ClipRect.x, expected.ClipRect.x) && close(actual.ClipRect.y, expected.ClipRect.y),
					"source clip rect minimum is unchanged");
				check(close(actual.ClipRect.z, expected.ClipRect.z) && close(actual.ClipRect.w, expected.ClipRect.w),
					"source clip rect maximum is unchanged");
				check(actual.ElemCount == expected.ElemCount && actual.IdxOffset == expected.IdxOffset &&
						  actual.VtxOffset == expected.VtxOffset,
					"source command offsets/count are unchanged");
				check(actual.GetTexID() == expected.GetTexID(), "source texture reference is unchanged");
				check(actual.UserCallback == expected.UserCallback &&
						  actual.UserCallbackData == expected.UserCallbackData,
					"source callback metadata is unchanged");
			}
		}
	};

	void testFitAndPreservation()
	{
		const ImVec2 displayPos(100.0f, 200.0f);
		Source source(displayPos, ImVec2(400.0f, 200.0f), ImVec2(1.5f, 2.0f));
		source.list.CmdBuffer[0].ClipRect = ImVec4(80.0f, 190.0f, 110.0f, 220.0f);
		source.list.CmdBuffer[0].TexRef._TexData = nullptr;
		source.list.CmdBuffer[0].TexRef._TexID = static_cast<ImTextureID>(0x1234);
		source.data.CmdListsCount = source.data.CmdLists.Size;
		source.data.TotalVtxCount = source.list.VtxBuffer.Size;
		source.data.TotalIdxCount = source.list.IdxBuffer.Size;
		const Snapshot original(source);

		DesktopMirrorDrawData mirror;
		check(mirror.Build(source.data, ImVec2(300.0f, 300.0f)), "valid source builds a mirror");
		DesktopMirrorDrawData::FitTransform fit{};
		check(DesktopMirrorDrawData::GetFitTransform(source.data, ImVec2(300.0f, 300.0f), fit),
			"fit parameters are available for window geometry");
		ImVec2 blurPoint{};
		check(fit.MapPosition(ImVec2(displayPos.x, displayPos.y), source.data.DisplayPos,
				  source.data.FramebufferScale, blurPoint),
			"window geometry maps to target pixels");
		checkVec(blurPoint, ImVec2(0.0f, 50.0f), "window geometry shares the draw-data letterbox transform");
		const ImDrawData* output = mirror.Get();
		check(output != nullptr, "successful build exposes draw data");
		if (!output)
			return;
		checkVec(output->DisplayPos, ImVec2(0.0f, 0.0f), "mirror starts at target origin");
		checkVec(output->DisplaySize, ImVec2(300.0f, 300.0f), "mirror reports target pixel size");
		checkVec(output->FramebufferScale, ImVec2(1.0f, 1.0f), "mirror output is in target pixels");
		check(output->CmdListsCount == 1 && output->CmdLists.Size == 1, "draw list count is preserved");
		check(output->TotalVtxCount == source.data.TotalVtxCount && output->TotalIdxCount == source.data.TotalIdxCount,
			"aggregate vertex and index counts are preserved");

		const ImDrawList* mirrored = output->CmdLists[0];
		check(mirrored->VtxBuffer.Size == source.list.VtxBuffer.Size, "vertex count is preserved");
		check(mirrored->IdxBuffer.Size == source.list.IdxBuffer.Size, "index count is preserved");
		check(mirrored->CmdBuffer.Size == source.list.CmdBuffer.Size, "command count is preserved");
		check(mirrored->VtxBuffer.Size > 0 && mirrored->CmdBuffer.Size > 3, "fixture exercises geometry, clipping, and callbacks");
		for (int i = 0; i < mirrored->IdxBuffer.Size; ++i)
			check(mirrored->IdxBuffer[i] == source.list.IdxBuffer[i], "index data survives list cloning");
		for (int i = 0; i < mirrored->CmdBuffer.Size; ++i) {
			const auto& actual = mirrored->CmdBuffer[i];
			const auto& expected = source.list.CmdBuffer[i];
			check(actual.ElemCount == expected.ElemCount && actual.IdxOffset == expected.IdxOffset &&
					  actual.VtxOffset == expected.VtxOffset,
				"every command preserves element count and offsets");
			check(actual.GetTexID() == expected.GetTexID(), "every command preserves its texture reference");
			check(actual.UserCallback == expected.UserCallback &&
					  actual.UserCallbackData == expected.UserCallbackData,
				"every command preserves callback metadata");
		}
		if (mirrored->VtxBuffer.Size > 0) {
			checkVec(mirrored->VtxBuffer[0].pos, ImVec2(-15.0f, 40.0f), "first vertex applies DisplayPos, framebuffer scale, and centered fit");
			checkVec(mirrored->VtxBuffer[1].pos, ImVec2(45.0f, 40.0f), "horizontal physical scale is applied");
			checkVec(mirrored->VtxBuffer[2].pos, ImVec2(45.0f, 80.0f), "vertical physical scale is applied");
		}
		if (mirrored->CmdBuffer.Size > 0) {
			const auto& command = mirrored->CmdBuffer[0];
			check(close(command.ClipRect.x, 0.0f) && close(command.ClipRect.y, 40.0f),
				"clip rect is transformed and cropped at the left/top boundary");
			check(close(command.ClipRect.z, 7.5f) && close(command.ClipRect.w, 70.0f),
				"clip rect preserves its in-target portion");
			check(command.ElemCount == source.list.CmdBuffer[0].ElemCount &&
					  command.IdxOffset == source.list.CmdBuffer[0].IdxOffset &&
					  command.VtxOffset == source.list.CmdBuffer[0].VtxOffset,
				"draw offsets and element count are preserved");
			check(command.GetTexID() == static_cast<ImTextureID>(0x1234), "texture reference survives cloning");
		}
		if (mirrored->CmdBuffer.Size > 2) {
			const auto& cropped = mirrored->CmdBuffer[2];
			check(close(cropped.ClipRect.x, 285.0f) && close(cropped.ClipRect.y, 230.0f) &&
					  close(cropped.ClipRect.z, 300.0f) && close(cropped.ClipRect.w, 290.0f),
				"clip rect is cropped at the right and bottom target boundaries");
		}
		if (mirrored->CmdBuffer.Size > 3) {
			const auto& outside = mirrored->CmdBuffer[3];
			check(close(outside.ClipRect.x, 300.0f) && close(outside.ClipRect.z, 300.0f),
				"fully offscreen clip rect collapses to a zero-width edge rectangle");
		}
		bool callbackCopied = false;
		for (int i = 0; i < mirrored->CmdBuffer.Size; ++i) {
			const auto& command = mirrored->CmdBuffer[i];
			if (command.UserCallback == callback) {
				callbackCopied = true;
				check(command.UserCallbackData == reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678)),
					"callback user data pointer is preserved");
			}
			check(std::isfinite(command.ClipRect.x) && std::isfinite(command.ClipRect.y) &&
					  std::isfinite(command.ClipRect.z) && std::isfinite(command.ClipRect.w),
				"all output clip rects are finite");
			check(command.ClipRect.x >= 0.0f && command.ClipRect.y >= 0.0f &&
					  command.ClipRect.z <= 300.0f && command.ClipRect.w <= 300.0f &&
					  command.ClipRect.x <= command.ClipRect.z && command.ClipRect.y <= command.ClipRect.w,
				"all output clip rects remain ordered and inside the target");
		}
		check(callbackCopied, "custom draw callback is preserved");
		for (int i = 0; i < mirrored->VtxBuffer.Size; ++i)
			check(std::isfinite(mirrored->VtxBuffer[i].pos.x) && std::isfinite(mirrored->VtxBuffer[i].pos.y),
				"all output vertex positions are finite");
		original.Verify(source);
	}

	void testIdentityAndScaleCases()
	{
		DesktopMirrorDrawData mirror;
		{
			Source source(ImVec2(25.0f, 40.0f), ImVec2(100.0f, 50.0f), ImVec2(2.0f, 1.0f));
			check(mirror.Build(source.data, ImVec2(200.0f, 50.0f)), "physical-size identity case builds");
			const auto* output = mirror.Get();
			check(output && output->CmdListsCount == 1, "identity case returns one draw list");
			if (output && output->CmdListsCount && output->CmdLists[0]->VtxBuffer.Size > 0)
				checkVec(output->CmdLists[0]->VtxBuffer[0].pos, ImVec2(-40.0f, -10.0f),
					"identity physical canvas subtracts nonzero DisplayPos and scales framebuffer coordinates");
		}
		{
			Source source(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 50.0f), ImVec2(1.0f, 1.0f));
			check(mirror.Build(source.data, ImVec2(400.0f, 200.0f)), "larger target builds");
			const auto* output = mirror.Get();
			if (output && output->CmdListsCount && output->CmdLists[0]->VtxBuffer.Size > 0)
				checkVec(output->CmdLists[0]->VtxBuffer[0].pos, ImVec2(-80.0f, -40.0f), "larger target uses uniform scale");
			check(mirror.Build(source.data, ImVec2(50.0f, 25.0f)), "smaller target builds");
			output = mirror.Get();
			if (output && output->CmdListsCount && output->CmdLists[0]->VtxBuffer.Size > 0)
				checkVec(output->CmdLists[0]->VtxBuffer[0].pos, ImVec2(-10.0f, -5.0f), "smaller target uses uniform scale");
		}
	}

	void testInvalidInputsAndReset()
	{
		DesktopMirrorDrawData mirror;
		Source valid(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f));
		check(mirror.Build(valid.data, ImVec2(100.0f, 100.0f)), "valid build before invalid-input checks");
		for (ImVec2 target : { ImVec2(0.0f, 100.0f), ImVec2(-1.0f, 100.0f),
				 ImVec2(INFINITY, 100.0f), ImVec2(NAN, 100.0f) }) {
			check(!mirror.Build(valid.data, target), "zero, negative, infinite, and NaN target dimensions are rejected");
			check(mirror.Get() == nullptr, "invalid build clears the previous output");
		}
		ImDrawData empty;
		empty.Valid = true;
		empty.DisplaySize = ImVec2(100.0f, 100.0f);
		empty.FramebufferScale = ImVec2(1.0f, 1.0f);
		check(!mirror.Build(empty, ImVec2(100.0f, 100.0f)), "empty draw data is rejected as a no-op");
		check(mirror.Get() == nullptr, "empty input leaves no stale output");
		Source badScale(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(NAN, 1.0f));
		check(!mirror.Build(badScale.data, ImVec2(100.0f, 100.0f)), "non-finite source framebuffer scale is rejected");
		check(mirror.Get() == nullptr, "bad source scale leaves no output");
		Source badDisplay(ImVec2(0.0f, 0.0f), ImVec2(INFINITY, 100.0f), ImVec2(1.0f, 1.0f));
		check(!mirror.Build(badDisplay.data, ImVec2(100.0f, 100.0f)), "infinite source display dimensions are rejected");
		Source badVertex(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f));
		badVertex.list.VtxBuffer[0].pos.x = NAN;
		check(!mirror.Build(badVertex.data, ImVec2(100.0f, 100.0f)), "non-finite source vertices are rejected");
		Source badClip(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f));
		badClip.list.CmdBuffer[0].ClipRect.x = NAN;
		check(!mirror.Build(badClip.data, ImVec2(100.0f, 100.0f)), "non-finite source clip rects are rejected");
		check(mirror.Get() == nullptr, "malformed source data leaves no output");
		check(mirror.Build(valid.data, ImVec2(100.0f, 100.0f)), "valid build recovers after invalid inputs");
		mirror.Reset();
		check(mirror.Get() == nullptr, "Reset releases mirrored draw data");
	}

	void testCallbackDiscoveryAcrossLists()
	{
		Source first(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f), false);
		Source second(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f), true, markedCallback);
		ImDrawData combined;
		combined.Valid = true;
		combined.DisplaySize = ImVec2(100.0f, 100.0f);
		combined.FramebufferScale = ImVec2(1.0f, 1.0f);
		combined.AddDrawList(&first.list);
		combined.AddDrawList(&second.list);
		check(DesktopMirrorDrawData::ContainsCallback(combined, markedCallback),
			"callback discovery searches every draw list");
		check(!DesktopMirrorDrawData::ContainsCallback(combined, otherCallback),
			"callback discovery requires an exact callback match");
		check(!DesktopMirrorDrawData::ContainsCallback(first.data, callback),
			"callback discovery reports absent callbacks");
	}

	void testGenericCallbackSurvivesFit()
	{
		Source generic(ImVec2(10.0f, 20.0f), ImVec2(100.0f, 100.0f), ImVec2(1.0f, 1.0f), true, callback);
		DesktopMirrorDrawData mirror;
		check(mirror.Build(generic.data, ImVec2(200.0f, 100.0f)), "callback frame builds the fitted mirror");
		const auto* fitted = mirror.Get();
		bool callbackPreserved = false;
		if (fitted && fitted->CmdListsCount > 0) {
			for (int i = 0; i < fitted->CmdLists[0]->CmdBuffer.Size; ++i) {
				const auto& command = fitted->CmdLists[0]->CmdBuffer[i];
				callbackPreserved |= command.UserCallback == callback &&
				                     command.UserCallbackData == generic.list.CmdBuffer[i].UserCallbackData;
			}
		}
		check(callbackPreserved, "callback and its user data are retained in the fitted copy");
	}

	struct ImGuiContextScope
	{
		ImGuiContextScope()
		{
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			ImGui::NewFrame();
		}
		~ImGuiContextScope()
		{
			ImGui::EndFrame();
			ImGui::DestroyContext();
		}
	};
}

TEST_CASE("Desktop mirror fits and preserves draw data", "[desktopmirror]")
{
	ImGuiContextScope context;
	testFitAndPreservation();
}

TEST_CASE("Desktop mirror identity and scale cases", "[desktopmirror]")
{
	ImGuiContextScope context;
	testIdentityAndScaleCases();
}

TEST_CASE("Desktop mirror rejects invalid input and resets", "[desktopmirror]")
{
	ImGuiContextScope context;
	testInvalidInputsAndReset();
}

TEST_CASE("Desktop mirror finds callbacks across draw lists", "[desktopmirror]")
{
	ImGuiContextScope context;
	testCallbackDiscoveryAcrossLists();
}

TEST_CASE("Desktop mirror keeps generic callbacks", "[desktopmirror]")
{
	ImGuiContextScope context;
	testGenericCallbackSurvivesFit();
}
