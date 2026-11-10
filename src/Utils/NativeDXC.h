#pragma once

#include <vector>

namespace NativeDXC
{
    /** @brief Loads DXIL or compiles a native shader at runtime according to the selected mode. */
    std::vector<char> Load(const char* packagedPath, const char* sourcePath, bool reverseZ = false);
}
