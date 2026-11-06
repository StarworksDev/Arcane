#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Platform/Process.hpp>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

// Rev 8.3 V5: servitor links ArcaneCore, ArcaneNet and ArcaneRemoteProtocol only.
// The staged closure beside servitor (Binaries/<cfg>/Servitor/, the sibling of
// this exe's folder) is the proof: no engine-side module may appear there.
TEST_CASE("servitor's staged closure holds no engine-side module", "[servitor][boundary]") {
    const auto dir = Arcane::Platform::ExecutablePath().parent_path().parent_path() / "Servitor";
    REQUIRE(std::filesystem::is_directory(dir));
#if defined(_WIN32)
    REQUIRE(std::filesystem::exists(dir / "servitor.exe"));
#else
    REQUIRE(std::filesystem::exists(dir / "servitor"));
#endif
    constexpr std::array<std::string_view, 10> kForbidden = {
        "ArcaneRemote.", "ArcaneEngine", "ArcaneScene", "ArcaneSim", "ArcaneAssets",
        "ArcaneCrypto", "ArcaneClient", "ArcaneRender", "ArcaneEditor", "ArcaneVerify"};
    std::size_t coreFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        if (name.starts_with("lib"))
            name.erase(0, 3);                       // POSIX shared-library prefix
        INFO(name);
        for (std::string_view bad : kForbidden)
            CHECK_FALSE(name.starts_with(bad));
        if (name.starts_with("ArcaneCore"))
            ++coreFiles;
    }
    CHECK(coreFiles > 0);                           // the closure was staged at all
}

#endif // !defined(ARC_BUILD_DIST)
