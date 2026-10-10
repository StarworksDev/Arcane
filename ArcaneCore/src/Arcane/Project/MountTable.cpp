#include <Arcane/Project/MountTable.hpp>

namespace Arcane
{
    void MountTable::Mount(std::string scheme, std::filesystem::path root)
    {
        m_roots[std::move(scheme)] = std::move(root);
    }

    bool MountTable::HasMount(std::string_view scheme) const
    {
        return m_roots.find(std::string(scheme)) != m_roots.end();
    }

    std::optional<std::filesystem::path> MountTable::Resolve(std::string_view mountPath) const
    {
        const std::size_t sep = mountPath.find("://");
        if (sep == std::string_view::npos)
            return std::nullopt;

        const std::string_view scheme = mountPath.substr(0, sep);
        const std::string_view rel    = mountPath.substr(sep + 3);
        if (scheme.empty() || rel.empty())
            return std::nullopt;

        const auto it = m_roots.find(std::string(scheme));
        if (it == m_roots.end())
            return std::nullopt;

        // std::filesystem::operator/ REPLACES the left side entirely when the right
        // side is absolute/rooted (drive letter, embedded "scheme://", or a leading
        // slash), so a naively-joined `rel` can escape `root`. Reject anything rooted
        // up front, then reject any ".."-climb that escapes the mount root once
        // joined and lexically normalized.
        // An embedded scheme ("game://a://b") is malformed on EVERY platform.
        // Windows rejects it below by accident of parsing ("a:" is a root
        // name); POSIX would read "a:" as an ordinary directory and accept
        // it, so the same mount path would be valid on one OS and not the
        // other. Say it explicitly.
        if (rel.find("://") != std::string_view::npos)
            return std::nullopt;

        const std::filesystem::path relPath(rel);
        if (relPath.has_root_name() || relPath.has_root_directory())
            return std::nullopt;

        const std::filesystem::path root     = it->second.lexically_normal();
        const std::filesystem::path resolved = (it->second / relPath).lexically_normal();

        const std::filesystem::path relToRoot = resolved.lexically_relative(root);
        if (relToRoot.empty() || relToRoot.begin()->string() == "..")
            return std::nullopt;

        return resolved;
    }
}
