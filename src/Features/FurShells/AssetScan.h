#pragma once

#include <filesystem>

namespace FurShellAssets
{
	/**
	 * @brief Reports whether any `_shell.dds` can exist under the Data folder, as loose files or inside a .bsa.
	 * @param a_dataDir The game's Data folder.
	 * @return False only when every texture source was read and none holds a shell; true on any doubt, such as an archive without a file name table.
	 */
	bool AnyShellTextures(const std::filesystem::path& a_dataDir);
}
