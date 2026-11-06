#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/Log.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    int g_hits = 0;
    void OnLog(const Arcane::LogRecord& rec, void*)
    {
        if (rec.text.find("remote-ring-line") != std::string::npos)
            ++g_hits;
    }
}

TEST_CASE("Subscribe observes a line and ReadSince resumes from seq", "[remote]")
{
    g_hits = 0;
    // Anchor on the newest seq first (ruling F3): a full-suite run leaves
    // thousands of earlier lines in the ring, and ReadSince(0, n) returns the
    // OLDEST n of them.
    const std::uint64_t base = Arcane::Log::LatestSeq();
    const auto sub = Arcane::Log::Subscribe(&OnLog, nullptr);
    Arcane::Log::Engine()->info("remote-ring-line one");
    Arcane::Log::Engine()->info("remote-ring-line two");
    CHECK(g_hits == 2);
    // Another thread may log in between, so find our lines by text, never by
    // a fixed offset from base.
    const auto findText = [](const std::vector<Arcane::LogRecord>& recs, const char* text) {
        for (const auto& r : recs)
            if (r.text.find(text) != std::string::npos)
                return r.seq;
        return std::uint64_t{0};
    };
    const auto mine = Arcane::Log::ReadSince(base, 64);
    const std::uint64_t one = findText(mine, "remote-ring-line one");
    const std::uint64_t two = findText(mine, "remote-ring-line two");
    REQUIRE(one > base);
    REQUIRE(two > one);
    CHECK(Arcane::Log::LatestSeq() >= two);
    const auto tail = Arcane::Log::ReadSince(one, 64);
    REQUIRE_FALSE(tail.empty());
    CHECK(tail.front().seq == one + 1);
    Arcane::Log::Unsubscribe(sub);
    const int frozen = g_hits;
    Arcane::Log::Engine()->info("remote-ring-line three");
    CHECK(g_hits == frozen);
    const auto after = Arcane::Log::ReadSince(two, 64);
    CHECK(findText(after, "remote-ring-line three") > two);
}
#endif
