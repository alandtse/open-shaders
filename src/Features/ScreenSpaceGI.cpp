#include "ScreenSpaceGI.h"

#include <DirectXTex.h>

#include "Deferred.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableExperimentalSpecularGI,
	EnableVanillaSSAO,
	InteriorsOnly,
	NumSlices,
	NumSteps,
	ResolutionMode,
	VRCullDistance,
	CenterFullResMaskScale,
	MinScreenRadius,
	AORadius,
	GIRadius,
	Thickness,
	DepthFadeRange,
	GISaturation,
	GIDistanceCompensation,
	AOPower,
	GIStrength,
	EnableTemporalDenoiser,
	EnableBlur,
	DepthDisocclusion,
	NormalDisocclusion,
	MaxAccumFrames,
	BlurRadius,
	DistanceNormalisation)

namespace
{
	constexpr float kVRCullDistanceMin = 0.0f;
	constexpr float kVRCullDistanceMax = 20480.0f;
	constexpr float kCenterMaskScaleMin = 0.45f;
	constexpr float kCenterMaskScaleMax = 1.0f;
	constexpr float kCenterMaskUiOffThreshold = 0.03f;
	constexpr float kCenterMaskFeather = 0.05f;
	constexpr int kResolutionModeMin = 0;
	constexpr int kResolutionModeMax = 2;

	float ClampVRCullDistance(float a_distance)
	{
		return std::clamp(a_distance, kVRCullDistanceMin, kVRCullDistanceMax);
	}

	int ClampResolutionMode(int a_resolutionMode)
	{
		return std::clamp(a_resolutionMode, kResolutionModeMin, kResolutionModeMax);
	}

	float ClampCenterMaskScale(float a_scale)
	{
		if (a_scale <= 0.0f)
			return 0.0f;
		return std::clamp(a_scale, kCenterMaskScaleMin, kCenterMaskScaleMax);
	}

	void ApplyPlatformSettingOverrides(ScreenSpaceGI::Settings& a_settings)
	{
		a_settings.ResolutionMode = ClampResolutionMode(a_settings.ResolutionMode);
		a_settings.VRCullDistance = ClampVRCullDistance(a_settings.VRCullDistance);
		a_settings.CenterFullResMaskScale = ClampCenterMaskScale(a_settings.CenterFullResMaskScale);
	}

	float2 GetHardenedSsgiFrameDim(float2 a_renderTexSize)
	{
		float2 frameDim = Util::ConvertToDynamic(a_renderTexSize);
		frameDim = { floor(frameDim.x), floor(frameDim.y) };

		auto* depthSRV = Util::GetCurrentSceneDepthSRV();
		uint32_t depthWidth = 0;
		uint32_t depthHeight = 0;
		if (!Util::TryGetDepthSrvDimensions(depthSRV, depthWidth, depthHeight))
			return { std::max(1.0f, frameDim.x), std::max(1.0f, frameDim.y) };

		float scaleX = frameDim.x / a_renderTexSize.x;  // runtime ratio fallback
		float scaleY = frameDim.y / static_cast<float>(depthHeight);
		scaleY = std::clamp(scaleY, 0.25f, 2.0f);

		if (!REL::Module::IsVR()) {
			scaleX = frameDim.x / static_cast<float>(depthWidth);
		} else {
			const float perEyeFrameWidth = frameDim.x * 0.5f;
			const float combinedX = (perEyeFrameWidth * 2.0f) / static_cast<float>(depthWidth);
			const float perEyeX = perEyeFrameWidth / static_cast<float>(depthWidth);

			const int viewportWidthPerEye = static_cast<int>(std::floor(a_renderTexSize.x * 0.5f));
			switch (Util::DetectVRDepthLayout(depthWidth, viewportWidthPerEye)) {
			case Util::VRDepthLayout::CombinedStereo:
				scaleX = combinedX;
				break;
			case Util::VRDepthLayout::PerEye:
				scaleX = perEyeX;
				break;
			case Util::VRDepthLayout::Unknown:
			default:
				scaleX = std::abs(combinedX - scaleX) <= std::abs(perEyeX - scaleX) ? combinedX : perEyeX;
				break;
			}
		}

		scaleX = std::clamp(scaleX, 0.25f, 2.0f);

		float2 hardenedFrameDim = {
			floor(a_renderTexSize.x * scaleX),
			floor(a_renderTexSize.y * scaleY)
		};
		hardenedFrameDim.x = std::max(1.0f, hardenedFrameDim.x);
		hardenedFrameDim.y = std::max(1.0f, hardenedFrameDim.y);
		return hardenedFrameDim;
	}
}

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
	ApplyPlatformSettingOverrides(settings);
	recompileFlag = true;
}

void ScreenSpaceGI::DrawSettings()
{
	ApplyPlatformSettingOverrides(settings);
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "Compute shaders failed to compile!");

	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	ImGui::Checkbox("Show Advanced Options", &showAdvanced);

	if (ImGui::BeginTable("Toggles", 4)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enable Screen Space Global Illumination. When disabled, all other settings are ignored.");
		}

		ImGui::TableNextColumn();
		{
			auto ilToggleGuard = Util::DisableGuard(!settings.Enabled);
			recompileFlag |= ImGui::Checkbox("Indirect Lighting (IL)", &settings.EnableGI);
		}
		ImGui::TableNextColumn();
		ImGui::Checkbox("Vanilla SSAO", &settings.EnableVanillaSSAO);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enable Skyrim's built-in SSAO. Usually disabled when using SSGI to avoid double-darkening.");
		}
		ImGui::TableNextColumn();
		if (showAdvanced) {
			recompileFlag |= ImGui::Checkbox("(Experimental) HQ Specular IL", &settings.EnableExperimentalSpecularGI);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("An experimental specular GI that is more accurate but requires more samples. Won't be blurred.");
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Quality/Performance");

	{
		auto qualityGuard = Util::DisableGuard(!settings.Enabled);
		auto drawGreyPresetButton = [](const char* a_label, const ImVec2& a_size) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.34f, 0.34f, 0.34f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.42f, 0.42f, 0.42f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.48f, 0.48f, 0.48f, 1.0f));
			const bool clicked = ImGui::Button(a_label, a_size);
			ImGui::PopStyleColor(3);
			return clicked;
		};

		{
			Util::BlueFrameStyleWrapper interiorsBlueStyle(true);
			ImGui::Checkbox("Interiors Only", &settings.InteriorsOnly);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Run SSGI only in interiors to improve exterior performance.");
		}

		if (ImGui::BeginTable("Presets", 4, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("PresetAO", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("PresetFoveatedQuality", ImGuiTableColumnFlags_WidthStretch, 1.35f);
			ImGui::TableSetupColumn("PresetFoveatedPerformance", ImGuiTableColumnFlags_WidthStretch, 1.35f);
			ImGui::TableSetupColumn("PresetReference", ImGuiTableColumnFlags_WidthStretch, 1.0f);

			ImGui::TableNextColumn();
			if (ImGui::Button("AO only", { -1, 0 })) {
				settings.NumSlices = 3;
				settings.NumSteps = 6;
				settings.ResolutionMode = 0;
				settings.CenterFullResMaskScale = 0.0f;
				settings.VRCullDistance = 1500.0f;
				settings.AOPower = 1.8f;
				settings.EnableBlur = false;
				settings.EnableTemporalDenoiser = false;
				settings.EnableGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Full Res, no GI, no Foveated; use Half Res or Foveated for more performance.");

			ImGui::TableNextColumn();
			if (drawGreyPresetButton("Foveated Quality", { -1, 0 })) {
				settings.NumSlices = 3;
				settings.NumSteps = 6;
				settings.ResolutionMode = 1;
				settings.CenterFullResMaskScale = 0.75f;
				settings.VRCullDistance = 1500.0f;
				settings.AOPower = 1.8f;
				settings.EnableBlur = false;
				settings.EnableTemporalDenoiser = false;
				settings.EnableGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("AO, no GI, Half Res periphery. Use slider to adjust center view. Slider at 0 = Half Res.");

			ImGui::TableNextColumn();
			if (drawGreyPresetButton("Foveated Perf.", { -1, 0 })) {
				settings.NumSlices = 3;
				settings.NumSteps = 6;
				settings.ResolutionMode = 2;
				settings.CenterFullResMaskScale = 0.75f;
				settings.VRCullDistance = 1500.0f;
				settings.AOPower = 1.8f;
				settings.EnableBlur = false;
				settings.EnableTemporalDenoiser = false;
				settings.EnableGI = false;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("AO, no GI, Quarter Res periphery. Use slider to adjust center view. Slider at 0 = Quarter Res.");

			ImGui::TableNextColumn();
			if (ImGui::Button("Reference", { -1, 0 })) {
				settings.NumSlices = 8;
				settings.NumSteps = 10;
				settings.ResolutionMode = 0;
				settings.EnableBlur = true;
				settings.EnableGI = true;
				recompileFlag = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Reference mode.");

			ImGui::EndTable();
		}

		if (REL::Module::IsVR()) {
			ImGui::SliderFloat("Shadow/GI Cull Distance", &settings.VRCullDistance, kVRCullDistanceMin, kVRCullDistanceMax, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("0 disables. Lower values improve performance but reduce distant AO/IL.");
			}
			settings.VRCullDistance = ClampVRCullDistance(settings.VRCullDistance);
		}

		if (showAdvanced) {
			ImGui::SliderInt("Slices", (int*)&settings.NumSlices, 1, 10);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many directions do the samples take.\n"
					"Controls noise.");

			ImGui::SliderInt("Steps Per Slice", (int*)&settings.NumSteps, 1, 20);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"How many samples does it take in one direction.\n"
					"Controls accuracy of lighting, and noise when effect radius is large.");
		}

		const int previousResolutionMode = settings.ResolutionMode;
		settings.ResolutionMode = ClampResolutionMode(settings.ResolutionMode);

		ImGui::RadioButton("Full Res", &settings.ResolutionMode, 0);
		constexpr float kPresetTableTotalWeight = 1.0f + 1.35f + 1.35f + 1.0f;
		constexpr float kSecondPresetColumnStartRatio = 1.0f / kPresetTableTotalWeight;
		const float groupStartX = ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x * kSecondPresetColumnStartRatio);
		ImGui::SameLine(groupStartX);

		ImGui::BeginGroup();
		ImGui::TextUnformatted("Foveated SSGI");
		ImGui::SameLine(0.0f, 14.0f);
		ImGui::RadioButton("Half Res", &settings.ResolutionMode, 1);
		ImGui::SameLine(0.0f, 14.0f);
		ImGui::RadioButton("Quarter Res", &settings.ResolutionMode, 2);
		ImGui::EndGroup();

		{
			const ImVec2 groupMin = ImGui::GetItemRectMin();
			const ImVec2 groupMax = ImGui::GetItemRectMax();
			const ImVec2 borderPad(6.0f, 4.0f);
			ImVec4 borderColorF = ImGui::GetStyleColorVec4(ImGuiCol_Button);
			borderColorF.w = 0.86f;
			const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(borderColorF);
			ImGui::GetWindowDrawList()->AddRect(
				{ groupMin.x - borderPad.x, groupMin.y - borderPad.y },
				{ groupMax.x + borderPad.x, groupMax.y + borderPad.y },
				borderColor,
				3.0f,
				0,
				1.0f);
		}

		settings.ResolutionMode = ClampResolutionMode(settings.ResolutionMode);
		recompileFlag |= (settings.ResolutionMode != previousResolutionMode);

		if (settings.ResolutionMode != 0) {
			float centerMaskUiValue = settings.CenterFullResMaskScale;
			const bool centerMaskOff = centerMaskUiValue <= 0.0f;

			const ImVec4 buttonColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
			const ImVec4 buttonHovered = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
			const ImVec4 buttonActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, buttonColor);
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, buttonHovered);
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, buttonActive);
			ImGui::PushStyleColor(ImGuiCol_SliderGrab, buttonHovered);
			ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, buttonActive);

			ImGui::SliderFloat("Foveated Area", &centerMaskUiValue, 0.0f, kCenterMaskScaleMax, centerMaskOff ? "Off" : "%.2f");
			ImGui::PopStyleColor(5);

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Slider far left: Foveated off: pure %s. Use slider to adjust foveated area. 1= Full Res.", settings.ResolutionMode == 1 ? "Half Res" : "Quarter Res");
			}

			if (centerMaskUiValue <= kCenterMaskUiOffThreshold)
				settings.CenterFullResMaskScale = 0.0f;
			else
				settings.CenterFullResMaskScale = ClampCenterMaskScale(centerMaskUiValue);
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	{
		auto visualGuard = Util::DisableGuard(!settings.Enabled);

		ImGui::SliderFloat("AO Power", &settings.AOPower, 0.f, 6.f, "%.2f");

		{
			auto ilGuard = Util::DisableGuard(!settings.EnableGI);
			ImGui::SliderFloat("IL Source Brightness", &settings.GIStrength, 0.f, 6.f, "%.2f");
		}

		ImGui::Separator();

		ImGui::SliderFloat("AO radius", &settings.AORadius, 10.f, 1024.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"A smaller radius produces tighter AO.",
				Util::Units::FormatDistance(settings.AORadius)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		{
			auto ilRadiusGuard = Util::DisableGuard(!settings.EnableGI);

			ImGui::SliderFloat("IL radius", &settings.GIRadius, 10.f, 1024.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"A larger radius produces wider IL.",
					Util::Units::FormatDistance(settings.GIRadius)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}

		if (showAdvanced) {
			ImGui::SliderFloat("Min Screen Radius", &settings.MinScreenRadius, 0.f, 0.05f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"The minimum screen-space effect radius as proportion of display width, to prevent far field AO being too small.");
		}

		ImGui::SliderFloat2("Depth Fade Range", &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			std::vector<std::string> tooltipLines = {
				"Distance range where depth-based effects fade out.",
				"Near: " + Util::Units::FormatDistance(settings.DepthFadeRange.x),
				"Far: " + Util::Units::FormatDistance(settings.DepthFadeRange.y)
			};
			Util::DrawMultiLineTooltip(tooltipLines);
		}

		if (showAdvanced) {
			ImGui::Separator();

			ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 128.0f, "%.1f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				std::vector<std::string> tooltipLines = {
					"How thick the occluders are. Only affects AO.",
					Util::Units::FormatDistance(settings.Thickness)
				};
				Util::DrawMultiLineTooltip(tooltipLines);
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual - IL");

	{
		auto visualILGuard = Util::DisableGuard(!settings.Enabled || !settings.EnableGI);

		if (showAdvanced) {
			ImGui::SliderFloat("IL Distance Compensation", &settings.GIDistanceCompensation, -5.0f, 5.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brighten/Dimming further radiance samples.");

			ImGui::Separator();
		}

		Util::PercentageSlider("IL Saturation", &settings.GISaturation);
	}

	///////////////////////////////
	ImGui::SeparatorText("Denoising");

	{
		auto denoiseGuard = Util::DisableGuard(!settings.Enabled);

		if (ImGui::BeginTable("denoisers", 2)) {
			ImGui::TableNextColumn();
			recompileFlag |= ImGui::Checkbox("Temporal Denoiser", &settings.EnableTemporalDenoiser);

			ImGui::TableNextColumn();
			ImGui::Checkbox("Blur", &settings.EnableBlur);

			ImGui::EndTable();
		}

		if (showAdvanced) {
			ImGui::Separator();

			{
				auto temporalGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser);
				ImGui::SliderInt("Max Frame Accumulation", (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting.");
			}

			ImGui::Separator();

			{
				auto disocclusionGuard = Util::DisableGuard(!settings.EnableTemporalDenoiser && !settings.EnableGI);

				Util::PercentageSlider("Movement Disocclusion", &settings.DepthDisocclusion, 0.f, 20.f);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"If a pixel has moved too far from the last frame, its radiance will not be carried to this frame.\n"
						"Lower values are stricter.");

				ImGui::Separator();
			}

			{
				auto blurGuard = Util::DisableGuard(!settings.EnableBlur);
				ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0.f, 30.f, "%.1f px");

				ImGui::SliderFloat("Geometry Weight", &settings.DistanceNormalisation, 0.f, 5.f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"Higher value makes the blur more sensitive to differences in geometry.");
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texAo[0], debugRescale)
		BUFFER_VIEWER_NODE(texAo[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlY[1], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[0], debugRescale)
		BUFFER_VIEWER_NODE(texIlCoCg[1], debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;
	ApplyPlatformSettingOverrides(settings);

	recompileFlag = true;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>());
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc);
			texRadiance->CreateSRV(srvDesc);
			texRadiance->CreateUAV(uavDesc);  // Create default UAV for mip 0

			// Create individual UAVs for each mip level for prefiltering
			for (uint i = 0; i < 5; ++i) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
					.Format = DXGI_FORMAT_R11G11B10_FLOAT,
					.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
					.Texture2D = { .MipSlice = i }
				};
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texRadiance->resource.get(), &mipUavDesc, uavRadiance[i].put()));
			}

			// Create temporary texture for prefiltering (single mip level, used as SRV input)
			D3D11_TEXTURE2D_DESC tempTexDesc = texDesc;
			tempTexDesc.MipLevels = 1;
			tempTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			D3D11_SHADER_RESOURCE_VIEW_DESC tempSrvDesc = {
				.Format = DXGI_FORMAT_R11G11B10_FLOAT,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = {
					.MostDetailedMip = 0,
					.MipLevels = 1 }
			};

			texRadianceTemp = eastl::make_unique<Texture2D>(tempTexDesc);
			texRadianceTemp->CreateSRV(tempSrvDesc);
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc);
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texIlY[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[0]->CreateSRV(srvDesc);
			texIlY[0]->CreateUAV(uavDesc);

			texIlY[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlY[1]->CreateSRV(srvDesc);
			texIlY[1]->CreateUAV(uavDesc);

			texGiSpecular[0] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[0]->CreateSRV(srvDesc);
			texGiSpecular[0]->CreateUAV(uavDesc);

			texGiSpecular[1] = eastl::make_unique<Texture2D>(texDesc);
			texGiSpecular[1]->CreateSRV(srvDesc);
			texGiSpecular[1]->CreateUAV(uavDesc);
		}
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		{
			texIlCoCg[0] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[0]->CreateSRV(srvDesc);
			texIlCoCg[0]->CreateUAV(uavDesc);

			texIlCoCg[1] = eastl::make_unique<Texture2D>(texDesc);
			texIlCoCg[1]->CreateSRV(srvDesc);
			texIlCoCg[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UNORM;
		{
			texAo[0] = eastl::make_unique<Texture2D>(texDesc);
			texAo[0]->CreateSRV(srvDesc);
			texAo[0]->CreateUAV(uavDesc);

			texAo[1] = eastl::make_unique<Texture2D>(texDesc);
			texAo[1]->CreateSRV(srvDesc);
			texAo[1]->CreateUAV(uavDesc);

			texAccumFrames[0] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[0]->CreateSRV(srvDesc);
			texAccumFrames[0]->CreateUAV(uavDesc);

			texAccumFrames[1] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[1]->CreateSRV(srvDesc);
			texAccumFrames[1]->CreateUAV(uavDesc);

			texCenterAo = eastl::make_unique<Texture2D>(texDesc);
			texCenterAo->CreateSRV(srvDesc);
			texCenterAo->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texCenterIlY = eastl::make_unique<Texture2D>(texDesc);
			texCenterIlY->CreateSRV(srvDesc);
			texCenterIlY->CreateUAV(uavDesc);

			texCenterGiSpecular = eastl::make_unique<Texture2D>(texDesc);
			texCenterGiSpecular->CreateSRV(srvDesc);
			texCenterGiSpecular->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		{
			texCenterIlCoCg = eastl::make_unique<Texture2D>(texDesc);
			texCenterIlCoCg->CreateSRV(srvDesc);
			texCenterIlCoCg->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc);
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute,
		&prefilterRadianceCompute,
		&radianceDisoccCompute,
		&giCompute,
		&centerGIMaskedCompute,
		&blurCompute,
		&upsampleCompute,
		&centerBlendCompute
	};

	for (auto shader : shaderPtrs)
		*shader = nullptr;

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		bool includeResolutionDefines = true;
		bool includeTemporalDefines = true;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", { { "LINEAR_FILTER", "" } } },
			{ &prefilterRadianceCompute, "prefilterRadiance.cs.hlsl", {} },
			{ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} },
			{ &giCompute, "gi.cs.hlsl", {} },
			{ &centerGIMaskedCompute, "gi.cs.hlsl", { { "CENTER_FULL_PASS", "" } }, false, false },
			{ &blurCompute, "blur.cs.hlsl", {} },
			{ &upsampleCompute, "upsample.cs.hlsl", {} },
			{ &centerBlendCompute, "centerBlend.cs.hlsl", {}, false, false },
		};
	for (auto& info : shaderInfos) {
		if (REL::Module::IsVR())
			info.defines.push_back({ "VR", "" });
		if (info.includeResolutionDefines) {
			if (settings.ResolutionMode == 1)
				info.defines.push_back({ "HALF_RES", "" });
			if (settings.ResolutionMode == 2)
				info.defines.push_back({ "QUARTER_RES", "" });
		}
		if (info.includeTemporalDefines && settings.EnableTemporalDenoiser)
			info.defines.push_back({ "TEMPORAL_DENOISER", "" });
		if (settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (settings.EnableExperimentalSpecularGI)
			info.defines.push_back({ "GI_SPECULAR", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	const bool baseShadersOK = texNoise &&
	                           prefilterDepthsCompute &&
	                           prefilterRadianceCompute &&
	                           radianceDisoccCompute &&
	                           giCompute &&
	                           blurCompute &&
	                           upsampleCompute;

	const bool centerShadersOK = texCenterAo &&
	                             texCenterIlY &&
	                             texCenterIlCoCg &&
	                             texCenterGiSpecular &&
	                             centerGIMaskedCompute &&
	                             centerBlendCompute;
	const float centerScale = ClampCenterMaskScale(settings.CenterFullResMaskScale);
	const bool centerMaskActive = centerScale > 0.0f && centerScale < 0.999f;

	// Keep legacy SSGI path fully functional when center mask is off.
	if (ClampResolutionMode(settings.ResolutionMode) == 0 || !centerMaskActive)
		return baseShadersOK;

	return baseShadersOK && centerShadersOK;
}

void ScreenSpaceGI::UpdateSB()
{
	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = GetHardenedSsgiFrameDim(res);

	static float4x4 prevInvView[2] = {};

	SSGICB data;
	{
		const bool useUnjitteredCamera = REL::Module::IsVR();

		for (int eyeIndex = 0; eyeIndex < (1 + REL::Module::IsVR()); ++eyeIndex) {
			const auto eye = Util::GetCameraData(eyeIndex);
			float proj11 = eye.projMat(0, 0);
			float proj22 = eye.projMat(1, 1);
			float4x4 currentInvView = eye.viewMat.Invert();

			if (useUnjitteredCamera) {
				const auto& projUnjittered = globals::game::frameBufferCached.GetCameraProjUnjittered(eyeIndex);
				proj11 = projUnjittered._11;
				proj22 = projUnjittered._22;
				currentInvView = globals::game::frameBufferCached.GetCameraViewInverse(eyeIndex);
			}

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = { 2.0f / proj11, -2.0f / proj22 };
			data.NDCToViewAdd[eyeIndex] = { -1.0f / proj11, 1.0f / proj22 };
			if (REL::Module::IsVR())
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = currentInvView;
		}

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = globals::state->frameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.MinScreenRadius = settings.MinScreenRadius * dynres.x;

		data.EffectRadius = std::max(settings.AORadius, settings.GIRadius);
		const float safeEffectRadius = std::max(data.EffectRadius, 1e-3f);
		data.EffectRadius = safeEffectRadius;
		data.AORadius = settings.AORadius / safeEffectRadius;
		data.GIRadius = settings.GIRadius / safeEffectRadius;
		data.Thickness = settings.Thickness;
		const float depthFadeStart = std::min(settings.DepthFadeRange.x, settings.DepthFadeRange.y);
		const float depthFadeEnd = std::max(settings.DepthFadeRange.x, settings.DepthFadeRange.y);
		data.DepthFadeRange = { depthFadeStart, depthFadeEnd };
		const float depthFadeSpan = std::max(depthFadeEnd - depthFadeStart, 1.0f);
		data.DepthFadeScaleConst = 1.0f / depthFadeSpan;

		data.GISaturation = settings.GISaturation;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.AORadius;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.NormalDisocclusion = settings.NormalDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
		data.BlurRadius = settings.BlurRadius;
		data.DistanceNormalisation = settings.DistanceNormalisation;
		data.VRCullDistance = REL::Module::IsVR() ? ClampVRCullDistance(settings.VRCullDistance) : 0.0f;
		data.CenterFullResMaskScale = ClampCenterMaskScale(settings.CenterFullResMaskScale);
		data.CenterFullResMaskFeather = kCenterMaskFeather;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI()
{
	auto context = globals::d3d::context;
	const int resolutionMode = ClampResolutionMode(settings.ResolutionMode);
	const bool runRadianceDisoccPass = settings.EnableGI || settings.EnableTemporalDenoiser;
	const bool runPrefilterRadiancePass = settings.EnableGI;
	const float centerScale = ClampCenterMaskScale(settings.CenterFullResMaskScale);
	const bool centerShadersReady = texCenterAo &&
	                                texCenterIlY &&
	                                texCenterIlCoCg &&
	                                texCenterGiSpecular &&
	                                centerGIMaskedCompute &&
	                                centerBlendCompute;
	const bool useCenterFullMask = centerShadersReady &&
	                               (resolutionMode != 0) &&
	                               (centerScale > 0.0f) &&
	                               (centerScale < 0.999f);

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISSAOBlurH, imageSpaceManager);

	// Toggle vanilla SSAO
	static bool* enableSSAO = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISSAOBlurH.get()) + 0x50LL);
	*enableSSAO = settings.EnableVanillaSSAO;

	const bool allowCurrentSpace = !settings.InteriorsOnly || Util::IsInterior();
	if (!(settings.Enabled && ShadersOK() && allowCurrentSpace)) {
		FLOAT clr[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearUnorderedAccessViewFloat(texAo[outputAoIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlY[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texIlCoCg[outputIlIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texGiSpecular[outputAoIdx]->uav.get(), clr);
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSGI");

	static uint lastFrameAoTexIdx = 0;
	static uint lastFrameGITexIdx = 0;
	static uint lastFrameAccumTexIdx = 0;
	uint inputAoTexIdx = lastFrameAoTexIdx;
	uint inputGITexIdx = lastFrameGITexIdx;

	//////////////////////////////////////////////////////

	if (recompileFlag)
		ClearShaderCache();

	UpdateSB();

	//////////////////////////////////////////////////////

	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = globals::deferred;

	float2 size = {
		(float)texRadiance->desc.Width,
		(float)texRadiance->desc.Height
	};
	size = GetHardenedSsgiFrameDim(size);
	auto resolution = std::array{ (uint)size.x, (uint)size.y };
	auto resChoices = std::array{
		resolution, std::array{ resolution[0] >> 1, resolution[1] >> 1 }, std::array{ resolution[0] >> 2, resolution[1] >> 2 }
	};
	auto internalRes = resChoices[resolutionMode];

	std::array<ID3D11ShaderResourceView*, 11> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 6> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Depths");

		srvs.at(0) = Util::GetCurrentSceneDepthSRV();
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion (optional in AO-only + no temporal mode)
	if (runRadianceDisoccPass) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Radiance Disocc");

		resetViews();
		srvs.at(0) = rts[deferred->forwardRenderTargets[0]].SRV;
		srvs.at(1) = texWorkingDepth->srv.get();
		srvs.at(2) = rts[NORMALROUGHNESS].SRV;
		srvs.at(3) = texPrevGeo->srv.get();
		srvs.at(4) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs.at(5) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(6) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(7) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(8) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(9) = texGiSpecular[inputAoTexIdx]->srv.get();
		srvs.at(10) = nullptr;

		uavs.at(0) = texRadiance->uav.get();
		uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(2) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(3) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(4) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(5) = texGiSpecular[!inputAoTexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(radianceDisoccCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		// Prefilter radiance texture only when GI is enabled.
		if (runPrefilterRadiancePass) {
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Prefilter Radiance");

			// First copy mip 0 from radiance to temporary texture to avoid read/write conflict
			context->CopySubresourceRegion(
				texRadianceTemp->resource.get(), 0, 0, 0, 0,
				texRadiance->resource.get(), 0, nullptr);

			resetViews();
			srvs.at(0) = texRadianceTemp->srv.get();  // Use temporary texture as input
			uavs.at(0) = uavRadiance[0].get();        // Mip 0
			uavs.at(1) = uavRadiance[1].get();        // Mip 1
			uavs.at(2) = uavRadiance[2].get();        // Mip 2
			uavs.at(3) = uavRadiance[3].get();        // Mip 3
			uavs.at(4) = uavRadiance[4].get();        // Mip 4

			context->CSSetShaderResources(0, 1, srvs.data());
			context->CSSetUnorderedAccessViews(0, 5, uavs.data(), nullptr);
			context->CSSetShader(prefilterRadianceCompute.get(), nullptr, 0);
			context->Dispatch((internalRes[0] + 15u) >> 4, (internalRes[1] + 15u) >> 4, 1);
		}

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// GI
	{
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - GI");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texRadiance->srv.get();
		srvs.at(3) = texNoise->srv.get();
		srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(5) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(6) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(7) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(8) = texGiSpecular[inputAoTexIdx]->srv.get();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();
		uavs.at(4) = texPrevGeo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAoTexIdx = inputAoTexIdx;
	}

	// blur
	if (settings.EnableBlur) {
		TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Diffuse Blur");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(3) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(4) = texIlCoCg[inputGITexIdx]->srv.get();

		uavs.at(0) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(blurCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// upsample
	if (resolutionMode != 0) {
		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texAo[inputAoTexIdx]->srv.get();
		srvs.at(2) = texIlY[inputGITexIdx]->srv.get();
		srvs.at(3) = texIlCoCg[inputGITexIdx]->srv.get();
		srvs.at(4) = texGiSpecular[inputAoTexIdx]->srv.get();

		uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
		uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
		uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
		uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(upsampleCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		inputAoTexIdx = !inputAoTexIdx;
		inputGITexIdx = !inputGITexIdx;
	}

	// full-res center refinement (half/quarter modes), then smooth blend into current output
	if (useCenterFullMask) {
		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Center FullRes GI");

			resetViews();
			srvs.at(0) = texWorkingDepth->srv.get();
			srvs.at(1) = rts[NORMALROUGHNESS].SRV;
			srvs.at(2) = texRadiance->srv.get();
			srvs.at(3) = texNoise->srv.get();
			srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
			srvs.at(5) = texAo[inputAoTexIdx]->srv.get();
			srvs.at(6) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(7) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(8) = texGiSpecular[inputAoTexIdx]->srv.get();
			srvs.at(9) = rts[deferred->forwardRenderTargets[0]].SRV;

			uavs.at(0) = texCenterAo->uav.get();
			uavs.at(1) = texCenterIlY->uav.get();
			uavs.at(2) = texCenterIlCoCg->uav.get();
			uavs.at(3) = texCenterGiSpecular->uav.get();
			uavs.at(4) = texPrevGeo->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(centerGIMaskedCompute.get(), nullptr, 0);
			context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);
		}

		{
			TracyD3D11Zone(globals::state->tracyCtx, "SSGI - Center Blend");

			resetViews();
			srvs.at(0) = texAo[inputAoTexIdx]->srv.get();
			srvs.at(1) = texIlY[inputGITexIdx]->srv.get();
			srvs.at(2) = texIlCoCg[inputGITexIdx]->srv.get();
			srvs.at(3) = texGiSpecular[inputAoTexIdx]->srv.get();
			srvs.at(4) = texCenterAo->srv.get();
			srvs.at(5) = texCenterIlY->srv.get();
			srvs.at(6) = texCenterIlCoCg->srv.get();
			srvs.at(7) = texCenterGiSpecular->srv.get();

			uavs.at(0) = texAo[!inputAoTexIdx]->uav.get();
			uavs.at(1) = texIlY[!inputGITexIdx]->uav.get();
			uavs.at(2) = texIlCoCg[!inputGITexIdx]->uav.get();
			uavs.at(3) = texGiSpecular[!inputAoTexIdx]->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(centerBlendCompute.get(), nullptr, 0);
			context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

			inputAoTexIdx = !inputAoTexIdx;
			inputGITexIdx = !inputGITexIdx;
		}
	}

	outputAoIdx = inputAoTexIdx;
	outputIlIdx = inputGITexIdx;

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}
