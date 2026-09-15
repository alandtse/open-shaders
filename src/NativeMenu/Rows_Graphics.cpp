#include "NativeMenu/NativeMenu.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"

#define I18N_KEY_PREFIX "native_menu.graphics."

namespace
{
	void __stdcall OpenCommunityShadersMenu()
	{
		if (globals::menu)
			globals::menu->IsEnabled = true;
	}
}

namespace NativeMenu
{
	std::vector<Row> GraphicsRows()
	{
		std::vector<Row> rows{
			NATIVE_MENU_HEADING(T(TKEY("heading"), "Open Shaders")),

			Button(T(TKEY("open_menu"), "Open Menu"), &OpenCommunityShadersMenu,
				T(TKEY("open_menu_desc"), "Opens the full Open Shaders settings window.")),
		};

		AppendRows(rows, HDRRows());
		AppendRows(rows, SSGIRows());

		return rows;
	}
}

#undef I18N_KEY_PREFIX
