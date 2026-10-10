// File -> Open Recent Scene: the pure half of SceneRecents. CPU-only,
// no filesystem ([editor]) -- Parse/Serialize/Push/Prune only. Unlike
// RecentProjectsTest, this file has no "must not clobber a document another
// process owns" contract to pin: the editor is the sole writer of
// recent_scenes.json, so the tests that matter are ordering, dedup, the cap,
// and tolerant reads of a malformed or future-versioned document.

#include <string>
#include <Arcane/Platform/Platform.hpp>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Project/SceneRecents.hpp"

using Arcane::Editor::SceneRecents::List;
namespace SceneRecents = Arcane::Editor::SceneRecents;

TEST_CASE("SceneRecents push dedups to front and caps at MaxEntries()", "[editor]")
{
    List list;
    for (int i = 0; i < 12; ++i)
        SceneRecents::Push(list, "D:/proj/scene" + std::to_string(i) + ".arcscene");

    // Capped, and the cap keeps the NEWEST entries.
    REQUIRE(list.paths.size() == SceneRecents::MaxEntries());
    CHECK(list.paths.front() == "D:/proj/scene11.arcscene");
    CHECK(list.paths.back() == "D:/proj/scene2.arcscene");

    // Re-pushing an existing entry moves it to the front rather than
    // duplicating it or growing the list past the cap.
    SceneRecents::Push(list, "D:/proj/scene5.arcscene");
    REQUIRE(list.paths.size() == SceneRecents::MaxEntries());
    CHECK(list.paths.front() == "D:/proj/scene5.arcscene");
    std::size_t count = 0;
    for (const std::string& p : list.paths)
        if (p == "D:/proj/scene5.arcscene")
            ++count;
    CHECK(count == 1);
}

TEST_CASE("SceneRecents push normalises the path", "[editor]")
{
    List list;
    // Backslashes and a redundant "./" segment must collapse to the same
    // generic-string key a second push of the "clean" spelling would produce
    // -- otherwise the same scene shows up as two menu rows.
#if ARC_PLATFORM_WINDOWS
    SceneRecents::Push(list, "D:\\proj\\.\\Scenes\\Level1.arcscene");
#else
    // POSIX: '\\' is a file-name byte, not a separator; the redundant
    // segments are what normalise ("./" and a doubled slash).
    SceneRecents::Push(list, "D:/proj/./Scenes//Level1.arcscene");
#endif
    REQUIRE(list.paths.size() == 1);
    CHECK(list.paths.front() == "D:/proj/Scenes/Level1.arcscene");

    SceneRecents::Push(list, "D:/proj/Scenes/Level1.arcscene");
    CHECK(list.paths.size() == 1);   // same key -- moved, not duplicated
}

TEST_CASE("SceneRecents push ignores an empty path", "[editor]")
{
    List list;
    SceneRecents::Push(list, "");
    CHECK(list.paths.empty());
}

TEST_CASE("SceneRecents serialize then parse round-trips order", "[editor]")
{
    List list;
    SceneRecents::Push(list, "D:/proj/a.arcscene");
    SceneRecents::Push(list, "D:/proj/b.arcscene");
    SceneRecents::Push(list, "D:/proj/c.arcscene");

    const std::string text = SceneRecents::Serialize(list);
    const List parsed = SceneRecents::Parse(text);

    REQUIRE(parsed.paths.size() == 3);
    CHECK(parsed.paths[0] == list.paths[0]);
    CHECK(parsed.paths[1] == list.paths[1]);
    CHECK(parsed.paths[2] == list.paths[2]);
}

TEST_CASE("SceneRecents parse is tolerant of bad or unknown documents", "[editor]")
{
    CHECK(SceneRecents::Parse("").paths.empty());
    CHECK(SceneRecents::Parse("junk").paths.empty());
    CHECK(SceneRecents::Parse(R"({"version":99,"scenes":[]})").paths.empty());
    CHECK(SceneRecents::Parse("[]").paths.empty());   // envelope required, not a bare array
    CHECK(SceneRecents::Parse(R"({"version":1})").paths.empty());   // missing "scenes"
}

TEST_CASE("SceneRecents parse skips non-string entries rather than failing the list", "[editor]")
{
    const std::string doc = R"({"version":1,"scenes":["D:/a.arcscene",42,null,"D:/b.arcscene"]})";
    const List parsed = SceneRecents::Parse(doc);

    REQUIRE(parsed.paths.size() == 2);
    CHECK(parsed.paths[0] == "D:/a.arcscene");
    CHECK(parsed.paths[1] == "D:/b.arcscene");
}

TEST_CASE("SceneRecents prune drops exactly the entries the predicate rejects", "[editor]")
{
    List list;
    list.paths = {"here", "gone", "also_here"};

    SceneRecents::Prune(list, [](const std::string& p) { return p != "gone"; });

    REQUIRE(list.paths.size() == 2);
    CHECK(list.paths[0] == "here");
    CHECK(list.paths[1] == "also_here");
}

TEST_CASE("SceneRecents prune on an all-missing list empties it", "[editor]")
{
    List list;
    list.paths = {"a", "b", "c"};

    SceneRecents::Prune(list, [](const std::string&) { return false; });

    CHECK(list.paths.empty());
}

TEST_CASE("SceneRecents::Replace keeps position, normalises and dedupes", "[editor][assetops]")
{
    List l; for (const char* p : { "D:/p/c.arcscene", "D:/p/b.arcscene", "D:/p/a.arcscene" }) SceneRecents::Push(l, p);
    SceneRecents::Replace(l, "D:/p/./b.arcscene", "D:/p/lv/b2.arcscene");
    CHECK(l.paths == std::vector<std::string>{ "D:/p/a.arcscene", "D:/p/lv/b2.arcscene", "D:/p/c.arcscene" });
    SceneRecents::Replace(l, "D:/p/c.arcscene", "D:/p/a.arcscene");
    SceneRecents::Replace(l, "D:/p/zzz.arcscene", "D:/p/q.arcscene");
    CHECK(l.paths == std::vector<std::string>{ "D:/p/a.arcscene", "D:/p/lv/b2.arcscene" });
}
