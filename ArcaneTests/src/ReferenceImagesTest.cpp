// Reference resolution and blessing. Pure filesystem logic, no GPU, no host.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Project/ProjectPaths.hpp>   // kDistBuild

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    fs::path TempProject(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_reference_images_test" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    void WriteSolidPng(const fs::path& p, unsigned char value)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::vector<unsigned char> px(4 * 4 * 4, value);
        for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        REQUIRE(Arcane::WritePngRgba(p, 4, 4, px.data()));
    }
}

TEST_CASE("reference: nothing on disk resolves to None but still names a bless target", "[reference]")
{
    const auto root = TempProject("empty");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.path.empty());
    // A first bless must know where to go, so blessTarget is ALWAYS set --
    // the shared level when nothing resolved.
    CHECK(res.blessTarget == root / "Verify" / "References" / "runtime-scene.png");
}

TEST_CASE("reference: a shared image resolves at the Shared level", "[reference]")
{
    const auto root = TempProject("shared");
    WriteSolidPng(root / "Verify" / "References" / "runtime-scene.png", 10);

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(res.level == Arcane::ReferenceLevel::Shared);
    CHECK(res.path == root / "Verify" / "References" / "runtime-scene.png");
    CHECK(res.blessTarget == res.path);       // bless writes back where it came from
}

TEST_CASE("reference: a backend override WINS over the shared image", "[reference]")
{
    const auto root = TempProject("override");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "vulkan" / "editor-ui.png", 20);

    const auto dx12 = Arcane::ResolveReference(root, "editor-ui", "dx12");
    CHECK(dx12.level == Arcane::ReferenceLevel::Shared);

    const auto vk = Arcane::ResolveReference(root, "editor-ui", "vulkan");
    CHECK(vk.level == Arcane::ReferenceLevel::Backend);
    CHECK(vk.path == root / "Verify" / "References" / "vulkan" / "editor-ui.png");
    CHECK(vk.blessTarget == vk.path);
}

TEST_CASE("reference: blessing a fresh name creates the SHARED image", "[reference]")
{
    const auto root = TempProject("bless-new");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);

    std::vector<unsigned char> px(4 * 4 * 4, 77);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    CHECK(fs::exists(root / "Verify" / "References" / "runtime-scene.png"));

    // Hole B: existence alone passes a bless that wrote nothing or wrote a
    // stale/blank image. Load the file back and check the content it was
    // actually blessed with.
    Arcane::PixelData blessed;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "runtime-scene.png",
                                blessed.width, blessed.height, blessed.rgba));
    REQUIRE(blessed.Valid());
    CHECK(blessed.rgba[0] == 77);

    // And it now resolves as Shared.
    const auto after = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    CHECK(after.level == Arcane::ReferenceLevel::Shared);
}

TEST_CASE("reference: blessing a BACKEND-resolved image does not touch the shared one", "[reference]")
{
    const auto root = TempProject("bless-backend");
    WriteSolidPng(root / "Verify" / "References" / "editor-ui.png", 10);
    WriteSolidPng(root / "Verify" / "References" / "dx12" / "editor-ui.png", 20);

    const auto res = Arcane::ResolveReference(root, "editor-ui", "dx12");
    REQUIRE(res.level == Arcane::ReferenceLevel::Backend);

    std::vector<unsigned char> px(4 * 4 * 4, 99);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK(Arcane::BlessReference(res, 4, 4, px.data()));

    Arcane::PixelData shared;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "editor-ui.png",
                                shared.width, shared.height, shared.rgba));
    CHECK(shared.rgba[0] == 10);              // untouched

    // Hole B: a bless that silently no-ops (or writes the wrong file) would
    // still pass the "shared untouched" check above. Load the backend file
    // back and check the bless actually landed the new content there.
    Arcane::PixelData backend;
    REQUIRE(Arcane::LoadPngRgba(root / "Verify" / "References" / "dx12" / "editor-ui.png",
                                backend.width, backend.height, backend.rgba));
    REQUIRE(backend.Valid());
    CHECK(backend.rgba[0] == 99);
}

TEST_CASE("reference: diff artifacts land under Saved/, which is gitignored", "[reference]")
{
    const auto root = TempProject("diffpath");
    const auto p = Arcane::DiffArtifactPath(root, "runtime-scene", "vulkan");

    // Dist has no <project>/Saved (spec s11.0): the path is refused (empty),
    // never a relative one, and both hosts then write no diff artifact.
    CHECK(p == (Arcane::kDistBuild ? std::filesystem::path{}
                                   : root / "Saved" / "Verify" / "runtime-scene-vulkan-diff.png"));
}

TEST_CASE("reference: a name with a path separator is REFUSED, not resolved", "[reference]")
{
    // A reference name is a NAME. Accepting "../../etc/passwd" would let a
    // command line write outside the project.
    const auto root = TempProject("traversal");
    const auto res = Arcane::ResolveReference(root, "../escape", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());           // refused: nowhere safe to write
}

// --- Hole C: NameIsSafe's guard is a conjunction of independent checks. The
// case above trips the '/' AND the ".." checks at once ("../escape"), so
// deleting either check alone still leaves the name refused and the suite
// none the wiser. Each check below is pinned with a name that trips exactly
// one -- deleting that specific check (and only that one) is what turns each
// case from PASS to FAIL.
//
// NameIsSafe actually has FOUR checks past the empty-string guard: '/', '\',
// a ".." substring, and a leading '.'. The dispatch's audit named only three
// (it reads as an oversight, not a decision) and offered "..hidden" for the
// dots case -- but "..hidden" trips BOTH the ".." check and the leading-dot
// check at once (its execution stops at the first, ".."), so it does not
// isolate either. "sub..dir" isolates the ".." check (dots in the middle, no
// leading dot); ".hidden" isolates the leading-dot check on its own.

TEST_CASE("reference: a name containing a slash (no dots) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-slash");
    const auto res = Arcane::ResolveReference(root, "sub/dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name containing a backslash (no dots) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-backslash");
    const auto res = Arcane::ResolveReference(root, "sub\\dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name with a dot-pair in the middle (no separator, no leading dot) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-dotpair-mid");
    const auto res = Arcane::ResolveReference(root, "sub..dir", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a name starting with a dot (no separator, no dot-pair) is refused", "[reference]")
{
    const auto root = TempProject("unsafe-leading-dot");
    const auto res = Arcane::ResolveReference(root, ".hidden", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: a backend containing a path separator is refused, not just a name", "[reference]")
{
    // ResolveReference guards both arguments, but a name-only exercise never
    // notices a broken backend guard. Task 8 sources `backend` from the host
    // (a GraphicsBackend rendered to string), not a literal, so this is not
    // hypothetical.
    const auto root = TempProject("unsafe-backend");
    const auto res = Arcane::ResolveReference(root, "editor-ui", "sub/dir");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

// --- Review Finding 2 (task-7 fix pass): NameIsSafe's FIFTH branch,
// name.empty(), was still dark -- nothing above ever passed "" for either
// argument, and it is exactly the branch a grep-based audit is structurally
// blind to (grepping for '/', '\', ".." finds three substring/char checks;
// an empty-string guard and a front()-of-non-empty check don't match that
// pattern). Order matters here: NameIsSafe checks name.empty() FIRST, so
// name.front() is never reached on an empty string -- see the comment on
// that check in ReferenceImages.cpp. These two pin the branch on each
// argument independently, matching the shape of the four cases above.

TEST_CASE("reference: an empty name is refused", "[reference]")
{
    const auto root = TempProject("empty-name");
    const auto res = Arcane::ResolveReference(root, "", "dx12");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

TEST_CASE("reference: an empty backend is refused", "[reference]")
{
    const auto root = TempProject("empty-backend");
    const auto res = Arcane::ResolveReference(root, "editor-ui", "");

    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
}

// --- Hole A: DiffArtifactPath applied NameIsSafe to neither argument, so
// DiffArtifactPath(root, "../../evil", "dx12") built a path escaping the
// project -- the very path Task 8 writes a diff image to on a comparison
// failure. Fixed to guard both arguments and return an empty path on
// refusal; these two cases are the ones that would have caught it.

TEST_CASE("reference: DiffArtifactPath refuses an unsafe name and returns an empty path", "[reference]")
{
    const auto root = TempProject("diffpath-unsafe-name");
    const auto p = Arcane::DiffArtifactPath(root, "../../evil", "dx12");

    CHECK(p.empty());
}

TEST_CASE("reference: DiffArtifactPath refuses an unsafe backend and returns an empty path", "[reference]")
{
    const auto root = TempProject("diffpath-unsafe-backend");
    const auto p = Arcane::DiffArtifactPath(root, "runtime-scene", "../evil");

    CHECK(p.empty());
}

// --- Review Finding 1 (task-7 fix pass): BlessReference's own false-return
// path -- `if (resolution.blessTarget.empty() || rgba == nullptr) return
// false;` -- was exercised by zero tests. Every BlessReference call above is
// wrapped in CHECK(...) expecting true; nothing ever constructs a refused
// resolution and asserts false, and nothing ever passes rgba == nullptr.
// Inverting that condition, or deleting the early return outright, would
// leave the whole suite green -- the mirror image of Hole B, which this task
// exists to close.

TEST_CASE("reference: BlessReference refuses a resolution with no bless target", "[reference]")
{
    // A refused ResolveReference (traversal name) carries an empty
    // blessTarget; BlessReference must decline to write at all, not just
    // decline to know where.
    const auto root = TempProject("bless-refused");
    const auto refused = Arcane::ResolveReference(root, "../escape", "dx12");
    REQUIRE(refused.blessTarget.empty());

    std::vector<unsigned char> px(4 * 4 * 4, 42);
    for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    CHECK_FALSE(Arcane::BlessReference(refused, 4, 4, px.data()));

    // And nothing was written anywhere under the project root.
    CHECK_FALSE(fs::exists(root / "Verify"));
}

TEST_CASE("reference: BlessReference refuses a null pixel buffer", "[reference]")
{
    // An otherwise-valid, resolvable bless target -- only the pixel pointer
    // is bad.
    const auto root = TempProject("bless-null");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "dx12");
    REQUIRE_FALSE(res.blessTarget.empty());

    CHECK_FALSE(Arcane::BlessReference(res, 4, 4, nullptr));
    CHECK_FALSE(fs::exists(res.blessTarget));
}

TEST_CASE("reference resolution records the ordered search space it tried", "[host][reference]")
{
    // Layout: a temp project root with NO references at all.
    const auto root = std::filesystem::temp_directory_path() / "arc-triedpaths-none";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Verify" / "References");

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);
    // Both levels were tried, backend-specific first, in order.
    REQUIRE(res.triedPaths.size() == 2);
    REQUIRE(res.triedPaths[0].filename() == "runtime-scene.png");
    REQUIRE(res.triedPaths[0].parent_path().filename() == "vulkan");
    REQUIRE(res.triedPaths[1].parent_path().filename() == "References");
    std::filesystem::remove_all(root);
}

TEST_CASE("a resolved reference still records what was tried before it", "[host][reference]")
{
    const auto root = std::filesystem::temp_directory_path() / "arc-triedpaths-shared";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Verify" / "References");
    // Only the SHARED level exists; the backend level will be tried and missed first.
    std::ofstream(root / "Verify" / "References" / "runtime-scene.png") << "x";

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::Shared);
    REQUIRE(res.triedPaths.size() == 2);          // backend miss, then shared hit
    REQUIRE(res.triedPaths.back() == res.path);   // last tried == resolved
    std::filesystem::remove_all(root);
}

TEST_CASE("a refused reference name records an empty search space", "[host][reference]")
{
    const auto res = Arcane::ResolveReference(std::filesystem::temp_directory_path(),
                                              "../evil", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);
    REQUIRE(res.triedPaths.empty());   // refusal is not a search
}

// ---- the ADAPTER level (software rasterisers' own reference sets) ----------

TEST_CASE("reference: only a SOFTWARE adapter names a reference set", "[reference]")
{
    CHECK(Arcane::ReferenceAdapterSet("vulkan", "NVIDIA GeForce RTX 4090", false).empty());
    CHECK(Arcane::ReferenceAdapterSet("vulkan", "AMD Radeon RX 7900 XTX (RADV NAVI31)", false).empty());
    // Mesa's lavapipe: the LLVM version in its name is not part of the key.
    CHECK(Arcane::ReferenceAdapterSet("vulkan", "llvmpipe (LLVM 20.1.2, 256 bits)", true) == "vulkan-lavapipe");
    CHECK(Arcane::ReferenceAdapterSet("vulkan", "llvmpipe (LLVM 17.0.6, 128 bits)", true) == "vulkan-lavapipe");
    CHECK(Arcane::ReferenceAdapterSet("dx12", "Microsoft Basic Render Driver", true) == "dx12-warp");
    CHECK(Arcane::ReferenceAdapterSet("vulkan", "SwiftShader Device (Subzero)", true) == "vulkan-software");
    CHECK(Arcane::ReferenceNameIsSafe(Arcane::ReferenceAdapterSet("vulkan", "llvmpipe", true)));
}

TEST_CASE("reference: an adapter set's image WINS over the backend and shared images", "[reference]")
{
    const auto root = TempProject("adapter-wins");
    const auto refs = root / "Verify" / "References";
    WriteSolidPng(refs / "runtime-scene.png", 10);
    WriteSolidPng(refs / "vulkan" / "runtime-scene.png", 20);
    WriteSolidPng(refs / "vulkan-lavapipe" / "runtime-scene.png", 30);

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan", "vulkan-lavapipe");
    CHECK(res.level == Arcane::ReferenceLevel::Adapter);
    CHECK(res.path == refs / "vulkan-lavapipe" / "runtime-scene.png");
    CHECK(res.blessTarget == res.path);
    REQUIRE(res.triedPaths.size() == 1);

    // No set (a hardware adapter) never sees the adapter level at all.
    const auto hw = Arcane::ResolveReference(root, "runtime-scene", "vulkan", "");
    CHECK(hw.level == Arcane::ReferenceLevel::Backend);
    CHECK(hw.triedPaths.size() == 1);
}

TEST_CASE("reference: an adapter set falls through to compare, but blesses ONLY its own level",
          "[reference]")
{
    const auto root = TempProject("adapter-fallthrough");
    const auto refs = root / "Verify" / "References";
    WriteSolidPng(refs / "editor-ui.png", 10);
    WriteSolidPng(refs / "vulkan" / "runtime-scene.png", 20);

    // Backend image, no set image: compared against the backend image...
    const auto rt = Arcane::ResolveReference(root, "runtime-scene", "vulkan", "vulkan-lavapipe");
    CHECK(rt.level == Arcane::ReferenceLevel::Backend);
    CHECK(rt.path == refs / "vulkan" / "runtime-scene.png");
    // ...but a bless writes the set's own image, never the hardware one.
    CHECK(rt.blessTarget == refs / "vulkan-lavapipe" / "runtime-scene.png");
    REQUIRE(rt.triedPaths.size() == 2);
    CHECK(rt.triedPaths[0] == refs / "vulkan-lavapipe" / "runtime-scene.png");
    CHECK(rt.triedPaths[1] == refs / "vulkan" / "runtime-scene.png");

    // Shared image only: the same rule one level further down.
    const auto ui = Arcane::ResolveReference(root, "editor-ui", "vulkan", "vulkan-lavapipe");
    CHECK(ui.level == Arcane::ReferenceLevel::Shared);
    CHECK(ui.blessTarget == refs / "vulkan-lavapipe" / "editor-ui.png");
    CHECK(ui.triedPaths.size() == 3);

    // Nothing anywhere: a first bless on the adapter still lands in its set.
    const auto none = Arcane::ResolveReference(root, "fresh", "vulkan", "vulkan-lavapipe");
    CHECK(none.level == Arcane::ReferenceLevel::None);
    CHECK(none.blessTarget == refs / "vulkan-lavapipe" / "fresh.png");

    // Blessing through the fall-through resolution leaves the hardware image alone.
    std::vector<unsigned char> px(4 * 4 * 4, 99);
    REQUIRE(Arcane::BlessReference(rt, 4, 4, px.data()));
    Arcane::PixelData hwImage;
    REQUIRE(Arcane::LoadPngRgba(refs / "vulkan" / "runtime-scene.png",
                                hwImage.width, hwImage.height, hwImage.rgba));
    CHECK(hwImage.rgba[0] == 20);
    const auto after = Arcane::ResolveReference(root, "runtime-scene", "vulkan", "vulkan-lavapipe");
    CHECK(after.level == Arcane::ReferenceLevel::Adapter);
}

TEST_CASE("reference: an unsafe adapter set is refused like an unsafe name", "[reference]")
{
    const auto root = TempProject("adapter-unsafe");
    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan", "../escape");
    CHECK(res.level == Arcane::ReferenceLevel::None);
    CHECK(res.blessTarget.empty());
    CHECK(res.triedPaths.empty());
}

TEST_CASE("reference: ResolveReferenceIn applies the same levels under any references directory",
          "[reference]")
{
    const auto root = TempProject("resolve-in");
    const auto thumbs = root / "Verify" / "References" / "thumbs";
    WriteSolidPng(thumbs / "mesh-reference_cube.png", 10);
    const auto res = Arcane::ResolveReferenceIn(thumbs, "mesh-reference_cube", "dx12", "");
    CHECK(res.level == Arcane::ReferenceLevel::Shared);
    CHECK(res.path == thumbs / "mesh-reference_cube.png");
    const auto sw = Arcane::ResolveReferenceIn(thumbs, "mesh-reference_cube", "vulkan", "vulkan-lavapipe");
    CHECK(sw.level == Arcane::ReferenceLevel::Shared);
    CHECK(sw.blessTarget == thumbs / "vulkan-lavapipe" / "mesh-reference_cube.png");
}
