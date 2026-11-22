#pragma once

namespace mafiareflections {

bool installReflectionObserver(const wchar_t* gameFolder) noexcept;
bool reflectionDiagnosticsAvailable() noexcept;
bool reflectionDiagnosticsEnabled() noexcept;
bool setReflectionDiagnosticsEnabled(bool enabled) noexcept;

} // namespace mafiareflections
