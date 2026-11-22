#include "native_d3d11_hooks.h"
#include "reflection_observer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
mafiareflections::native::Observers callbacks{};

struct TestDirectory {
    const std::filesystem::path path;
    ~TestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}
} // namespace

namespace mafiareflections::native {
bool installVerifiedHooks(const Observers& observers) noexcept {
    callbacks = observers;
    return callbacks.beforeDraw && callbacks.beforeDispatch && callbacks.afterPresent && callbacks.beforeResize;
}
bool getShaderIdentity(ID3D11DeviceChild*, ShaderIdentity&) noexcept { return false; }
HookHealth hookHealth() noexcept {
    HookHealth health{};
    health.deviceFactoriesCalled = 1;
    health.gameDevicesTagged = 1;
    health.shadersTagged = 12;
    health.gameDrawsObserved = 8;
    health.gamePresentsObserved = 1;
    return health;
}
} // namespace mafiareflections::native

int main() {
    using namespace mafiareflections;
    if (reflectionDiagnosticsAvailable() || setReflectionDiagnosticsEnabled(false))
        return fail("The menu accepted changes before observer initialization.");

    const auto folder = std::filesystem::temp_directory_path()
        / (L"MafiaReflections-test-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(folder)) return fail("Could not make a new test folder.");
    const TestDirectory cleanup{folder};
    const auto settings = folder / L"MafiaReflections.ini";
    const auto log = folder / L"MafiaReflections.log";
    if (!WritePrivateProfileStringW(L"Diagnostics", L"Enabled", L"0", settings.c_str()))
        return fail("Could not create test settings.");
    if (!installReflectionObserver(folder.c_str()) || !reflectionDiagnosticsAvailable() || reflectionDiagnosticsEnabled())
        return fail("Expected an armed observer with saved recording disabled.");

    const auto initialLog = readFile(log);
    callbacks.beforeResize(nullptr);
    if (readFile(log) != initialLog) return fail("An observer callback logged while recording was off.");
    if (!setReflectionDiagnosticsEnabled(true) || !reflectionDiagnosticsEnabled()
        || GetPrivateProfileIntW(L"Diagnostics", L"Enabled", 0, settings.c_str()) != 1)
        return fail("The F8 On choice was not applied and saved.");
    callbacks.beforeResize(nullptr);
    if (readFile(log).find("ResizeBuffers attempted") == std::string::npos)
        return fail("The observer did not log after F8 enabled recording.");
    callbacks.afterPresent(nullptr, 0, 0, S_OK);
    const auto withHealth = readFile(log);
    if (withHealth.find("HOOK_HEALTH frame=1") == std::string::npos
        || withHealth.find("taggedShaders=12") == std::string::npos
        || withHealth.find("reflectionMatches=0") == std::string::npos)
        return fail("Hook health was not recorded or incorrectly claimed reflection matches.");
    if (!setReflectionDiagnosticsEnabled(false) || reflectionDiagnosticsEnabled()
        || GetPrivateProfileIntW(L"Diagnostics", L"Enabled", 1, settings.c_str()) != 0)
        return fail("The F8 Off choice was not applied and saved.");
    const auto finalLog = readFile(log);
    callbacks.beforeResize(nullptr);
    if (readFile(log) != finalLog) return fail("The F8 Off choice did not stop recording.");
    std::cout << "F8 reflection diagnostics settings persist and gate the observer callbacks.\n";
}
