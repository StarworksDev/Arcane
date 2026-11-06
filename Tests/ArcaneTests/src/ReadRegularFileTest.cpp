#include <catch2/catch_test_macros.hpp>
#include <Arcane/IO/ReadRegularFile.hpp>
#include "Helpers/ConstantScan.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::filesystem::path Scratch(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "arcane-read-regular";
    std::filesystem::create_directories(dir);
    return dir / name;
}

TEST_CASE("ReadRegularFile refuses a directory, a non-regular path, and a file over the cap, and returns an empty vector for an empty file",
          "[io][read]") {
    const auto dir = Scratch("a-directory");
    std::filesystem::create_directory(dir);
    CHECK_FALSE(Arcane::ReadRegularFile(dir).has_value());

    const auto empty = Scratch("empty.bin");
    { std::ofstream out(empty, std::ios::binary); }
    const auto emptyBytes = Arcane::ReadRegularFile(empty);
    REQUIRE(emptyBytes.has_value());
    CHECK(emptyBytes->empty());

    const auto small = Scratch("sixteen.bin");
    {
        std::ofstream out(small, std::ios::binary);
        out << "0123456789abcdef";
    }
    CHECK_FALSE(Arcane::ReadRegularFile(small, 15).has_value());
    const auto exact = Arcane::ReadRegularFile(small, 16);
    REQUIRE(exact.has_value());
    CHECK(exact->size() == 16);

    CHECK_FALSE(Arcane::ReadRegularFile(Scratch("no-such-file")).has_value());

    // Pin: the body sizes with is_regular_file + file_size and never tellg.
    std::ifstream src(Arcane::Test::RepoRoot() / "Source/Runtime/ArcaneCore/src/Arcane/IO/ReadRegularFile.cpp");
    REQUIRE(src.good());
    std::ostringstream ss;
    ss << src.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("is_regular_file") != std::string::npos);
    CHECK(text.find("file_size") != std::string::npos);
    CHECK(text.find("tellg") == std::string::npos);
}

namespace {
// The source with /* */ blocks dropped, then each line stripped of literals and
// a trailing // comment. What is left is code.
std::string CodeOnly(const std::string& text) {
    std::string noBlocks;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const auto end = text.find("*/", i + 2);
            if (end == std::string::npos) break;
            for (std::size_t k = i; k < end + 2; ++k)
                if (text[k] == '\n') noBlocks.push_back('\n');
            i = end + 1;
            continue;
        }
        noBlocks.push_back(text[i]);
    }
    std::istringstream lines(noBlocks);
    std::string line, out;
    while (std::getline(lines, line)) {
        out += Arcane::Test::StripLiteralsAndComments(line);
        out.push_back('\n');
    }
    return out;
}
}

TEST_CASE("production sources do not size a whole file with tellg", "[io][read]") {
    const auto root = Arcane::Test::RepoRoot();
    const std::vector<std::string> allowed = {
        // tail read, guarded by is_regular_file in this same task
        "Source/Programs/Servitor/src/Servitor/Client.cpp",
    };
    std::vector<std::string> hits;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Source")) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        const std::string rel = std::filesystem::relative(entry.path(), root).generic_string();
        if (rel.find("/ThirdParty/") != std::string::npos) continue;
        if (std::find(allowed.begin(), allowed.end(), rel) != allowed.end()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        if (CodeOnly(ss.str()).find("tellg") != std::string::npos)
            hits.push_back(rel);
    }
    for (const auto& h : hits)
        INFO(h);
    CHECK(hits.empty());
    if (!hits.empty()) {
        std::string all;
        for (const auto& h : hits) all += "\n  " + h;
        FAIL("tellg sizes a whole file in:" << all);
    }
}
