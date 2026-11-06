#pragma once
// The host descriptor (spec s4.4, AM5): one JSON file per host process at
// <project>/Saved/Remote/<hostKind>-<pid>.json (projectless editor: under
// EditorUserDir), written atomically, owner-only (Windows DACL: owner + SYSTEM;
// POSIX: 0600), heartbeat-refreshed, deleted on clean exit. It holds the
// bearer token, so nothing here logs a descriptor.
#include <Arcane/Modules/ArcaneRemoteProtocolApi.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

ARC_CONSTANT("spec s4.4: a heartbeat older than 10 s reports the host hung (under the 12 s hang watchdog)")
inline constexpr std::int64_t kHungAfterMs = 10000;

struct Descriptor {
    std::uint32_t protocol = kProtocol;
    HostKind hostKind = HostKind::Editor;
    std::uint32_t pid = 0;
    std::uint64_t processStart = 0;
    std::uint16_t port = 0;
    std::string token;
    std::string project;
    std::int64_t heartbeatUnixMs = 0;
    std::string state;
    std::uint64_t frame = 0;
    std::uint32_t abi = 0;  // kGamePluginABIVersion of the host that wrote it (ruling F27); HostSession::Start writes it
    std::uint32_t ownerPid = 0; // --owner-pid the host watches (0 = none); a missing key reads 0
    std::string client;     // --client NAME the host was launched with ("" = none); HostSession writes it, a missing key reads ""
    // Final fix pass (I1): the host was armed by an explicit --remote, which
    // servitor launch always passes. False for an editor armed by
    // editor.remote.enable alone (the one a user opens by hand). quit
    // --client's untagged --host fallback takes only a host with it set. A
    // missing key reads false.
    bool explicitRemote = false;
};

// <projectRoot>/Saved/Remote/<editor|runtime|server>-<pid>.json, through Paths.
ARC_REMOTE_PROTOCOL_API std::filesystem::path DescriptorPath(const std::filesystem::path& projectRoot, HostKind kind, std::uint32_t pid);
// <projectRoot>/Saved/Remote, or <EditorUserDir>/Remote when projectless.
ARC_REMOTE_PROTOCOL_API std::filesystem::path DescriptorDir(const std::filesystem::path& projectRoot, bool projectless);
// Writes path + ".tmp", makes it owner-only BEFORE the rename (AM5), renames it
// over `path`. Creates the parent directory. False (and no temp left) on failure.
ARC_REMOTE_PROTOCOL_API bool WriteDescriptorAtomic(const std::filesystem::path& path, const Descriptor& desc);
// Tolerant (rev 8.3 V6): unknown keys are ignored and any non-negative integer
// `protocol` is returned as written. nullopt only for an unreadable or
// malformed file (no object, or a missing/ill-typed protocol, hostKind, pid,
// port or token). Missing optional keys read as the struct defaults (abi 0).
ARC_REMOTE_PROTOCOL_API std::optional<Descriptor> ReadDescriptor(const std::filesystem::path& path);
// Re-reads the descriptor and rewrites it atomically with a fresh heartbeat,
// state and frame. A missing or unreadable descriptor is left alone.
ARC_REMOTE_PROTOCOL_API void TouchHeartbeat(const std::filesystem::path& path, std::int64_t unixMs, std::string state, std::uint64_t frame);
// nowUnixMs - heartbeatUnixMs > kHungAfterMs. A hung descriptor is reported, not deleted.
ARC_REMOTE_PROTOCOL_API bool IsHung(const Descriptor& desc, std::int64_t nowUnixMs);
// !Platform::IsProcessLive(pid, processStart): the process is gone or its pid was recycled.
ARC_REMOTE_PROTOCOL_API bool IsStale(const Descriptor& desc);

} // namespace Arcane::Remote

#else
// Dist builds compile the remote surface out; this empty branch keeps the include legal.
#endif
