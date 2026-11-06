#include <Arcane/Remote/Worlds.hpp>

#if !defined(ARC_BUILD_DIST)

#include <algorithm>
#include <mutex>
#include <utility>

namespace Arcane::Remote
{

namespace
{

struct CommandWorldState {
    std::mutex mutex;
    WorldTable table;
};

CommandWorldState& State()
{
    static CommandWorldState state;
    return state;
}

// The table's worlds sorted by name; none when fn is null.
std::vector<WorldInfo> SortedWorlds(const WorldTable& table)
{
    std::vector<WorldInfo> worlds;
    if (table.fn != nullptr)
        worlds = table.fn(table.user);
    std::sort(worlds.begin(), worlds.end(),
              [](const WorldInfo& a, const WorldInfo& b) { return a.name < b.name; });
    return worlds;
}

nlohmann::json Names(const std::vector<WorldInfo>& worlds)
{
    nlohmann::json names = nlohmann::json::array();
    for (const WorldInfo& w : worlds)
        names.push_back(w.name);
    return names;
}

std::string NamesText(const std::vector<WorldInfo>& worlds)
{
    std::string text;
    for (const WorldInfo& w : worlds) {
        if (!text.empty())
            text += ", ";
        text += w.name;
    }
    return text;
}

} // namespace

Envelope WorldList(const WorldTable& table)
{
    nlohmann::json rows = nlohmann::json::array();
    for (const WorldInfo& w : SortedWorlds(table))
        rows.push_back({{"name", w.name}, {"role", w.role}});
    return MakeOk("world.list", {{"worlds", std::move(rows)}}, {});
}

PickedWorld ResolveWorld(const WorldTable& table, const std::string& requested)
{
    PickedWorld picked;
    const std::vector<WorldInfo> worlds = SortedWorlds(table);
    if (worlds.empty()) {
        picked.pick = WorldPick::None;
        picked.error = MakeError({}, "host.unavailable", "this host serves no world",
                                 "relaunch the host with --remote");
        return picked;
    }

    if (requested.empty()) {
        if (worlds.size() == 1) {
            picked.pick = WorldPick::Ok;
            picked.world = worlds.front();
            return picked;
        }
        picked.pick = WorldPick::Missing;
        picked.error = MakeError({}, "arg.missing", "this host runs several worlds; name one",
                                 "pass --world <name>: " + NamesText(worlds));
        picked.error.data["worlds"] = Names(worlds);
        return picked;
    }

    for (const WorldInfo& w : worlds) {
        if (w.name == requested) {
            picked.pick = WorldPick::Ok;
            picked.world = w;
            return picked;
        }
    }
    picked.pick = WorldPick::Unknown;
    picked.error = MakeError({}, "arg.unknown", "this host has no world named '" + requested + "'",
                             "pass --world <name>: " + NamesText(worlds));
    for (const WorldInfo& w : worlds)
        picked.error.errors.front().candidates.push_back(w.name);
    picked.error.errors.front().detail["world"] = requested;
    return picked;
}

void SetCommandWorlds(const WorldTable& table)
{
    CommandWorldState& state = State();
    std::lock_guard lock(state.mutex);
    state.table = table;
}

WorldTable CommandWorlds()
{
    CommandWorldState& state = State();
    std::lock_guard lock(state.mutex);
    return state.table;
}

} // namespace Arcane::Remote

#endif
