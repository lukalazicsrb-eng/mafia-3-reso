#pragma once

#include <windows.h>

namespace mafiareflections {

// The loader must hash-verify the original 2.5.1 module and keep it loaded.
bool installReflectionMenu(HMODULE verifiedMod) noexcept;

}  // namespace mafiareflections
