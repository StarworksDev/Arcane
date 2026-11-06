#include <Arcane/IO/ReadRegularFile.hpp>
#include <exception>
#include <fstream>
#include <system_error>

namespace Arcane {
std::optional<std::vector<std::byte>>
ReadRegularFile(const std::filesystem::path& path, std::uint64_t maxBytes) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return std::nullopt;
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz > maxBytes)
        return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::vector<std::byte> out;
    try {
        out.resize(static_cast<std::size_t>(sz));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!out.empty() &&
        !in.read(reinterpret_cast<char*>(out.data()),
                 static_cast<std::streamsize>(out.size())))
        return std::nullopt;
    return out;
}
}
