#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the exe directory off-Windows

#include <Arcane/Base/Log.hpp>

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Mirrors the Assets/ShaderPaths anchor: relative paths resolve against
        // the executable directory so lookups work regardless of CWD.
        std::filesystem::path ExeRelative(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#else
            if (const std::string self = ExecutablePathUtf8(); !self.empty())
                return std::filesystem::path(self).parent_path() / path;
#endif
            return path;
        }
    }

    void ShaderSourceProvider::AddRoot(const std::filesystem::path& root)
    {
        m_roots.push_back(root);
    }

    std::optional<std::string> ShaderSourceProvider::Get(std::string_view logicalName) const
    {
        const std::filesystem::path rel(logicalName);
        // has_root_name/root_directory (not just is_absolute): Windows
        // drive-relative "C:foo" and root-relative "\foo" both resolve OUTSIDE
        // the roots yet are not "absolute" paths.
        if (rel.empty() || rel.has_root_name() || rel.has_root_directory())
            return std::nullopt;
        for (const auto& part : rel)
        {
            if (part == "..")
            {
                ARC_WARN("ShaderSourceProvider: rejected escaping name '{}'",
                         std::string(logicalName));
                return std::nullopt;
            }
        }

        for (const std::filesystem::path& root : m_roots)
        {
            std::ifstream file(ExeRelative(root) / rel, std::ios::binary);
            if (!file)
                continue;
            std::string text{ std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>() };
            return text;
        }
        return std::nullopt;
    }
}
