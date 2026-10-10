#pragma once

// The fixture modules' file names, spelled per platform (Linux port,
// 2026-10-05). The build emits every test plugin as <Stem> + the platform's
// module extension (premake5.lua's arcane_module: HotReloadPluginV1.dll on
// Windows, HotReloadPluginV1.so on Linux), so a test names a fixture by its
// STEM and these two helpers supply the rest. On Windows they return exactly
// the literals the tests used before ("HotReloadPluginV1.dll",
// "../HotReloadPluginV1/HotReloadPluginV1.dll").

#include <Arcane/Platform/Platform.hpp>

#include <string>
#include <string_view>

namespace Arcane::Test
{
    // The staged copy beside the test exe: "HotReloadPluginV1.dll" / ".so".
    inline std::string ModuleFile(std::string_view stem)
    {
        return Arcane::Platform::ModuleFileName(stem);
    }

    // The freshly built original in its own bin dir, relative to the test exe:
    // "../HotReloadPluginV1/HotReloadPluginV1.dll" / ".so".
    inline std::string BuiltModule(std::string_view stem)
    {
        std::string out = "../";
        out += stem;
        out += '/';
        out += Arcane::Platform::ModuleFileName(stem);
        return out;
    }
}
