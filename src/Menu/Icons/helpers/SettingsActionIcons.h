#pragma once

#include "IconsTabler.h"
#include "Menu/Icons/helpers/IconFonts.h"

/** @brief Shared glyphs for settings restoration and shader-cache actions. */
namespace SettingsActionIcons
{
	inline constexpr Icons::GlyphRef kRestoreDefaults = Icons::TI(ICON_TI_ERASER);
	inline constexpr Icons::GlyphRef kRestoreSaved = Icons::TI(ICON_TI_RESTORE);
	inline constexpr Icons::GlyphRef kReapplyOverride = Icons::TI(ICON_TI_REFRESH);
	inline constexpr Icons::GlyphRef kClearCache = Icons::TI(ICON_TI_DATABASE_X);
	inline constexpr Icons::GlyphRef kClearActiveShaders = Icons::TI(ICON_TI_DATABASE_MINUS);
}
