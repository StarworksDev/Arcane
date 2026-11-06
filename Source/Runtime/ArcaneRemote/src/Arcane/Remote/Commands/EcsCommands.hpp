#pragma once
// entity.get, ecs.query, ecs.stats and ecs.archetypes (spec s7.2; Servitor P0
// Task 23): the read-only host side of `servitor entity get` and `servitor ecs
// ...`. Every command resolves its registry through ResolveWorld(CommandWorlds(),
// call.world) (ruling F16), so the commands hold no world pointer; HostSession::
// Start registers them and points the artifact directory at <remote dir>/out.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

// The text after the last "::", or the whole string.
ARC_REMOTE_API std::string ShortName(std::string_view qualified);

enum class NamePick : std::uint8_t { One, None, Many };
struct NameResolution {
    NamePick pick = NamePick::None;
    std::string qualified;
    std::vector<std::string> candidates; // sorted qualified names
};

// A qualified name equal to `requested`, or the one qualified name whose
// ShortName equals it, is One. Two or more short-name hits are Many (candidates
// lists them, sorted). Zero is None. Shared with schema.type (CatalogCommands),
// so a short component name works there too.
ARC_REMOTE_API NameResolution PickComponent(const std::vector<std::string>& qualifiedNames, std::string_view requested);

// Registers, once per process (a second call returns), four Observe host
// commands that run on the main thread with needsFrame false and resolve --world:
//   entity.get {entity, components}  -> data {entity:{guid:{hi,lo}, ref:"guid:<id>", name},
//                                       components:{Short: {...}}}; an entity without
//                                       Identity is {guid:null, name:null, handle} plus
//                                       warning entity.no-identity
//   ecs.query {with, without, fields, limit, strict} -> data {count, truncated,
//                                       rows:[{guid, ref, name, components}]} sorted by
//                                       guid.hi, guid.lo, name (no-Identity rows last)
//   ecs.stats {}       -> data {entityCount, archetypeCount, chunkCount, bytesUsed}
//   ecs.archetypes {}  -> data {archetypes:[{signature, entityCount, chunkCount}]}
ARC_REMOTE_API void RegisterEcsCommands();

// Where ecs.query writes rows that exceed the inline cap (one `kind:"ecs"`
// artifact), and resource.get a value that does (`kind:"resource"`, Task 24;
// the directory lives in CommandSupport). Empty: no artifact; the rows are
// dropped with warning ecs.no-artifact-dir. Safe to call twice; a later call
// replaces the directory.
ARC_REMOTE_API void SetEcsArtifactDir(const std::filesystem::path& dir);
// Clears the directory only when it is still `dir` (a later session's is left alone).
ARC_REMOTE_API void ReleaseEcsArtifactDir(const std::filesystem::path& dir);

} // namespace Arcane::Remote

#endif
