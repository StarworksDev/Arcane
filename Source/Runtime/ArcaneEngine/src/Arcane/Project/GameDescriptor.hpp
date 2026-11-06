#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// GameDescriptor: the game module's `<sourceDir>/<Name>.arcmodule` (engine
// modularization s4). It names the engine modules the game uses; the SDK's
// build/arcane.lua links and includes that closure and nothing else. A game
// WITHOUT one gets the default set below -- so writing the default descriptor
// never changes what a project links, it only makes the set visible and
// editable. New projects (Project::Create) and the editor's Create -> C++ Class
// (on a project that has none yet) write it; an existing descriptor is never
// overwritten.

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace Arcane
{
    // Every Runtime + Client module (ArcaneClient's closure plus Net and
    // Crypto), the ImGui vendor module, ArcaneImGui and ArcanePhysics2D (the
    // 2D sim hook: Engine no longer links it, a game that simulates does). The build-side twin is
    // ARC_GAME_DEFAULT_MODULES in build/modules.lua; keep the two equal.
    inline constexpr std::array<std::string_view, 6> kDefaultGameModules{
        "ArcaneClient", "ArcaneImGui", "ImGui", "ArcaneNet", "ArcaneCrypto", "ArcanePhysics2D",
    };

    // "<moduleName>.arcmodule".
    [[nodiscard]] ARC_ENGINE_API std::string GameDescriptorFileName(std::string_view moduleName);

    // The default descriptor's JSON text for `moduleName` (type Game, the
    // default set as its dependencies), ending in a newline. `name` must equal
    // the file stem -- build/modules.lua refuses a mismatch.
    [[nodiscard]] ARC_ENGINE_API std::string RenderDefaultGameDescriptor(std::string_view moduleName);

    // Write the default descriptor into `sourceDir` unless a file of that name
    // already exists (an existing descriptor is left exactly as it is). True
    // when the descriptor exists afterwards (written now or already there).
    ARC_ENGINE_API bool WriteDefaultGameDescriptorIfAbsent(const std::filesystem::path& sourceDir,
                                                           std::string_view moduleName);
}
