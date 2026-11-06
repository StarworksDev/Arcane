#pragma once
#include <Arcane/Core/Api.hpp>
#include <Arcane/Core/Constant.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace Arcane {
// A cooked mesh or a shader blob in this tree is far under this cap.
ARC_CONSTANT("file format: the whole-file read cap; a cooked mesh or shader blob is far under it")
inline constexpr std::uint64_t kMaxRegularFileBytes = 1ull << 30;

// Reads a whole regular file. nullopt when the path is missing, not a regular
// file, its size is unavailable or over maxBytes, the open fails, or the
// allocation throws. An empty regular file returns an empty vector. Never throws.
[[nodiscard]] ARC_CORE_API std::optional<std::vector<std::byte>>
ReadRegularFile(const std::filesystem::path& path,
                std::uint64_t maxBytes = kMaxRegularFileBytes);
}
