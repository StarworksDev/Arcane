#pragma once
// The host's worlds (spec s7.1 `world.list`, s4.6 `--world`; Servitor P0 Task
// 19, rulings F14 and F16). A host installs one WorldTable through
// HostSession::Config::worlds; every world-taking command resolves its target
// through ResolveWorld(CommandWorlds(), call.world), the one world path.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Astra
{
class Registry;
}

namespace Arcane::Remote
{

struct WorldInfo {
    std::string name;
    std::string role;                    // ToString(Runtime::Mode())
    Astra::Registry* registry = nullptr; // not serialized
};

// Main thread: called inside Drain (commands) and by HostSession::Start.
using WorldListFn = std::vector<WorldInfo> (*)(void* user);

struct WorldTable {
    WorldListFn fn = nullptr;
    void* user = nullptr;
};

// data.worlds = [{name, role}] sorted by name. registry pointers are not in JSON.
// A null fn lists no worlds (ok, empty array).
ARC_REMOTE_API Envelope WorldList(const WorldTable& table);

enum class WorldPick { Ok, Missing, Unknown, None };
struct PickedWorld {
    WorldPick pick = WorldPick::Missing;
    WorldInfo world;
    Envelope error;
};

// requested empty and one world -> that world.
// requested empty and several -> arg.missing exit 2, error.data.worlds lists names.
// requested names one -> that world.
// requested names none -> arg.unknown exit 2, candidates are the names.
// table.fn null, or zero worlds -> WorldPick::None, host.unavailable exit 5 (nothing ran; ruling S3-9).
ARC_REMOTE_API PickedWorld ResolveWorld(const WorldTable& table, const std::string& requested);

// The ONE world table every world-taking command reads (ruling F16). Copies the
// table. Safe to call twice; a later call replaces the stored table (ruling F11).
// HostSession::Start installs config.worlds; Stop installs an empty table.
ARC_REMOTE_API void SetCommandWorlds(const WorldTable& table);
ARC_REMOTE_API WorldTable CommandWorlds();   // a copy of the stored table

} // namespace Arcane::Remote

#endif
