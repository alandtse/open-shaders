// Separate TU for Util::Subrect::OpaquePreviewBlendCallback. Split from
// Subrect.cpp because this needs the plugin's d3d singletons (globals::d3d),
// and Subrect.cpp is also compiled standalone by the unit-test target
// (tests/cpp/CMakeLists.txt) which has no PCH and no D3D context to bind.
// Plugin builds pick this up automatically via the src/*.cpp GLOB_RECURSE.

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Subrect.h"
#include "Utils/UI.h"
#include "Utils/UILayout.h"

#include <PCH.h>
#include <cstring>
#include <d3d11.h>
#include <imgui.h>

namespace Util::Subrect
{
	namespace
	{
		void FadedPreviewBlendCallback(const ImDrawList*, const ImDrawCmd* command)
		{
			auto* device = globals::d3d::device;
			auto* context = globals::d3d::context;
			if (!device || !context)
				return;
			static winrt::com_ptr<ID3D11BlendState> fadeBlend;
			if (!fadeBlend) {
				D3D11_BLEND_DESC desc{};
				auto& target = desc.RenderTarget[0];
				target.BlendEnable = TRUE;
				target.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
				target.DestBlend = D3D11_BLEND_ONE;
				target.BlendOp = D3D11_BLEND_OP_ADD;
				target.SrcBlendAlpha = D3D11_BLEND_ZERO;
				target.DestBlendAlpha = D3D11_BLEND_ONE;
				target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
				                               D3D11_COLOR_WRITE_ENABLE_GREEN |
				                               D3D11_COLOR_WRITE_ENABLE_BLUE;
				if (FAILED(device->CreateBlendState(&desc, fadeBlend.put()))) {
					context->OMSetBlendState(nullptr, nullptr, 0);
					return;
				}
				Util::SetResourceName(fadeBlend.get(), "Subrect::FadedPreviewBlend");
			}
			float opacity;
			std::memcpy(&opacity, command->UserCallbackData, sizeof(opacity));
			const float factor[]{ opacity, opacity, opacity, opacity };
			context->OMSetBlendState(fadeBlend.get(), factor, 0xFFFFFFFF);
		}
	}

	void OpaquePreviewBlendCallback(const ImDrawList*, const ImDrawCmd*)
	{
		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		if (!device || !context) {
			return;
		}

		static winrt::com_ptr<ID3D11BlendState> opaqueBlend;
		if (!opaqueBlend) {
			D3D11_BLEND_DESC desc{};
			desc.RenderTarget[0].BlendEnable = FALSE;
			desc.RenderTarget[0].RenderTargetWriteMask =
				D3D11_COLOR_WRITE_ENABLE_RED |
				D3D11_COLOR_WRITE_ENABLE_GREEN |
				D3D11_COLOR_WRITE_ENABLE_BLUE;
			if (FAILED(device->CreateBlendState(&desc, opaqueBlend.put()))) {
				return;
			}
			Util::SetResourceName(opaqueBlend.get(), "Subrect::OpaquePreviewBlend");
		}
		if (opaqueBlend) {
			context->OMSetBlendState(opaqueBlend.get(), nullptr, 0xFFFFFFFF);
		}
	}

	void ImageOpaque(ID3D11ShaderResourceView* a_srv, const ImVec2& a_size, const ImVec2& a_uv0, const ImVec2& a_uv1)
	{
		if (!a_srv) {
			return;
		}
		auto* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		const ImVec2 imageMax(imageMin.x + a_size.x, imageMin.y + a_size.y);
		drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(IM_COL32_BLACK));
		float opacity = std::clamp(ImGui::GetStyle().Alpha, 0.0f, 1.0f);
		if (opacity <= 0.0f) {
			ImGui::Dummy(a_size);
			return;
		}
		if (opacity < 1.0f)
			drawList->AddCallback(FadedPreviewBlendCallback, &opacity, sizeof(opacity));
		else
			drawList->AddCallback(OpaquePreviewBlendCallback, nullptr);
		ImGui::Image(reinterpret_cast<ImTextureID>(a_srv), a_size, a_uv0, a_uv1);
		drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
	}
	void Controller::DrawEditor(ID3D11ShaderResourceView* previewSrv, ID3D11Texture2D* previewTexture, float uvVisibleWidth, float uvStartX, ImDrawCallback imageRenderCallback)
	{
		const float availableWidth = ImGui::GetContentRegionAvail().x;
		if (availableWidth <= 0.0f)
			return;
		const auto& style = ImGui::GetStyle();
		const float uiScale = Util::GetUIScale();
		const auto prepareFieldLabel = [&](const char* label, const char* id, float width) {
			const bool stacked = width + ImGui::CalcTextSize(label).x + style.ItemInnerSpacing.x > availableWidth;
			if (stacked)
				ImGui::TextWrapped("%s", label);
			ImGui::SetNextItemWidth(std::min(width, availableWidth));
			return std::string(stacked ? "" : label) + "###" + id;
		};
		const auto buttonWidth = [&](const char* label) {
			return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
		};

		// Hosts that render without first calling LoadSettings would otherwise
		// see an empty presets vector and the combo would mislabel as "(Custom)".
		EnsureDefaultPreset();
		if (selectedPresetIndex >= static_cast<int>(presets.size())) {
			selectedPresetIndex = -1;
		}

		std::string currentPreview =
			(selectedPresetIndex >= 0 && selectedPresetIndex < static_cast<int>(presets.size())) ? presets[selectedPresetIndex].name : T("ui.subrect.custom", "(Custom)");

		const auto presetLabel = prepareFieldLabel(T("ui.subrect.crop_preset", "Crop Preset"), "SubrectCropPreset", ImGui::CalcItemWidth());
		if (ImGui::BeginCombo(presetLabel.c_str(), currentPreview.c_str())) {
			for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
				const bool isSelected = selectedPresetIndex == i;
				if (ImGui::Selectable(presets[i].name.c_str(), isSelected)) {
					ApplyPreset(i);
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		const auto saveAsLabel = prepareFieldLabel(T("ui.subrect.save_as", "Save As"), "SubrectSaveAs", ImGui::CalcItemWidth());
		ImGui::InputText(saveAsLabel.c_str(), newPresetName, sizeof(newPresetName));
		const char* saveLabel = T("ui.subrect.save_preset", "Save Preset");
		Util::SameLineIfFits(buttonWidth(saveLabel));
		if (Util::Button(saveLabel)) {
			std::string presetName = newPresetName;
			if (!presetName.empty()) {
				// Preserve the right-eye UV only when stereo is on. In mono
				// mode currentRightUV is not tracked against currentUV, so
				// snapshotting it would falsely mark the preset as having an
				// explicit right eye and disable the auto-mirror fallback
				// once stereo is later enabled. Leave rightUV as nullopt in
				// mono — ApplyPreset will mirror left at apply time.
				Preset newPreset{ .name = presetName, .uv = currentUV };
				if (stereoEnabled) {
					newPreset.rightUV = currentRightUV;
				}
				presets.push_back(std::move(newPreset));
				selectedPresetIndex = static_cast<int>(presets.size()) - 1;
				newPresetName[0] = '\0';
			}
		}

		if (selectedPresetIndex > 0) {
			const char* deleteLabel = T("ui.subrect.delete_preset", "Delete Preset");
			Util::SameLineIfFits(buttonWidth(deleteLabel));
			if (Util::Button(deleteLabel)) {
				presets.erase(presets.begin() + selectedPresetIndex);
				ApplyPreset(0);
			}
		}

		const char* resetLabel = T("ui.subrect.reset_crop", "Reset Crop");
		Util::SameLineIfFits(buttonWidth(resetLabel));
		if (Util::Button(resetLabel)) {
			ApplyPreset(0);
		}

		ImGui::Spacing();
		const float sliderWidth = ThemeManager::Constants::SUBRECT_SLIDER_WIDTH * uiScale;
		bool changed = false;
		const auto positionLabel = prepareFieldLabel(T("ui.subrect.position_uv", "Position UV (X, Y)"), "SubrectPositionUV", sliderWidth);
		changed |= ImGui::SliderFloat2(positionLabel.c_str(), &currentUV.x, 0.0f, 1.0f, "%.3f");
		const auto sizeLabel = prepareFieldLabel(T("ui.subrect.size_uv", "Size UV (W, H)"), "SubrectSizeUV", sliderWidth);
		changed |= ImGui::SliderFloat2(sizeLabel.c_str(), &currentUV.w, 0.01f, 1.0f, "%.3f");

		if (changed) {
			selectedPresetIndex = -1;
			ClampCurrentUV();
			if (stereoEnabled) {
				SyncRightUV();
			}
		}

		ImGui::Spacing();
		ImGui::TextWrapped("%s", T("ui.subrect.interactive_cropping", "Interactive Cropping (Drag on the image to select)"));

		if (!previewSrv || !previewTexture) {
			ImGui::TextDisabled("%s", T("ui.subrect.preview_unavailable", "Preview unavailable."));
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		previewTexture->GetDesc(&desc);
		float maxWidth = std::min(ThemeManager::Constants::SUBRECT_PREVIEW_WIDTH * uiScale, ImGui::GetContentRegionAvail().x);
		if (maxWidth <= 0.0f || desc.Width == 0 || desc.Height == 0 || !std::isfinite(uvVisibleWidth) || uvVisibleWidth <= 0.0f)
			return;
		float aspectRatio = (static_cast<float>(desc.Width) * uvVisibleWidth) / static_cast<float>(desc.Height);
		ImVec2 imageSize(maxWidth, maxWidth / aspectRatio);
		ImVec2 cursorPos = ImGui::GetCursorScreenPos();

		ImDrawList* hostDrawList = ImGui::GetWindowDrawList();
		if (imageRenderCallback) {
			const ImVec2 imageMin(cursorPos.x + style.ImageBorderSize, cursorPos.y + style.ImageBorderSize);
			hostDrawList->AddRectFilled(imageMin, ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y), ImGui::GetColorU32(IM_COL32_BLACK), style.ImageRounding);
			hostDrawList->AddCallback(imageRenderCallback, nullptr);
		}
		ImGui::Image(reinterpret_cast<ImTextureID>(previewSrv), imageSize,
			ImVec2(uvStartX, 0.0f), ImVec2(uvStartX + uvVisibleWidth, 1.0f));
		if (imageRenderCallback) {
			hostDrawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		}

		ImGui::SetCursorScreenPos(cursorPos);
		ImGui::SetNextItemAllowOverlap();
		ImGui::InvisibleButton("##subrectCanvas", imageSize);

		ImVec2 mousePos = ImGui::GetIO().MousePos;
		ImVec2 relativeMouseP(mousePos.x - cursorPos.x, mousePos.y - cursorPos.y);
		float mouseUVX = std::clamp(relativeMouseP.x / imageSize.x, 0.0f, 1.0f);
		float mouseUVY = std::clamp(relativeMouseP.y / imageSize.y, 0.0f, 1.0f);

		if (ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			isDraggingCrop = true;
			selectedPresetIndex = -1;
			dragStartUV[0] = mouseUVX;
			dragStartUV[1] = mouseUVY;
			currentUV.x = mouseUVX;
			currentUV.y = mouseUVY;
			currentUV.w = 0.0f;
			currentUV.h = 0.0f;
		}

		if (isDraggingCrop) {
			float minX = std::min(dragStartUV[0], mouseUVX);
			float minY = std::min(dragStartUV[1], mouseUVY);
			float maxX = std::max(dragStartUV[0], mouseUVX);
			float maxY = std::max(dragStartUV[1], mouseUVY);

			currentUV.x = minX;
			currentUV.y = minY;
			currentUV.w = maxX - minX;
			currentUV.h = maxY - minY;
			ClampCurrentUV();
			if (stereoEnabled) {
				SyncRightUV();
			}

			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				isDraggingCrop = false;
			}
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 pMin(cursorPos.x + currentUV.x * imageSize.x, cursorPos.y + currentUV.y * imageSize.y);
		ImVec2 pMax(cursorPos.x + (currentUV.x + currentUV.w) * imageSize.x,
			cursorPos.y + (currentUV.y + currentUV.h) * imageSize.y);
		drawList->AddRect(pMin, pMax, ImGui::GetColorU32(IM_COL32(0, 255, 0, 255)), 0.0f, 0, ThemeManager::Constants::SUBRECT_OUTLINE_WIDTH * uiScale);
	}

}  // namespace Util::Subrect
