#include "Features/Wind/Wind.h"

#include "Globals.h"
#include "Menu.h"
#include "Utils/Game.h"
#include "Utils/Region.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace
{
	constexpr unsigned kRingSegments = 32;
	constexpr unsigned kSpokeStride = 4;
	constexpr unsigned kWakeSections = 12;
	constexpr float kLineWidth = 1.5f;
	constexpr float kMinimumClipW = 1e-4f;
	constexpr ImU32 kWakeColor = IM_COL32(64, 220, 255, 230);
	constexpr std::array<ImU32, 6> kShapeColors{
		IM_COL32(255, 190, 60, 230), IM_COL32(120, 220, 120, 230),
		IM_COL32(255, 140, 80, 230), IM_COL32(240, 240, 240, 230),
		IM_COL32(210, 140, 255, 230), kWakeColor
	};

	struct ShapeRenderer
	{
		DirectX::SimpleMath::Matrix viewProjection;
		float3 eye;
		const ImGuiViewport* viewport;
		ImDrawList* drawList;

		void Line(float3 a_start, float3 a_end, ImU32 a_color) const
		{
			a_start -= eye;
			a_end -= eye;
			const auto startClip = float4::Transform(float4(a_start.x, a_start.y, a_start.z, 1.0f), viewProjection);
			const auto endClip = float4::Transform(float4(a_end.x, a_end.y, a_end.z, 1.0f), viewProjection);
			const std::array startPlanes{ startClip.w + startClip.x, startClip.w - startClip.x,
				startClip.w + startClip.y, startClip.w - startClip.y, startClip.z,
				startClip.w - startClip.z, startClip.w - kMinimumClipW };
			const std::array endPlanes{ endClip.w + endClip.x, endClip.w - endClip.x,
				endClip.w + endClip.y, endClip.w - endClip.y, endClip.z,
				endClip.w - endClip.z, endClip.w - kMinimumClipW };
			float first = 0.0f, last = 1.0f;
			for (std::size_t i = 0; i < startPlanes.size(); ++i) {
				const float start = startPlanes[i], end = endPlanes[i];
				if (!std::isfinite(start) || !std::isfinite(end) || (start < 0.0f && end < 0.0f))
					return;
				if (start < 0.0f)
					first = std::max(first, start / (start - end));
				else if (end < 0.0f)
					last = std::min(last, start / (start - end));
			}
			if (first > last)
				return;
			const auto delta = a_end - a_start;
			float2 startScreen, endScreen;
			if (!Util::Region::ProjectToScreen(viewProjection, a_start + delta * first, startScreen) ||
				!Util::Region::ProjectToScreen(viewProjection, a_start + delta * last, endScreen))
				return;
			drawList->AddLine({ viewport->Pos.x + startScreen.x * viewport->Size.x,
								  viewport->Pos.y + startScreen.y * viewport->Size.y },
				{ viewport->Pos.x + endScreen.x * viewport->Size.x,
					viewport->Pos.y + endScreen.y * viewport->Size.y },
				a_color, kLineWidth);
		}

		void Ring(const float3& a_center, const float3& a_right, const float3& a_up, ImU32 a_color) const
		{
			float3 previous = a_center + a_right;
			for (unsigned i = 1; i <= kRingSegments; ++i) {
				const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kRingSegments;
				const float3 point = a_center + a_right * std::cos(angle) + a_up * std::sin(angle);
				Line(previous, point, a_color);
				previous = point;
			}
		}

		void Sphere(const float3& a_center, float a_radius, ImU32 a_color) const
		{
			if (a_radius <= 0.0f)
				return;
			const float3 x{ a_radius, 0, 0 }, y{ 0, a_radius, 0 }, z{ 0, 0, a_radius };
			Ring(a_center, x, y, a_color);
			Ring(a_center, x, z, a_color);
			Ring(a_center, y, z, a_color);
		}

		void Shape(const OpenWind::WindField::TransientWindSource& a_source) const
		{
			using Type = OpenWind::WindField::TransientWindSourceType;
			const auto index = static_cast<std::size_t>(a_source.type);
			if (a_source.strength <= 0.0f || a_source.maxDistance <= 0.0f || index >= kShapeColors.size())
				return;
			const auto color = kShapeColors[index];
			if (a_source.type == Type::AttachedWedge) {
				float3 previousCenter, previousRight, previousUp;
				OpenWind::WindField::GetAttachedWedgeSection(a_source, 0.0f, previousCenter, previousRight, previousUp);
				Ring(previousCenter, previousRight, previousUp, color);
				for (unsigned section = 1; section <= kWakeSections; ++section) {
					float3 center, right, up;
					OpenWind::WindField::GetAttachedWedgeSection(a_source, static_cast<float>(section) / kWakeSections, center, right, up);
					Ring(center, right, up, color);
					for (unsigned i = 0; i < kRingSegments; i += kSpokeStride) {
						const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kRingSegments;
						Line(previousCenter + previousRight * std::cos(angle) + previousUp * std::sin(angle),
							center + right * std::cos(angle) + up * std::sin(angle), color);
					}
					previousCenter = center;
					previousRight = right;
					previousUp = up;
				}
				return;
			}
			const bool wave = a_source.type == Type::DirectionalWave || a_source.type == Type::RadialWave ||
			                  a_source.type == Type::DirectionalOvalWave;
			const float front = std::max(a_source.wavefrontDistance, 0.0f);
			const float trailing = std::abs(a_source.waveHalfWidth) + std::max(a_source.propagationSpeed, 0.0f) *
			                                                              std::clamp(a_source.decayTime, 0.0f, OpenWind::WindField::kTransientImpulseMaximumDecayTime);
			float first = wave ? std::max(front - trailing, 0.0f) : 0.0f;
			float last = wave ? std::min(front + std::abs(a_source.waveHalfWidth), a_source.maxDistance) : a_source.maxDistance;
			if (first >= last)
				return;
			if (a_source.type == Type::RadialWave ||
				(a_source.type == Type::DirectionalWave && a_source.direction.LengthSquared() <= 1e-6f)) {
				Sphere(a_source.origin, first, color);
				Sphere(a_source.origin, last, color);
				if (front > first && front < last)
					Sphere(a_source.origin, front, color);
				return;
			}
			if (a_source.direction.LengthSquared() <= 1e-6f)
				return;
			float3 forward = a_source.direction;
			forward.Normalize();
			const float horizontalLength = std::hypot(forward.x, forward.y);
			const float3 right = horizontalLength > kMinimumClipW ?
			                         float3{ forward.y, -forward.x, 0.0f } / horizontalLength :
			                         float3{ 1, 0, 0 };
			const float3 up = forward.Cross(right);
			const bool oval = a_source.type == Type::DirectionalOvalWave;
			const bool flow = a_source.type == Type::OrientedFlow;
			const float verticalScale = oval ? std::max(a_source.verticalScale, kMinimumClipW) : 1.0f;
			if (flow) {
				first = -std::max(a_source.propagationSpeed, kMinimumClipW);
				last = std::max(a_source.waveHalfWidth, kMinimumClipW);
			}
			const auto radius = [&](float distance) {
				if (flow)
					return a_source.maxDistance;
				const float base = std::abs(a_source.collisionRadius);
				const float core = base + distance * std::max(a_source.coneSpreadTangent, 0.0f);
				if (oval)
					return std::max(core + std::max(a_source.falloffWidth, 0.0f) *
											   (0.25f + 0.75f * distance / a_source.maxDistance),
						base + distance * std::max(a_source.sideSpreadTangent, 0.0f));
				return a_source.type == Type::DirectionalWave && a_source.sideStrength > 0.0f ?
				           std::max(core, base + distance * std::max(a_source.sideSpreadTangent, 0.0f)) :
				           core;
			};
			const auto firstCenter = a_source.origin + forward * first;
			const auto lastCenter = a_source.origin + forward * last;
			const float firstRadius = radius(first), lastRadius = radius(last);
			Ring(firstCenter, right * firstRadius, up * (firstRadius * verticalScale), color);
			Ring(lastCenter, right * lastRadius, up * (lastRadius * verticalScale), color);
			if (wave && front > first && front < last)
				Ring(a_source.origin + forward * front, right * radius(front), up * (radius(front) * verticalScale), color);
			for (unsigned i = 0; i < kRingSegments; i += kSpokeStride) {
				const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kRingSegments;
				const float3 rim = right * std::cos(angle) + up * (std::sin(angle) * verticalScale);
				Line(firstCenter + rim * firstRadius, lastCenter + rim * lastRadius, color);
			}
		}
	};
}

bool Wind::IsOverlayVisible() const
{
	return provider && globals::menu && globals::menu->overlayVisible && uiState.showTransientShapes &&
	       !globals::game::isVR;
}

void Wind::DrawOverlay()
{
	if (!IsOverlayVisible())
		return;
	const auto& sources = providerFrame->transients;
	const auto count = std::min(providerFrame->transientCount, static_cast<uint32_t>(sources.size()));
	const auto* viewport = ImGui::GetMainViewport();
	auto* drawList = ImGui::GetForegroundDrawList();
	if (globals::game::shadowState && RE::Main::WorldRootCamera()) {
		const auto eye = Util::GetEyePosition(0);
		const ShapeRenderer renderer{ Util::GetCameraData(0).viewProjMat, { eye.x, eye.y, eye.z }, viewport, drawList };
		for (uint32_t i = 0; i < count; ++i)
			renderer.Shape(sources[i]);
	}
	const auto label = std::format("Wind Shapes: {} active transients | Cyan: attached wakes", count);
	const auto& padding = ImGui::GetStyle().WindowPadding;
	ImVec2 labelPosition{ viewport->Pos.x + padding.x, viewport->Pos.y + padding.y };
	drawList->AddText(labelPosition, kWakeColor, label.c_str());
}
