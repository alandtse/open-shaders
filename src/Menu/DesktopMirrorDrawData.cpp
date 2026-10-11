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

bool DesktopMirrorDrawData::Build(const ImDrawData& source, const ImVec2 targetSize)
{
	Reset();
	const auto fail = [this]() {
		Reset();
		return false;
	};
	if (!source.Valid || source.CmdLists.Size <= 0 || !IsFinitePositive(source.DisplaySize.x) || !IsFinitePositive(source.DisplaySize.y) ||
		!IsFinite(source.DisplayPos) || !IsFinitePositive(source.FramebufferScale.x) || !IsFinitePositive(source.FramebufferScale.y) ||
		!IsFinitePositive(targetSize.x) || !IsFinitePositive(targetSize.y)) {
		return false;
	}

	const double sourceWidth = static_cast<double>(source.DisplaySize.x) * source.FramebufferScale.x;
	const double sourceHeight = static_cast<double>(source.DisplaySize.y) * source.FramebufferScale.y;
	if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight) || sourceWidth <= 0.0 || sourceHeight <= 0.0)
		return false;

	const double scale = (std::min)(static_cast<double>(targetSize.x) / sourceWidth, static_cast<double>(targetSize.y) / sourceHeight);
	if (!std::isfinite(scale) || scale <= 0.0)
		return false;

	const double offsetX = (static_cast<double>(targetSize.x) - sourceWidth * scale) * 0.5;
	const double offsetY = (static_cast<double>(targetSize.y) - sourceHeight * scale) * 0.5;
	const auto transform = [&](float value, float displayPosition, float framebufferScale, double offset, float& result) {
		const double transformed = offset + (static_cast<double>(value) - displayPosition) * framebufferScale * scale;
		if (!std::isfinite(transformed) || std::abs(transformed) > std::numeric_limits<float>::max())
			return false;
		result = static_cast<float>(transformed);
		return true;
	};

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
				if (!transform(position.x, source.DisplayPos.x, source.FramebufferScale.x, offsetX, position.x) ||
					!transform(position.y, source.DisplayPos.y, source.FramebufferScale.y, offsetY, position.y)) {
					return fail();
				}
			}

			for (int commandIndex = 0; commandIndex < clone->CmdBuffer.Size; ++commandIndex) {
				auto& clipRect = clone->CmdBuffer[commandIndex].ClipRect;
				if (!transform(clipRect.x, source.DisplayPos.x, source.FramebufferScale.x, offsetX, clipRect.x) ||
					!transform(clipRect.y, source.DisplayPos.y, source.FramebufferScale.y, offsetY, clipRect.y) ||
					!transform(clipRect.z, source.DisplayPos.x, source.FramebufferScale.x, offsetX, clipRect.z) ||
					!transform(clipRect.w, source.DisplayPos.y, source.FramebufferScale.y, offsetY, clipRect.w)) {
					return fail();
				}
				clipRect.x = std::clamp(clipRect.x, 0.0f, targetSize.x);
				clipRect.y = std::clamp(clipRect.y, 0.0f, targetSize.y);
				clipRect.z = std::clamp(clipRect.z, 0.0f, targetSize.x);
				clipRect.w = std::clamp(clipRect.w, 0.0f, targetSize.y);
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
