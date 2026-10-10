#include "ProjectLayout.hpp"

#include <Arcane/Platform/Platform.hpp>   // NativeModuleFileName: the slot's per-platform spelling

namespace arcbuild
{
    std::filesystem::path SlotPath(
        const ProjectLayout& project)
    {
        if (project.gameModule.empty())
            return {};

        // The manifest's authored name in this platform's spelling: the slot a
        // Linux build fills is Binaries/<Stem>.so (identity on Windows).
        return project.root / "Binaries" / Arcane::Platform::NativeModuleFileName(project.gameModule);
    }

    std::filesystem::path SolutionPath(
        const ProjectLayout& project,
        const std::filesystem::path& discovered)
    {
        if (!discovered.empty())
        {
            if (discovered.is_absolute())
                return discovered;

            return (project.root / discovered).lexically_normal();
        }

        return project.root / (project.name + ".slnx");
    }

    std::vector<std::filesystem::path> CleanTargets(
        const ProjectLayout& project,
        std::string_view config)
    {
        return
        {
            project.root / "Binaries",
            project.root / "Intermediate" / std::string(config)
        };
    }

    std::filesystem::path NinjaLinkOutput(
        const ProjectLayout& project,
        std::string_view config)
    {
        if (project.gameModule.empty())
            return {};

        return project.root / "Intermediate" / std::string(config) /
               "Ninja" / "Binaries" / Arcane::Platform::NativeModuleFileName(project.gameModule);
    }
}
