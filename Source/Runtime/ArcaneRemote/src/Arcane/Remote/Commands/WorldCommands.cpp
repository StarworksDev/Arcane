#include <Arcane/Remote/Commands/WorldCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Worlds.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// world.list takes no arguments; reflected (empty) for its schema (spec s5.3).
struct WorldListArgs {
};
ARC_REFLECT_TYPE(WorldListArgs)
ARC_REFLECT_TYPE_END()

namespace
{

constexpr std::string_view kUsage = "world.list takes no arguments; --world <name> picks one of its worlds";

CommandResult WorldListCommand(const Call& call, void* /*user*/)
{
    CommandResult result;
    // args must be an empty object (null reads as empty): world.list takes none.
    if (!call.args.is_null() && !call.args.is_object()) {
        result.envelope = MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                                    std::string(kUsage));
        return result;
    }
    if (call.args.is_object() && !call.args.empty()) {
        result.envelope = MakeError(call.command, "arg.unknown",
                                    call.command + " has no argument '" + call.args.begin().key() + "'",
                                    std::string(kUsage));
        return result;
    }
    const WorldTable table = CommandWorlds();
    if (table.fn == nullptr) {
        result.envelope = MakeError(call.command, "host.unavailable", "no armed host session serves world.list",
                                    "relaunch the host with --remote");
        return result;
    }
    if (!call.world.empty()) {
        PickedWorld picked = ResolveWorld(table, call.world);
        if (picked.pick != WorldPick::Ok) {
            result.envelope = std::move(picked.error);   // the dispatcher stamps call.command
            return result;
        }
    }
    result.envelope = WorldList(table);
    return result;
}

} // namespace

void RegisterWorldCommands()
{
    static bool registered = false;   // main thread only (HostSession::Start)
    if (registered)
        return;
    registered = true;

    Desc list;
    list.name = "world.list";
    list.summary = "List the host's worlds: name and role, sorted by name";
    list.access = Access::Observe;
    list.mutates = Mutates::None;
    list.play = PlayPolicy::Any;
    list.thread = ExecThread::Main;
    list.needsFrame = false;
    list.worldScoped = true;   // validates --world itself (ResolveWorld below)
    list.argsMeta = Astra::GetMeta<WorldListArgs>();
    list.fn = &WorldListCommand;
    std::string error;
    if (!CommandRegistry::Get().Register(std::move(list), &error))
        ARC_ERROR("remote: world command refused: {}", error);
}

} // namespace Arcane::Remote

#endif
