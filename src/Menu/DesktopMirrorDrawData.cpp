#include "DesktopMirrorDrawData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	bool IsFinitePositive(float value)
	{
		return std::isfinite(value) && value > 0.0f;
	}

	bool IsFinite(const ImVec2& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y);
	}
}

bool DesktopMirrorDrawData::ContainsCallback(const ImDrawData& source, const ImDrawCallback callback)
{
	if (!callback)
		return false;
	for (int listIndex = 0; listIndex < source.CmdLists.Size; ++listIndex) {
		const auto* drawList = source.CmdLists[listIndex];
		if (!drawList)
			continue;
		for (int commandIndex = 0; commandIndex < drawList->CmdBuffer.Size; ++commandIndex) {
			if (drawList->CmdBuffer[commandIndex].UserCallback == callback)
				return true;
		}
	}
	return false;
}

bool DesktopMirrorDrawData::FitTransform::MapPosition(const ImVec2 position, const ImVec2 displayPos, const ImVec2 framebufferScale, ImVec2& result) const
{
	const double x = offsetX + (static_cast<double>(position.x) - displayPos.x) * framebufferScale.x * scale;
	const double y = offsetY + (static_cast<double>(position.y) - displayPos.y) * framebufferScale.y * scale;
	if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<float>::max() || std::abs(y) > std::numeric_limits<float>::max())
		return false;
	result = ImVec2(static_cast<float>(x), static_cast<float>(y));
	return true;
}

bool DesktopMirrorDrawData::GetFitTransform(const ImDrawData& source, const ImVec2 targetSize, FitTransform& transform)
{
	if (!source.Valid || !IsFinitePositive(source.DisplaySize.x) || !IsFinitePositive(source.DisplaySize.y) ||
		!IsFinite(source.DisplayPos) || !IsFinitePositive(source.FramebufferScale.x) || !IsFinitePositive(source.FramebufferScale.y) ||
		!IsFinitePositive(targetSize.x) || !IsFinitePositive(targetSize.y)) {
		return false;
	}

	const double sourceWidth = static_cast<double>(source.DisplaySize.x) * source.FramebufferScale.x;
	const double sourceHeight = static_cast<double>(source.DisplaySize.y) * source.FramebufferScale.y;
	if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) || sourceWidth <= 0.0 || sourceHeight <= 0.0)
		return false;

	const double scale = (std::min)(static_cast<double>(targetSize.x) / sourceWidth, static_cast<double>(targetSize.y) / sourceHeight);
	const double offsetX = (static_cast<double>(targetSize.x) - sourceWidth * scale) * 0.5;
	const double offsetY = (static_cast<double>(targetSize.y) - sourceHeight * scale) * 0.5;
	if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(offsetX) || !std::isfinite(offsetY))
		return false;

	transform = { scale, offsetX, offsetY };
	return true;
}

bool DesktopMirrorDrawData::Build(const ImDrawData& source, const ImVec2 targetSize)
{
	Reset();
	const auto fail = [this]() {
		Reset();
		return false;
	};
	FitTransform fit{};
	if (source.CmdLists.Size <= 0 || !GetFitTransform(source, targetSize, fit)) {
		return false;
	}

	try {
		m_clonedLists.reserve(static_cast<size_t>(source.CmdLists.Size));
		for (int listIndex = 0; listIndex < source.CmdLists.Size; ++listIndex) {
			const auto* sourceList = source.CmdLists[listIndex];
			if (!sourceList)
				return fail();

			std::unique_ptr<ImDrawList, DrawListDeleter> clone(sourceList->CloneOutput());
			if (!clone)
				return fail();

			for (int vertexIndex = 0; vertexIndex < clone->VtxBuffer.Size; ++vertexIndex) {
				auto& position = clone->VtxBuffer[vertexIndex].pos;
				if (!fit.MapPosition(position, source.DisplayPos, source.FramebufferScale, position)) {
					return fail();
				}
			}

			for (int commandIndex = 0; commandIndex < clone->CmdBuffer.Size; ++commandIndex) {
				auto& clipRect = clone->CmdBuffer[commandIndex].ClipRect;
				ImVec2 min{}, max{};
				if (!fit.MapPosition(ImVec2(clipRect.x, clipRect.y), source.DisplayPos, source.FramebufferScale, min) ||
					!fit.MapPosition(ImVec2(clipRect.z, clipRect.w), source.DisplayPos, source.FramebufferScale, max)) {
					return fail();
				}
				clipRect.x = std::clamp(min.x, 0.0f, targetSize.x);
				clipRect.y = std::clamp(min.y, 0.0f, targetSize.y);
				clipRect.z = std::clamp(max.x, 0.0f, targetSize.x);
				clipRect.w = std::clamp(max.y, 0.0f, targetSize.y);
			}

			m_clonedLists.push_back(std::move(clone));
		}

		if (m_clonedLists.empty())
			return fail();

		m_drawData = source;
		m_drawData.CmdLists.clear();
		m_drawData.CmdLists.reserve(static_cast<int>(m_clonedLists.size()));
		m_drawData.TotalIdxCount = 0;
		m_drawData.TotalVtxCount = 0;
		for (const auto& list : m_clonedLists) {
			m_drawData.CmdLists.push_back(list.get());
			m_drawData.TotalIdxCount += list->IdxBuffer.Size;
			m_drawData.TotalVtxCount += list->VtxBuffer.Size;
		}
		m_drawData.CmdListsCount = m_drawData.CmdLists.Size;
		m_drawData.DisplayPos = ImVec2(0.0f, 0.0f);
		m_drawData.DisplaySize = targetSize;
		m_drawData.FramebufferScale = ImVec2(1.0f, 1.0f);
		m_built = true;
		return true;
	} catch (...) {
		Reset();
		return false;
	}
}

void DesktopMirrorDrawData::Reset()
{
	m_built = false;
	m_drawData.CmdLists.clear();
	m_drawData.CmdListsCount = 0;
	m_drawData.TotalIdxCount = 0;
	m_drawData.TotalVtxCount = 0;
	m_clonedLists.clear();
}
