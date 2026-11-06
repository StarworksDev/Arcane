#pragma once
// capture.compare (spec s7.4; Servitor P0 Task 28): the host side of
// `servitor compare <reference> [--source host|viewport|window] [--bless]
// [--max-diff-pixels N] [--max-diff-pixel-ratio R]`. One render-ticket job on
// Task 27's capture step (the same hook pairs, the same one-capture-in-flight
// gate), then CompareImages against the resolved reference and, with --bless,
// the host's bless hook -- both only on the drain that returns the retired
// ticket's bytes. ArcaneRemote never depends on ArcaneVerify (R8-2): reference
// resolution and blessing are the host program's ResolveFn / BlessFn, which
// call ReferenceImages' ResolveReference / BlessReference.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Arcane/Remote/Registry.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class JobTable; // Jobs.hpp

struct ResolvedReference {
    bool found = false;                   // false: no reference file at any level
    std::filesystem::path path;           // the PNG the verdict compares against
    std::string level;                    // the resolved verdict level (ReferenceResolution's level name)
    std::vector<std::string> triedPaths;  // every path the resolver looked at, in order; the envelope's triedPaths
};
// Main thread. Resolve: where `slot` resolves for this host (backend, adapter
// set). Bless: write tight RGBA8 to the reference `slot` resolves to (the
// host owns the write); false = nothing was written.
using ResolveFn = ResolvedReference (*)(const std::filesystem::path& projectRoot, const std::string& slot,
                                        void* user);
using BlessFn = bool (*)(const std::filesystem::path& projectRoot, const std::string& slot,
                         std::uint32_t width, std::uint32_t height, const unsigned char* rgba, void* user);
struct CompareHooks {
    ResolveFn resolve = nullptr;
    BlessFn bless = nullptr;
    std::filesystem::path projectRoot;
    void* user = nullptr;
};
// Both are safe to call twice; a later call replaces the stored hooks / pointer
// (ruling F11). A null JobTable*, or a null resolve or bless, makes
// capture.compare answer host.unavailable (exit 5) after the capture.no-renderer check.
// (A null JobTable* -- a torn-down session -- answers host.unavailable before
// that check, as capture.* does since Task 27's review.)
// RegisterCompareCommand(nullptr) also finishes every compare job still
// running in the previous table (Cancelled, host.unavailable) and frees its
// state, so no job outlives the hooks it would call.
//
// capture.compare's wire check on `reference` (final fix pass, G-3): the same
// rule as ArcaneVerify's ReferenceNameIsSafe, kept as an identical copy here
// because ArcaneRemote never depends on ArcaneVerify (R8-2); a test pins the
// two to the same answers. Non-empty, no slash or backslash, no ':', no "..",
// no leading '.'.
ARC_REMOTE_API bool CompareReferenceNameIsSafe(const std::string& name);
ARC_REMOTE_API void SetCompareHooks(CompareHooks hooks);
ARC_REMOTE_API void RegisterCompareCommand(JobTable* jobs);

} // namespace Arcane::Remote

#endif
