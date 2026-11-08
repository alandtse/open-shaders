#pragma once

#include "CellRequest.h"
#include "SaveLocation.h"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace DX12ResourceService
{
    struct InteriorCellCandidate
    {
        CellRequest identity;
        std::string editorId, displayName;
    };
    struct SaveCellResolution
    {
        std::optional<CellRequest> cell;
        std::string explanation;
        uint32_t candidates = 0;
    };

    /** @brief Refuse duplicate/ambiguous names, always prioritize exact EditorIDs.
     * Neither a save filename nor a localized location label is a FormID.
     */
    inline SaveCellResolution ResolveSaveCell(
        const SaveLocationHint& hint, std::span<const InteriorCellCandidate> interiorCells)
    {
        const auto search = [&](std::string_view text, bool editorId) {
            SaveCellResolution found;
            if (text.empty()) return found;
            for (const auto& candidate : interiorCells) {
                const auto& candidateName = editorId ? candidate.editorId : candidate.displayName;
                if (!SameCellHint(candidateName, text)) continue;
                ++found.candidates;
                if (found.candidates == 1) found.cell = candidate.identity;
                else found.cell.reset();
            }
            return found;
        };
        const auto byEditor = search(hint.editorId, true);
        if (byEditor.candidates == 1) return { byEditor.cell, "exact save filename CELL EditorID", 1 };
        if (byEditor.candidates > 1) return { {}, "ambiguous save filename CELL EditorID", byEditor.candidates };
        const auto byName = search(hint.locationName, false);
        if (byName.candidates == 1) return { byName.cell, "unique save header location name", 1 };
        if (byName.candidates > 1) return { {}, "ambiguous save header location name", byName.candidates };
        return { {}, "save location does not resolve to a known closed interior", 0 };
    }
}
