#include "Menu/ImGuiSurfaceSync.h"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstring>
#include <string>

namespace
{
	struct HeadlessSurface
	{
		ImGuiContext* context = nullptr;

		explicit HeadlessSurface(ImVec2 displaySize, float fontPixels)
		{
			ImGuiContext* prev = ImGui::GetCurrentContext();
			context = ImGui::CreateContext();
			ImGui::SetCurrentContext(context);

			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.DisplaySize = displaySize;
			io.DeltaTime = 1.0f / 60.0f;

			ImFontConfig cfg{};
			cfg.SizePixels = fontPixels;
			io.Fonts->AddFontDefault(&cfg);
			unsigned char* pixels = nullptr;
			int width = 0;
			int height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			io.Fonts->SetTexID(static_cast<ImTextureID>(1));

			ImGui::SetCurrentContext(prev);
		}

		~HeadlessSurface()
		{
			if (context) {
				ImGuiContext* prev = ImGui::GetCurrentContext();
				if (prev == context)
					prev = nullptr;
				ImGui::DestroyContext(context);
				ImGui::SetCurrentContext(prev);
			}
		}

		HeadlessSurface(const HeadlessSurface&) = delete;
		HeadlessSurface& operator=(const HeadlessSurface&) = delete;
	};

	struct SharedMenuModel
	{
		bool featureEnabled = false;
		std::array<char, 64> searchBuffer{ "initial" };
		bool showWarningModal = false;
	};

	struct SurfaceObservation
	{
		ImVec2 windowPos{};
		ImVec2 windowSize{};
		float fontSize = 0.0f;
		int activeTab = -1;
		bool detailsOpen = false;
		bool modalOpen = false;
		ImVec2 tab1Center{};
		ImVec2 headerCenter{};
		ImVec2 inputCenter{};
		ImVec2 modalOkCenter{};
	};

	ImVec2 GetLastItemCenter()
	{
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
	}

	SurfaceObservation RunSurfaceFrame(
		HeadlessSurface& surface,
		ImGuiSurfaceSync& sync,
		HeadlessSurface& peer,
		SharedMenuModel& model,
		ImVec2 initialWindowPos,
		ImVec2 initialWindowSize)
	{
		SurfaceObservation obs{};
		sync.ApplyBeforeNewFrame(surface.context);
		{
			ScopedImGuiContext guard(surface.context);
			ImGui::NewFrame();

			ImGui::SetNextWindowPos(initialWindowPos, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize(initialWindowSize, ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Open Shaders###CommunityShaders")) {
				obs.windowPos = ImGui::GetWindowPos();
				obs.windowSize = ImGui::GetWindowSize();
				obs.fontSize = ImGui::GetFontSize();

				ImGui::Checkbox("Enable Feature", &model.featureEnabled);

				if (ImGui::BeginTabBar("##SettingsTabs")) {
					if (ImGui::BeginTabItem("General")) {
						obs.activeTab = 0;
						ImGui::EndTabItem();
					}
					const bool tab1Selected = ImGui::BeginTabItem("Advanced");
					obs.tab1Center = GetLastItemCenter();
					if (tab1Selected) {
						obs.activeTab = 1;
						ImGui::EndTabItem();
					}
					ImGui::EndTabBar();
				}

				obs.detailsOpen = ImGui::CollapsingHeader("Details");
				obs.headerCenter = GetLastItemCenter();

				ImGui::InputText("Search", model.searchBuffer.data(), model.searchBuffer.size());
				obs.inputCenter = GetLastItemCenter();

				if (model.showWarningModal)
					ImGui::OpenPopup("###SharedWarning");

				if (ImGui::BeginPopupModal("Warning###SharedWarning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
					obs.modalOpen = true;
					const bool okPressed = ImGui::Button("OK");
					obs.modalOkCenter = GetLastItemCenter();
					if (okPressed) {
						model.showWarningModal = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}
			ImGui::End();
			ImGui::Render();
		}
		sync.OnFrameCompleted(surface.context, peer.context);
		return obs;
	}

	void ClickOnSurface(
		HeadlessSurface& active,
		HeadlessSurface& peer,
		ImGuiSurfaceSync& sync,
		SharedMenuModel& model,
		ImVec2 clickPoint,
		ImVec2 activePos,
		ImVec2 activeSize,
		ImVec2 peerPos,
		ImVec2 peerSize)
	{
		{
			ScopedImGuiContext guard(active.context);
			ImGui::GetIO().AddMousePosEvent(clickPoint.x, clickPoint.y);
		}
		RunSurfaceFrame(active, sync, peer, model, activePos, activeSize);

		{
			ScopedImGuiContext guard(active.context);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
		}
		RunSurfaceFrame(active, sync, peer, model, activePos, activeSize);
		RunSurfaceFrame(peer, sync, active, model, peerPos, peerSize);

		{
			ScopedImGuiContext guard(active.context);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
		}
		RunSurfaceFrame(active, sync, peer, model, activePos, activeSize);
		RunSurfaceFrame(peer, sync, active, model, peerPos, peerSize);
	}
}

TEST_CASE("ImGuiSurfaceSync preserves independent layout and font scale across VR and desktop surfaces", "[menu][vr][imgui]")
{
	HeadlessSurface vrSurface(ImVec2(2560.0f, 1440.0f), 32.0f);
	HeadlessSurface desktopSurface(ImVec2(1024.0f, 1024.0f), 22.0f);
	ImGuiSurfaceSync sync;
	SharedMenuModel model;

	const auto vrObs = RunSurfaceFrame(
		vrSurface, sync, desktopSurface, model, ImVec2(120.0f, 80.0f), ImVec2(1800.0f, 1100.0f));
	const auto desktopObs = RunSurfaceFrame(
		desktopSurface, sync, vrSurface, model, ImVec2(24.0f, 24.0f), ImVec2(920.0f, 860.0f));

	CHECK(vrObs.fontSize == 32.0f);
	CHECK(desktopObs.fontSize == 22.0f);
	CHECK(vrObs.windowSize.x == 1800.0f);
	CHECK(desktopObs.windowSize.x == 920.0f);
	CHECK(vrObs.windowPos.x == 120.0f);
	CHECK(desktopObs.windowPos.x == 24.0f);
}

TEST_CASE("ImGuiSurfaceSync synchronizes tabs, collapsing headers, text focus, and modal dismissal", "[menu][vr][imgui]")
{
	HeadlessSurface vrSurface(ImVec2(2560.0f, 1440.0f), 32.0f);
	HeadlessSurface desktopSurface(ImVec2(1024.0f, 1024.0f), 22.0f);
	ImGuiSurfaceSync sync;
	SharedMenuModel model;

	const ImVec2 vrPos(120.0f, 80.0f);
	const ImVec2 vrSize(1800.0f, 1100.0f);
	const ImVec2 desktopPos(24.0f, 24.0f);
	const ImVec2 desktopSize(920.0f, 860.0f);

	RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	auto vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	auto desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	REQUIRE(vrObs.activeTab == 0);
	REQUIRE(desktopObs.activeTab == 0);
	REQUIRE_FALSE(vrObs.detailsOpen);
	REQUIRE_FALSE(desktopObs.detailsOpen);

	ClickOnSurface(
		desktopSurface, vrSurface, sync, model, desktopObs.tab1Center, desktopPos, desktopSize, vrPos, vrSize);
	vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	CHECK(desktopObs.activeTab == 1);
	CHECK(vrObs.activeTab == 1);
	CHECK(vrObs.windowSize.x == 1800.0f);
	CHECK(desktopObs.windowSize.x == 920.0f);

	ClickOnSurface(
		vrSurface, desktopSurface, sync, model, vrObs.headerCenter, vrPos, vrSize, desktopPos, desktopSize);
	vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	CHECK(vrObs.detailsOpen);
	CHECK(desktopObs.detailsOpen);

	ClickOnSurface(
		desktopSurface, vrSurface, sync, model, desktopObs.inputCenter, desktopPos, desktopSize, vrPos, vrSize);
	REQUIRE(desktopSurface.context->ActiveId != 0);

	ClickOnSurface(
		vrSurface, desktopSurface, sync, model, vrObs.inputCenter, vrPos, vrSize, desktopPos, desktopSize);
	REQUIRE(vrSurface.context->ActiveId != 0);
	CHECK(desktopSurface.context->ActiveId == 0);

	{
		ScopedImGuiContext guard(vrSurface.context);
		ImGui::GetIO().AddInputCharacter('!');
	}
	RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);

	CHECK(std::string(model.searchBuffer.data()).find('!') != std::string::npos);

	model.showWarningModal = true;
	vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	REQUIRE(vrObs.modalOpen);
	REQUIRE(desktopObs.modalOpen);

	ClickOnSurface(
		vrSurface, desktopSurface, sync, model, vrObs.modalOkCenter, vrPos, vrSize, desktopPos, desktopSize);
	vrObs = RunSurfaceFrame(vrSurface, sync, desktopSurface, model, vrPos, vrSize);
	desktopObs = RunSurfaceFrame(desktopSurface, sync, vrSurface, model, desktopPos, desktopSize);
	CHECK_FALSE(vrObs.modalOpen);
	CHECK_FALSE(desktopObs.modalOpen);
}
