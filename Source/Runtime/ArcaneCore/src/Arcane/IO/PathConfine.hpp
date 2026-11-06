#pragma once
#include <filesystem>
#include <system_error>

namespace Arcane {
// True when `candidate` is a regular file whose weakly_canonical path is
// strictly inside weakly_canonical(`directory`). `candidate` may be absolute:
// the asset directory is absolute, and `base / relative` is absolute too.
// False for empty, when canonicalization fails, when the candidate is not a
// regular file, and when the relative path from the canonical directory to
// the canonical candidate is empty or starts with "..". An empty `directory`
// means the current directory (a bare "mesh.gltf" has an empty parent_path).
[[nodiscard]] inline bool PathIsInsideDirectory(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate)
{
    namespace fs = std::filesystem;
    if (candidate.empty())
        return false;
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec) || ec)
        return false;
    const fs::path dir = fs::weakly_canonical(directory.empty() ? fs::path(".") : directory, ec);
    if (ec || dir.empty())
        return false;
    const fs::path file = fs::weakly_canonical(candidate, ec);
    if (ec || file.empty())
        return false;
    const fs::path rel = file.lexically_relative(dir);
    if (rel.empty() || rel == ".")
        return false;
    return *rel.begin() != "..";
}
}
